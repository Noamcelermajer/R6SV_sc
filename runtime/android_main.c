/* Android front end: a NativeActivity (no Java code) that runs the recompiled Rainbow Six: Shadow Vanguard.
 *
 * image.bin comes from the APK's assets. The game data (the content of /sdcard/gameloft/games/GloftR6HP: data/,
 * SaveGame.bin, ...) is read from the app's external files folder, /sdcard/Android/data/com.r6sv.recomp/files, which
 * needs no storage permission; copy it there with `adb push` (docs/notes/frontends.md). The game's hard-coded folder
 * /sdcard/gameloft/games/GloftR6HP maps to that folder (rt_frontend_paths) and its /data/data/<package> folder to
 * the app's internal data folder.
 *
 * Input: AInputQueue events (through native_app_glue) are forwarded per pointer to rt_game_touch (action codes of
 * Android's MotionEvent: 0 down, 1 up, 2 move; the game layer translates them for the game); the accelerometer is
 * read through ASensorManager and sent to rt_game_accel; the back key to rt_game_key. Pause and resume call
 * rt_game_pause / rt_game_resume.
 */
#include <EGL/egl.h>
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "frontend.h"
#include "rt.h"

/* weak defaults until game.c provides these entry points */
__attribute__((weak)) void rt_game_accel(float x, float y, float z) { (void)x; (void)y; (void)z; }
__attribute__((weak)) void rt_game_key(int keycode, int down) { (void)keycode; (void)down; }

#define LOOPER_ID_SENSOR 3

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLSurface surf = EGL_NO_SURFACE;
static EGLContext ctx = EGL_NO_CONTEXT;
static EGLConfig cfg;
static int width, height, booted, paused;
/* The menus pick their layout from the viewport width (docs/DATA.md section 4): the window's buffers are
 * RT_RENDER_WIDTH wide with the screen's aspect ratio (render_width.txt in the data folder overrides the width),
 * Android's scaler stretches them to the display, and touches are scaled into the same space. */
static int render_width = RT_RENDER_WIDTH;
/* Default: the buffer is exactly RT_RENDER_WIDTH x RT_DEFAULT_HEIGHT (854x480), the size the host runner was verified
 * with; the display scaler stretches it. render_width.txt in the data folder switches to "that width, the screen's
 * aspect ratio" (rt_render_size), which keeps pixels square but gives the game a viewport that is not 16:9. */
static int keep_aspect;
static float touch_sx = 1, touch_sy = 1;
static void *gles;
static ASensorManager *sensors;
static const ASensor *accel_sensor;
static ASensorEventQueue *sensor_queue;

static void *getproc(const char *name) {
    void *p = dlsym(gles, name);
    return p ? p : (void *)eglGetProcAddress(name);
}

/* ---- crash reports: a fault inside game memory names the game address and the game call chain ------------ */
static void on_signal(int sig, siginfo_t *info, void *uc) {
    (void)uc;
    uintptr_t a = (uintptr_t)info->si_addr;
    if (rt_mem && a >= (uintptr_t)rt_mem && a < (uintptr_t)rt_mem + (1ull << 32))
        rt_log("crash: signal %d at game address %#llx", sig, (unsigned long long)(a - (uintptr_t)rt_mem));
    else
        rt_log("crash: signal %d at host address %p", sig, (void *)a);
    rt_print_trace(rt_game_cpu());
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- EGL ------------------------------------------------------------------------------------------------- */
static int egl_surface(struct android_app *app) {
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(dpy, NULL, NULL);
        const EGLint renderable = RT_EGL_CONTEXT_VERSION >= 3 ? EGL_OPENGL_ES3_BIT : RT_EGL_CONTEXT_VERSION == 2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES_BIT;
        const EGLint attr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                               EGL_STENCIL_SIZE, 8, EGL_RENDERABLE_TYPE, renderable, EGL_NONE};
        EGLint n;
        if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) {
            const EGLint fallback[] = {EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_DEPTH_SIZE, 16,
                                       EGL_RENDERABLE_TYPE, renderable, EGL_NONE};
            if (!eglChooseConfig(dpy, fallback, &cfg, 1, &n) || n < 1) rt_fatal("no EGL config");
        }
        const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, RT_EGL_CONTEXT_VERSION, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
        if (ctx == EGL_NO_CONTEXT) rt_fatal("eglCreateContext (ES %d) failed: %#x", RT_EGL_CONTEXT_VERSION, eglGetError());
    }
    int sw = ANativeWindow_getWidth(app->window), sh = ANativeWindow_getHeight(app->window);
    if (sw > 0 && sh > 0) {
        int rw = render_width, rh = RT_DEFAULT_HEIGHT;
        if (keep_aspect) rt_render_size(sw, sh, render_width, &rw, &rh);
        else if (sw < rw) rt_render_size(sw, sh, rw, &rw, &rh);
        ANativeWindow_setBuffersGeometry(app->window, rw, rh, 0);
        touch_sx = (float)rw / sw;
        touch_sy = (float)rh / sh;
        rt_log("screen %dx%d, rendering at %dx%d", sw, sh, rw, rh);
    }
    surf = eglCreateWindowSurface(dpy, cfg, app->window, NULL);
    if (surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        rt_log("EGL surface failed (%#x)", eglGetError());
        return 0;
    }
    eglQuerySurface(dpy, surf, EGL_WIDTH, &width);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &height);
    eglSwapInterval(dpy, 1);
    return 1;
}

