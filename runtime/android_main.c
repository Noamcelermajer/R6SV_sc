/* Android front end: a NativeActivity (no Java code) that runs the recompiled game.
 *
 * image.bin comes from the APK's assets. The game data (the OBB contents, extracted) is read from the app's
 * external files folder, /sdcard/Android/data/<package>/files, which needs no storage permission; copy it there
 * with `adb push`. The game's own /data/data/com.gameloft... folder maps to the app's internal data folder.
 * Touches are forwarded per pointer, so several fingers work (move and aim at once).
 */
#include <EGL/egl.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <stdio.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rt.h"

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLSurface surf = EGL_NO_SURFACE;
static EGLContext ctx = EGL_NO_CONTEXT;
static EGLConfig cfg;
static int width, height, booted, paused;
/* The game lays itself out for the resolutions it knows: 1280x720, 1440x810, 1920x1080 and 1280 wide at any aspect
 * render correctly, while 1600 or 2418 wide (this phone's 2418x1080) draw the title art and the controls' zones at a
 * fraction of the screen. So the window's buffers are 1280 wide with the screen's aspect ratio (render_width.txt in
 * the data folder overrides the width), Android's scaler stretches them to the display, and touches are scaled into
 * the same space. */
static int render_width = 1280;
static float touch_sx = 1, touch_sy = 1;
static void *gles;

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

static int egl_surface(struct android_app *app) {
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(dpy, NULL, NULL);
        const EGLint attr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                               EGL_STENCIL_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        EGLint n;
        if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) {
            const EGLint fallback[] = {EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_DEPTH_SIZE, 16,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
            if (!eglChooseConfig(dpy, fallback, &cfg, 1, &n) || n < 1) rt_fatal("no EGL config");
        }
        const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    }
    int sw = ANativeWindow_getWidth(app->window), sh = ANativeWindow_getHeight(app->window);
    if (sw > 0 && sh > 0) {
        int rw = sw > render_width ? render_width : sw, rh = (int)((long)rw * sh / sw);
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
    if (fscanf(f, "%d", &w) == 1 && w >= 320 && w <= 8192) render_width = w;
    fclose(f);
}

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
    snprintf(probe, sizeof probe, "%s/sprites.gla", data);
    if (stat(probe, &st)) rt_log("game data is missing: copy the extracted OBB contents to %s", data);
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
    case APP_CMD_PAUSE:
        if (booted && !paused) { rt_game_pause(); paused = 1; }
        break;
    case APP_CMD_RESUME:
        if (booted && paused) { rt_game_resume(); paused = 0; }
        break;
    default:
        break;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *e) {
    (void)app;
    if (!booted || AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 0;
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

void android_main(struct android_app *app) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    read_render_width(app);
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    for (;;) {
        int events;
        struct android_poll_source *source;
        while (ALooper_pollOnce(booted && surf != EGL_NO_SURFACE && !paused ? 0 : -1, NULL, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                rt_log("activity destroyed");
                exit(0); /* the game keeps global state: start fresh next time */
            }
        }
        if (booted && surf != EGL_NO_SURFACE && !paused) {
            rt_game_step();
            eglSwapBuffers(dpy, surf);
        }
    }
}
