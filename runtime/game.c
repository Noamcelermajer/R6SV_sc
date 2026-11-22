/* Driving the game the way its Java side does (docs/JAVA-SIDE.md, sections 3 and 9.5): constructors, JNI_OnLoad,
 * the one-time natives (Device and GLResLoader on the UI thread, GameRenderer.nativeInit, GLMediaPlayer, Game.nativeInit),
 * GameRenderer.nativeStart(1, w, h, lang) which runs appInit, then nativeRender per frame, touch and key events and
 * pause/resume. Shared by the Windows runner and the Android activity; the platform layer owns the window, the GL
 * context and the input, and calls these on the GL thread. Input may come from any thread: it is queued and
 * delivered on the render thread at the start of the next rt_game_step. */
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"

#define PKG "Java_com_gameloft_android_ANMP_GloftR6HP_"
#define CLS "com/gameloft/android/ANMP/GloftR6HP/"

static cpu_t *boot_cpu;
static uint32_t env;
static uint32_t cls_game, cls_renderer, obj_renderer, obj_view, obj_game;
static volatile int started;  /* GameRenderer.nativeStart has returned */
static volatile int paused;
static int lang;

static void check(cpu_t *c, const char *what) {
    if (c->fault) {
        rt_log("%s: translated code faulted (fault %u, r15 %#x)", what, c->fault, c->r[15]);
        rt_print_trace(c);
        rt_fatal("stopping after a fault");
    }
}

/* Call a Java_* native: (env, thiz-or-class, args...) on the calling thread's CPU state */
static uint32_t native(const char *name, uint32_t thiz, int nargs, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    cpu_t *c = rt_thread_cpu();
    char sym[256];
    snprintf(sym, sizeof sym, PKG "%s", name);
    if (rt_verbose) rt_log("-> %s", name);
    uint32_t r = rt_call(c, rt_export(sym), 2 + nargs, env, thiz, a0, a1, a2, a3);
    check(c, name);
    return r;
}

static void run_constructors(cpu_t *c) {
    rt_log("running %u constructors", rt_init_array_count);
    for (uint32_t k = 0; k < rt_init_array_count; k++) {
        rt_call(c, rt_init_array[k], 0);
        if (c->fault) rt_fatal("constructor %u at %#x faulted (fault %u, r15 %#x)", k, rt_init_array[k], c->fault, c->r[15]);
    }
}

/* GameRenderer.onSurfaceCreated: steps 2-6 of JAVA-SIDE.md 3.2. Runs again whenever the GL context is recreated. */
static void surface_created(int width, int height) {
    native("GameRenderer_nativeInit", obj_renderer, 0, 0, 0, 0, 0);       /* stores env in g_env: before Game.nativeInit */
    native("GLResLoader_nativeInit", rt_jni_class(CLS "GLResLoader"), 0, 0, 0, 0, 0);
    uint32_t sounds = native("GLMediaPlayer_nativeGetTotalSounds", rt_jni_class(CLS "GLMediaPlayer"), 0, 0, 0, 0, 0);
    uint32_t same = native("GLMediaPlayer_nativeGetTotalSoundsOfSameInstance", rt_jni_class(CLS "GLMediaPlayer"), 0, 0, 0, 0, 0);
    if (sounds != 635 || same != 2) rt_log("warning: GLMediaPlayer totals %u/%u, expected 635/2", sounds, same);
    native("GLMediaPlayer_nativeInit", rt_jni_class(CLS "GLMediaPlayer"), 0, 0, 0, 0, 0);
    native("Game_nativeInit", cls_game, 0, 0, 0, 0, 0);                    /* caches 21 statics, db() once */

    /* nativeStart runs appInit the first time; the larger side first, as GameRenderer.b/c are in Java */
    if (width < height) { int t = width; width = height; height = t; }
    lang = rt_jni_game_language();
    native("GameRenderer_nativeStart", cls_renderer, 4, 1, (uint32_t)width, (uint32_t)height, (uint32_t)lang);
}