static void read_render_width(struct android_app *app) {
    char path[600];
    snprintf(path, sizeof path, "%s/render_width.txt", app->activity->externalDataPath);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int w = 0;
    if (fscanf(f, "%d", &w) == 1 && w >= 320 && w <= 8192) { render_width = w; keep_aspect = 1; }
    fclose(f);
}

/* ---- immersive mode: hide the system bars (a JNI call into the activity; failures are only logged) ------- */
static void hide_system_ui(struct android_app *app) {
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    jobject activity = app->activity->clazz;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID get_window = (*env)->GetMethodID(env, cls, "getWindow", "()Landroid/view/Window;");
    jobject window = get_window ? (*env)->CallObjectMethod(env, activity, get_window) : NULL;
    if (window) {
        jclass wcls = (*env)->GetObjectClass(env, window);
        jmethodID get_decor = (*env)->GetMethodID(env, wcls, "getDecorView", "()Landroid/view/View;");
        jobject decor = get_decor ? (*env)->CallObjectMethod(env, window, get_decor) : NULL;
        if (decor) {
            jclass vcls = (*env)->GetObjectClass(env, decor);
            jmethodID set_flags = (*env)->GetMethodID(env, vcls, "setSystemUiVisibility", "(I)V");
            /* LAYOUT_STABLE | HIDE_NAVIGATION | FULLSCREEN | LAYOUT_HIDE_NAVIGATION | LAYOUT_FULLSCREEN | IMMERSIVE_STICKY */
            if (set_flags) (*env)->CallVoidMethod(env, decor, set_flags, 0x100 | 0x2 | 0x4 | 0x200 | 0x400 | 0x1000);
        }
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        rt_log("hiding the system bars failed (non-fatal)");
    }
    (*vm)->DetachCurrentThread(vm);
}

/* ---- accelerometer ----------------------------------------------------------------------------------------- */
static void sensor_start(struct android_app *app) {
    if (!sensors) sensors = ASensorManager_getInstanceForPackage(RT_APP_PACKAGE);
    if (!sensors) return;
    if (!accel_sensor) accel_sensor = ASensorManager_getDefaultSensor(sensors, ASENSOR_TYPE_ACCELEROMETER);
    if (!accel_sensor) return;
    if (!sensor_queue) sensor_queue = ASensorManager_createEventQueue(sensors, app->looper, LOOPER_ID_SENSOR, NULL, NULL);
    if (!sensor_queue) return;
    ASensorEventQueue_enableSensor(sensor_queue, accel_sensor);
    ASensorEventQueue_setEventRate(sensor_queue, accel_sensor, 1000000 / 60);
}

static void sensor_stop(void) {
    if (sensor_queue && accel_sensor) ASensorEventQueue_disableSensor(sensor_queue, accel_sensor);
}

static void sensor_drain(void) {
    ASensorEvent ev;
    while (sensor_queue && ASensorEventQueue_getEvents(sensor_queue, &ev, 1) > 0)
        if (ev.type == ASENSOR_TYPE_ACCELEROMETER && booted && !paused) rt_game_accel(ev.acceleration.x, ev.acceleration.y, ev.acceleration.z);
}

