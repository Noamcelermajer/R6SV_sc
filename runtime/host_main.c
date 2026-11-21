/* Windows host runner: a window with an OpenGL ES 2 context from ANGLE, the game library loaded into game memory,
 * and the calls the Android Java side would make (setPaths, init, initGL, resize, step every frame, touches).
 *
 *   nova3_runner --image image.bin --angle <dir with libEGL.dll and libGLESv2.dll> --obb <dir with main.1050...obb>
 *                [--data dir] [--home dir] [--temp dir] [--width 1280 --height 720] [--frames N --shot out.bmp] [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "rt.h"

/* ---- EGL (loaded from ANGLE's libEGL.dll) -------------------------------------------------------------- */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
#define EGL_API __stdcall
static EGLDisplay (EGL_API *eglGetDisplay)(void *);
static unsigned (EGL_API *eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static unsigned (EGL_API *eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLSurface (EGL_API *eglCreateWindowSurface)(EGLDisplay, EGLConfig, HWND, const EGLint *);
static EGLSurface (EGL_API *eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLContext (EGL_API *eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static unsigned (EGL_API *eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static unsigned (EGL_API *eglSwapBuffers)(EGLDisplay, EGLSurface);
static unsigned (EGL_API *eglSwapInterval)(EGLDisplay, EGLint);
static EGLint (EGL_API *eglGetError)(void);
static void *(EGL_API *eglGetProcAddress)(const char *);
static HMODULE gles;

static void *getproc(const char *name) {
    void *p = (void *)GetProcAddress(gles, name);
    return p ? p : eglGetProcAddress(name);
}

/* ---- window -------------------------------------------------------------------------------------------- */
static int width = 1280, height = 720, quit;
static int mouse_down;
static struct { int pending, action, x, y; } touch_q[64];
static int touch_n;

static void queue_touch(int action, int x, int y) {
    if (touch_n < 64) { touch_q[touch_n].action = action; touch_q[touch_n].x = x; touch_q[touch_n].y = y; touch_n++; }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CLOSE: quit = 1; return 0;
    case WM_LBUTTONDOWN: mouse_down = 1; SetCapture(h); queue_touch(0, (short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_LBUTTONUP: mouse_down = 0; ReleaseCapture(); queue_touch(1, (short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_MOUSEMOVE: if (mouse_down) queue_touch(2, (short)LOWORD(l), (short)HIWORD(l)); return 0;
    default: return DefWindowProcA(h, msg, w, l);
    }
}

/* ---- crash reports: a host access violation inside game memory names the game address --------------- */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
        if (a >= (uintptr_t)rt_mem && a < (uintptr_t)rt_mem + (1ull << 32))
            rt_log("crash: %s game address %#llx (unmapped game memory)", e->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read of",
                   (unsigned long long)(a - (uintptr_t)rt_mem));
        else
            rt_log("crash: access violation at host address %p", (void *)a);
        rt_print_trace(rt_game_cpu());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void save_bmp(const char *path) {
    uint8_t *px = malloc((size_t)width * height * 4);
    void (EGL_API *readpixels)(int, int, int, int, unsigned, unsigned, void *) = getproc("glReadPixels");
    readpixels(0, 0, width, height, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return; }
    uint32_t size = 54 + (uint32_t)width * height * 4;
    uint8_t h[54] = {'B', 'M'};
    memcpy(h + 2, &size, 4);
    h[10] = 54; h[14] = 40;
    memcpy(h + 18, &width, 4);
    memcpy(h + 22, &height, 4);
    h[26] = 1; h[28] = 32;
    fwrite(h, 1, 54, f);
    for (int k = 0; k < width * height; k++) { uint8_t t = px[4 * k]; px[4 * k] = px[4 * k + 2]; px[4 * k + 2] = t; }
    fwrite(px, 4, (size_t)width * height, f); /* GL rows are bottom-up, as BMP rows are */
    fclose(f);
    free(px);
    rt_log("screenshot written to %s", path);
}

static const char *opt(int argc, char **argv, const char *name, const char *def) {
    for (int k = 1; k + 1 < argc; k++) if (!strcmp(argv[k], name)) return argv[k + 1];
    return def;
}

int main(int argc, char **argv) {
    for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "-v")) rt_verbose++;
    const char *image = opt(argc, argv, "--image", "image.bin");
    const char *angle = opt(argc, argv, "--angle", ".");
    const char *obb = opt(argc, argv, "--obb", ".");
    const char *data = opt(argc, argv, "--data", "data");
    const char *home = opt(argc, argv, "--home", "home");
    const char *temp = opt(argc, argv, "--temp", "temp");
    const char *shot = opt(argc, argv, "--shot", NULL);
    int frames = atoi(opt(argc, argv, "--frames", "0"));
    /* --tap FRAME[:X:Y] (repeatable): a scripted tap, at the screen center by default, for unattended runs */
    int taps[64][3], ntaps = 0;
    for (int k = 1; k + 1 < argc && ntaps < 64; k++)
        if (!strcmp(argv[k], "--tap")) {
            int f = 0, x = -1, y = -1;
            sscanf(argv[k + 1], "%d:%d:%d", &f, &x, &y);
            taps[ntaps][0] = f; taps[ntaps][1] = x; taps[ntaps][2] = y;
            ntaps++;
        }
    width = atoi(opt(argc, argv, "--width", "1280"));
    height = atoi(opt(argc, argv, "--height", "720"));
    CreateDirectoryA(data, NULL);
    CreateDirectoryA(home, NULL);
    CreateDirectoryA(temp, NULL);
    AddVectoredExceptionHandler(1, on_exception);

    /* window and GL context */
    char path[MAX_PATH];
    SetDllDirectoryA(angle); /* libGLESv2.dll loads d3dcompiler_47.dll by name */
    snprintf(path, sizeof path, "%s\\libEGL.dll", angle);
    HMODULE egl = LoadLibraryA(path);
    snprintf(path, sizeof path, "%s\\libGLESv2.dll", angle);
    gles = LoadLibraryA(path);
    if (!egl || !gles) rt_fatal("cannot load ANGLE from %s (libEGL.dll, libGLESv2.dll)", angle);
#define LOAD(fn) *(void **)&fn = (void *)GetProcAddress(egl, #fn); if (!fn) rt_fatal("libEGL.dll has no " #fn)
    LOAD(eglGetDisplay); LOAD(eglInitialize); LOAD(eglChooseConfig); LOAD(eglCreateWindowSurface); LOAD(eglCreatePbufferSurface);
    LOAD(eglCreateContext); LOAD(eglMakeCurrent); LOAD(eglSwapBuffers); LOAD(eglSwapInterval); LOAD(eglGetError);
    LOAD(eglGetProcAddress);
    WNDCLASSA wc = {CS_OWNDC, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, "nova3"};
    RegisterClassA(&wc);
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND win = CreateWindowA("nova3", "N.O.V.A. 3 (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    EGLDisplay dpy = eglGetDisplay(GetDC(win));
    EGLint major, minor, n;
    if (!eglInitialize(dpy, &major, &minor)) rt_fatal("eglInitialize failed (%#x)", eglGetError());
    /* --offscreen: render into a pbuffer of exactly --width x --height (a window larger than the monitor gets
     * shrunk by Windows, so the game would be told one size and draw into another) */
    int offscreen = 0;
    for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--offscreen")) offscreen = 1;
    const EGLint cfg_attr[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3040, 4,
                               0x3033 /* EGL_SURFACE_TYPE */, 0x1 | 0x4 /* PBUFFER | WINDOW */, 0x3038};
    EGLConfig cfg;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) rt_fatal("no EGL config");
    const EGLint pb_attr[] = {0x3057 /* EGL_WIDTH */, width, 0x3056 /* EGL_HEIGHT */, height, 0x3038};
    EGLSurface surf = offscreen ? eglCreatePbufferSurface(dpy, cfg, pb_attr) : eglCreateWindowSurface(dpy, cfg, win, NULL);
    const EGLint ctx_attr[] = {0x3098, 2, 0x3038};
    EGLContext ctx = eglCreateContext(dpy, cfg, NULL, ctx_attr);
    if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) rt_fatal("EGL context failed (%#x)", eglGetError());
    eglSwapInterval(dpy, 1);
    rt_log("EGL %d.%d, window %dx%d", major, minor, width, height);

    /* game memory, then the Java side's start-up sequence */
    rt_mem_init(image);
    rt_gl_getproc = getproc;
    rt_game_boot(obb, data, home, temp, width, height);

    for (int frame = 0; !quit && (frames == 0 || frame < frames); frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        for (int k = 0; k < ntaps; k++) {
            int x = taps[k][1] < 0 ? width / 2 : taps[k][1], y = taps[k][2] < 0 ? height / 2 : taps[k][2];
            if (frame == taps[k][0]) queue_touch(0, x, y);
            if (frame == taps[k][0] + 3) queue_touch(1, x, y);
        }
        for (int k = 0; k < touch_n; k++) rt_game_touch(touch_q[k].action, touch_q[k].x, touch_q[k].y, 0);
        touch_n = 0;
        rt_game_step();
        if (frame == 0) rt_log("first frame done");
        if (shot && frames && frame == frames - 1) save_bmp(shot);
        for (int k = 1; k + 1 < argc; k++) /* --shot-at FRAME:FILE */
            if (!strcmp(argv[k], "--shot-at")) {
                const char *colon = strchr(argv[k + 1], ':');
                if (colon && atoi(argv[k + 1]) == frame) save_bmp(colon + 1);
            }
        eglSwapBuffers(dpy, surf);
        if (offscreen) Sleep(16); /* a pbuffer has no vsync; the game's timing is in real seconds */
    }
    rt_log("exiting");
    return 0;
}