/* Game memory must be loaded (rt_mem_init*) and rt_gl_getproc set, with the GL context current. `obb` is unused
 * (no expansion file); `data` is the folder with the game data, `home` the app's private folder, `temp` a scratch
 * folder. width and height are the size the game renders at (854x480 by design, see frontend.h). */
void rt_game_boot(const char *obb, const char *data, const char *home, const char *temp, int width, int height) {
    rt_data_imports_init();
    boot_cpu = rt_thread_cpu();
    rt_imports_init();
    uint32_t vm = rt_jni_init();
    env = rt_jni_env();
    rt_jni_set_screen(width, height);
    rt_jni_set_paths(obb, data, home, temp);

    cls_game = rt_jni_class(CLS "Game");
    cls_renderer = rt_jni_class(CLS "GameRenderer");
    obj_renderer = rt_jni_object(CLS "GameRenderer");
    obj_view = rt_jni_object(CLS "GameGLSurfaceView");
    obj_game = rt_jni_object(CLS "Game");

    /* The game's CPU-features code reads /proc/cpuinfo, and vox picks its NEON assembly mixers and decoders when it
     * finds "neon". That assembly is not translated, so the runtime describes an ARMv7 CPU with VFP and no NEON: vox
     * then uses its C++ mixers and decoders, which are. */
    if (temp) {
        char cpuinfo[600];
        snprintf(cpuinfo, sizeof cpuinfo, "%s/cpuinfo", temp);
        FILE *f = fopen(cpuinfo, "w");
        if (f) {
            fputs("Processor\t: ARMv7 Processor rev 0 (v7l)\nprocessor\t: 0\nprocessor\t: 1\nprocessor\t: 2\nprocessor\t: 3\n"
                  "Features\t: swp half thumb fastmult vfp edsp vfpv3 vfpv3d16 tls\nCPU implementer\t: 0x41\n"
                  "CPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xc0f\nCPU revision\t: 0\n", f);
            fclose(f);
            rt_path_alias("/proc/cpuinfo", cpuinfo);
        }
    }

    run_constructors(boot_cpu);
    uint32_t onload = rt_call(boot_cpu, rt_export("JNI_OnLoad"), 2, vm, 0);
    check(boot_cpu, "JNI_OnLoad");
    rt_log("JNI_OnLoad -> %#x", onload);
    if (onload != 0x00010004) rt_log("warning: JNI_OnLoad returned %#x, expected 0x10004", onload);

    /* Game.onCreate (UI thread): Device.init and GLResLoader.init */
    native("GLUtils_Device_nativeInit", rt_jni_class(CLS "GLUtils/Device"), 0, 0, 0, 0, 0);
    native("GLResLoader_nativeInit", rt_jni_class(CLS "GLResLoader"), 0, 0, 0, 0, 0);

    /* GameRenderer.onSurfaceCreated (GL thread). The Gameloft logo video that Java plays first is skipped. */
    surface_created(width, height);
    native("GameRenderer_nativeResize", cls_renderer, 2, (uint32_t)width, (uint32_t)height, 0, 0);
    started = 1;
    paused = 0;
    /* onWindowFocusChanged(true) normally arrives around here */
    native("GameGLSurfaceView_nativeResume", obj_view, 0, 0, 0, 0, 0);
}

/* The surface changed size (not the render buffer): GameRenderer.nativeResize is a no-op in the game. */
void rt_game_resize(int width, int height) {
    rt_jni_set_screen(width, height);
    if (!started) return;
    native("GameRenderer_nativeResize", cls_renderer, 2, (uint32_t)width, (uint32_t)height, 0, 0);
}

/* The GL context was recreated (Java: onSurfaceCreated again): all the game's GL objects are gone. nativeStart with
 * the game already initialised only records "restart", and the next frames reload every texture. */
void rt_game_restart_gl(int width, int height) {
    if (!started) return;
    surface_created(width, height);
}

/* ---- input queue: any thread posts, the render thread delivers ------------------------------------------------ */
enum { EV_TOUCH, EV_KEY, EV_ACCEL };
typedef struct { int kind, a, b, c, d; } event;
#define QMAX 512
static event queue[QMAX];
static int qhead, qtail; /* qhead: next to read, qtail: next to write */
static volatile int qlock;