/* ---- boot --------------------------------------------------------------------------------------------------- */
static void boot(struct android_app *app) {
    AAsset *a = AAssetManager_open(app->activity->assetManager, "image.bin", AASSET_MODE_BUFFER);
    if (!a) rt_fatal("image.bin missing from the APK assets");
    rt_mem_init_from(AAsset_getBuffer(a), (size_t)AAsset_getLength(a));
    AAsset_close(a);
    gles = dlopen("libGLESv2.so", RTLD_NOW);
    rt_gl_getproc = getproc;

    char data[512], home[512], temp[512];
    snprintf(data, sizeof data, "%s", app->activity->externalDataPath);
    snprintf(home, sizeof home, "%s", app->activity->internalDataPath);
    snprintf(temp, sizeof temp, "%s/tmp", app->activity->internalDataPath);
    mkdir(data, 0770);
    mkdir(home, 0770);
    mkdir(temp, 0770);
    rt_log("game data folder: %s", data);
    struct stat st;
    char probe[600];
    snprintf(probe, sizeof probe, "%s/data/Res.array", data);
    if (stat(probe, &st)) rt_log("stat %s: %s", probe, strerror(errno));
    if (stat(probe, &st)) rt_log("game data is missing: copy the content of GloftR6HP to %s (adb push GloftR6HP/. %s/)", data, data);
    rt_frontend_paths(data, home);
    rt_game_boot(data, data, home, temp, width, height);
    booted = 1;
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window && egl_surface(app)) {
            if (!booted) boot(app);
            else rt_game_resize(width, height);
        }
        break;
    case APP_CMD_TERM_WINDOW:
        if (surf != EGL_NO_SURFACE) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(dpy, surf);
            surf = EGL_NO_SURFACE;
        }
        break;
    case APP_CMD_GAINED_FOCUS:
        hide_system_ui(app);
        break;
    case APP_CMD_PAUSE:
        sensor_stop();
        if (booted && !paused) { rt_game_pause(); paused = 1; }
        break;
    case APP_CMD_RESUME:
        sensor_start(app);
        if (booted && paused) { rt_game_resume(); paused = 0; }
        break;
    default:
        break;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *e) {
    (void)app;
    if (!booted) return 0;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e);
        /* as Game.onKeyDown/onKeyUp: everything goes to the game except the volume and camera keys, which the
         * system keeps (the back key included: the game decides what it does, Game.Exit() finishes the activity) */
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE || code == AKEYCODE_CAMERA ||
            (act != AKEY_EVENT_ACTION_DOWN && act != AKEY_EVENT_ACTION_UP))
            return 0;
        rt_game_key(code, act == AKEY_EVENT_ACTION_DOWN);
        return 1;
    }
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 0;
    int32_t action = AMotionEvent_getAction(e), kind = action & AMOTION_EVENT_ACTION_MASK;
    size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    size_t count = AMotionEvent_getPointerCount(e);
    switch (kind) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP: {
        int down = kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN;
        rt_game_touch(down ? 0 : 1, (int)(AMotionEvent_getX(e, index) * touch_sx),
                      (int)(AMotionEvent_getY(e, index) * touch_sy), AMotionEvent_getPointerId(e, index));
        break;
    }
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t k = 0; k < count; k++)
            rt_game_touch(2, (int)(AMotionEvent_getX(e, k) * touch_sx), (int)(AMotionEvent_getY(e, k) * touch_sy),
                          AMotionEvent_getPointerId(e, k));
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (size_t k = 0; k < count; k++)
            rt_game_touch(1, (int)(AMotionEvent_getX(e, k) * touch_sx), (int)(AMotionEvent_getY(e, k) * touch_sy),
                          AMotionEvent_getPointerId(e, k));
        break;
    default:
        return 0;
    }
    return 1;
}

static void main_loop(void *arg);

/* The translated code recurses on the host stack (the Windows runner links with a 32 MB stack, guest threads get
 * 8 MB), but native_app_glue's thread has Bionic's default 1 MB. The looper belongs to this thread, so instead of
 * starting another thread the loop runs on a 64 MB mapping that the stack pointer is switched to. */
#define MAIN_STACK (64u << 20)
void android_main(struct android_app *app) {
    char *stack = mmap(NULL, MAIN_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack == MAP_FAILED) {
        rt_log("cannot map a %u MB stack, running on the thread's own", MAIN_STACK >> 20);
        main_loop(app);
        return;
    }
    mprotect(stack, 4096, PROT_NONE); /* guard page */
    uintptr_t top = ((uintptr_t)stack + MAIN_STACK) & ~(uintptr_t)15;
    __asm__ volatile("mov x19, sp\n\t"
                     "mov sp, %0\n\t"
                     "mov x0, %2\n\t"
                     "blr %1\n\t"
                     "mov sp, x19"
                     :
                     : "r"(top), "r"(main_loop), "r"(app)
                     : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
                       "x17", "x19", "x30", "cc", "memory");
}

static void main_loop(void *arg) {
    struct android_app *app = arg;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    read_render_width(app);
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    for (;;) {
        int events, ident;
        struct android_poll_source *source;
        while ((ident = ALooper_pollOnce(booted && surf != EGL_NO_SURFACE && !paused ? 0 : -1, NULL, &events, (void **)&source)) >= 0) {
            if (ident == LOOPER_ID_SENSOR) sensor_drain();
            else if (source) source->process(app, source);
            if (app->destroyRequested) {
                rt_log("activity destroyed");
                exit(0); /* the game keeps global state: start fresh next time */
            }
        }
        if (booted && surf != EGL_NO_SURFACE && !paused) {
            rt_game_step();
            eglSwapBuffers(dpy, surf);
            if (rt_game_exit_requested() && !app->destroyRequested) {
                rt_log("Game.Exit() called, finishing the activity");
                ANativeActivity_finish(app->activity);
                paused = 1; /* stop stepping until the destroy command arrives */
            }
        }
    }
}