static void post(event e) {
    while (__atomic_exchange_n(&qlock, 1, __ATOMIC_ACQUIRE)) { }
    int next = (qtail + 1) % QMAX;
    if (next != qhead) { queue[qtail] = e; qtail = next; }
    else if (e.kind == EV_TOUCH && e.a == 2) { /* full: a move is the cheapest thing to lose */ }
    else { qhead = (qhead + 1) % QMAX; queue[qtail] = e; qtail = next; }
    __atomic_store_n(&qlock, 0, __ATOMIC_RELEASE);
}
static int take(event *e) {
    while (__atomic_exchange_n(&qlock, 1, __ATOMIC_ACQUIRE)) { }
    int ok = qhead != qtail;
    if (ok) { *e = queue[qhead]; qhead = (qhead + 1) % QMAX; }
    __atomic_store_n(&qlock, 0, __ATOMIC_RELEASE);
    return ok;
}

/* action: 0 down, 1 up, 2 move (Android MotionEvent codes, as the front ends send them). x, y in render-buffer pixels,
 * origin top-left; pointer is the Android pointer id. Java sends ACTION_DOWN with id 0 and the others with
 * getPointerId; the caller passes the id it has. */
void rt_game_touch(int action, int x, int y, int pointer) {
    event e = {EV_TOUCH, action, x, y, pointer};
    post(e);
}

/* keycode: Android KEYCODE_*; Java does not forward volume (24, 25) and camera (27) keys */
void rt_game_key(int keycode, int down) {
    if (keycode == 24 || keycode == 25 || keycode == 27) return;
    event e = {EV_KEY, keycode, down, 0, 0};
    post(e);
}

void rt_game_accel(float x, float y, float z) {
    event e = {EV_ACCEL, 0, 0, 0, 0};
    memcpy(&e.a, &x, 4);
    memcpy(&e.b, &y, 4);
    memcpy(&e.c, &z, 4);
    post(e);
}

static void deliver(const event *e) {
    switch (e->kind) {
    case EV_TOUCH: {
        /* Java resets the Xperia touchpad state before every event */
        native("Game_nativeIsRightTouchPad", cls_game, 1, (uint32_t)-1, 0, 0, 0);
        native("Game_nativeIsLeftTouchPad", cls_game, 1, (uint32_t)-1, 0, 0, 0);
        const char *name = e->a == 0 ? "Game_nativeTouchPressed" : e->a == 1 ? "Game_nativeTouchReleased" : "Game_nativeTouchMoved";
        native(name, obj_game, 3, (uint32_t)e->b, (uint32_t)e->c, (uint32_t)e->d, 0);
        break;
    }
    case EV_KEY:
        native(e->b ? "Game_nativeSetOnKeyDown" : "Game_nativeSetOnKeyUp", cls_game, 1, (uint32_t)e->a, 0, 0, 0);
        break;
    case EV_ACCEL:
        native("Game_nativeAccelerometer", obj_game, 3, (uint32_t)e->a, (uint32_t)e->b, (uint32_t)e->c, 0); /* softfp: float bits */
        break;
    }
}

/* One frame: queued input, then GameRenderer.nativeRender (the platform layer swaps the buffers). While paused Java
 * clears to black and does not call the game. */
void rt_game_step(void) {
    if (!started) return;
    cpu_t *c = rt_thread_cpu();
    c->budget = 0xFFFFFFFFu;
    event e;
    while (take(&e)) if (!paused) deliver(&e);
    if (paused) return;
    native("GameRenderer_nativeRender", cls_renderer, 0, 0, 0, 0, 0);
}

/* onWindowFocusChanged(false) / (true). nativePause dereferences the application object, so never before nativeStart returned. */
void rt_game_pause(void) {
    if (!started || paused) return;
    paused = 1;
    native("GameGLSurfaceView_nativePause", obj_view, 0, 0, 0, 0, 0);
}
void rt_game_resume(void) {
    if (!started || !paused) return;
    paused = 0;
    native("GameGLSurfaceView_nativeResume", obj_view, 0, 0, 0, 0, 0);
}
cpu_t *rt_game_cpu(void) { return boot_cpu; }
