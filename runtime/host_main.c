/* Windows host runner: a window (or an offscreen buffer) with an OpenGL ES context from ANGLE, the game library
 * loaded into game memory, and the calls the Android Java side would make (init, resize, step every frame, touches).
 *
 *   r6sv_runner --image image.bin --angle <dir with libEGL.dll and libGLESv2.dll> --data <GloftR6HP folder> [options]
 *
 *   --image FILE         the packed image (default: image.bin), --angle DIR (default: angle\ next to the exe)
 *
 *   --data DIR           the content of /sdcard/gameloft/games/GloftR6HP (data/, SaveGame.bin, ...). Default: data
 *   --home DIR           stands in for /data/data/<package> (default: home), --temp DIR (default: temp)
 *   --width N --height N size of the window or of the offscreen buffer (default 854x480, see runtime/frontend.h)
 *   --offscreen          render into a pbuffer of exactly that size; paced to real time
 *   --frames N           stop after N frames (0: run until the window is closed)
 *   --shot FRAME:FILE    save the frame (before swapping) as FILE.png, .bmp or .ppm; repeatable. "--shot FILE" with
 *                        --frames N saves the last frame
 *   --tap FRAME[:X:Y]    a tap (touch down, three frames later up) at X,Y (default: the center); repeatable
 *   --drag FRAME:X0:Y0:X1:Y1[:N]   touch down at X0,Y0, a move per frame along N frames (default 10), then up
 *   --accel X:Y:Z        a constant accelerometer reading, sent every frame
 *   --wav FILE           record the game's audio into a WAV file (read by the audio backend from NOVA_AUDIO_WAV)
 *   -v                   verbose (repeat for more: -vv logs host functions, -vvv every import)
 *
 * On a fault the process prints the game address, the host address and (checked builds) the game call chain with
 * function names, then continues the exception chain.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "frontend.h"
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

/* weak default until game.c provides the accelerometer entry point */
__attribute__((weak)) void rt_game_accel(float x, float y, float z) { (void)x; (void)y; (void)z; }

/* ---- window and input ---------------------------------------------------------------------------------- */
static int width = RT_RENDER_WIDTH, height = RT_DEFAULT_HEIGHT, quit, mouse_down;
static struct { int action, x, y; } touch_q[256];
static int touch_n;

static void queue_touch(int action, int x, int y) {
    if (touch_n < 256) { touch_q[touch_n].action = action; touch_q[touch_n].x = x; touch_q[touch_n].y = y; touch_n++; }
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

/* ---- crash reports: the game address, the host address and the game call chain ------------------------- */
static void report_crash(const char *what) {
    rt_log("crash: %s", what);
    rt_print_trace(rt_game_cpu());
    cpu_t *self = rt_thread_cpu();
    if (self != rt_game_cpu()) { rt_log("(thread %lu)", GetCurrentThreadId()); rt_print_trace(self); }
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    char what[256];
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
        const char *rw = e->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read of";
        if (a >= (uintptr_t)rt_mem && a < (uintptr_t)rt_mem + (1ull << 32))
            snprintf(what, sizeof what, "%s game address %#llx (unmapped game memory), host pc %p", rw,
                     (unsigned long long)(a - (uintptr_t)rt_mem), e->ExceptionRecord->ExceptionAddress);
        else
            snprintf(what, sizeof what, "%s host address %p, host pc %p", rw, (void *)a, e->ExceptionRecord->ExceptionAddress);
    } else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
               code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || code == EXCEPTION_PRIV_INSTRUCTION) {
        snprintf(what, sizeof what, "exception %#lx at host pc %p", code, e->ExceptionRecord->ExceptionAddress);
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    report_crash(what);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void on_abort(int sig) {
    report_crash(sig == SIGABRT ? "abort() (rt_fatal or an assertion)" : "signal");
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---- screenshots: PNG (stored deflate blocks), BMP, PPM ------------------------------------------------ */
static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t b[4];
    be32(b, len); fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc32_update(0, (const uint8_t *)type, 4);
    if (len) crc = crc32_update(crc, data, len);
    be32(b, crc); fwrite(b, 1, 4, f);
}

/* rgba: top-down RGBA8 */
static int write_png(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < w; x++) memcpy(raw + y * row + 1 + 3 * x, rgba + 4 * ((size_t)y * w + x), 3);
    }
    size_t blocks = (raw_len + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_len + 5 * blocks + 4);
    size_t zl = 0, pos = 0;
    z[zl++] = 0x78; z[zl++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < raw_len; k++) { a = (a + raw[k]) % 65521; b = (b + a) % 65521; }
    while (pos < raw_len) {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[zl++] = pos + n == raw_len; z[zl++] = (uint8_t)n; z[zl++] = (uint8_t)(n >> 8);
        z[zl++] = (uint8_t)~n; z[zl++] = (uint8_t)(~n >> 8);
        memcpy(z + zl, raw + pos, n); zl += n; pos += n;
    }
    be32(z + zl, (b << 16) | a); zl += 4;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zl);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 1;
}

static int has_ext(const char *path, const char *ext) {
    size_t n = strlen(path), e = strlen(ext);
    return n >= e && !_stricmp(path + n - e, ext);
}

static void save_shot(const char *path) {
    size_t n = (size_t)width * height;
    uint8_t *px = malloc(n * 4), *top = malloc(n * 4);
    void (EGL_API *readpixels)(int, int, int, int, unsigned, unsigned, void *) = getproc("glReadPixels");
    readpixels(0, 0, width, height, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
    for (int y = 0; y < height; y++) memcpy(top + (size_t)y * width * 4, px + (size_t)(height - 1 - y) * width * 4, (size_t)width * 4);
    int ok = 0;
    if (has_ext(path, ".png")) {
        ok = write_png(path, top, width, height);
    } else if (has_ext(path, ".ppm")) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", width, height);
            for (size_t k = 0; k < n; k++) fwrite(top + 4 * k, 1, 3, f);
            fclose(f); ok = 1;
        }
    } else { /* BMP, 32 bit, bottom-up rows as read */
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t size = 54 + (uint32_t)n * 4;
            uint8_t h[54] = {'B', 'M'};
            memcpy(h + 2, &size, 4); h[10] = 54; h[14] = 40;
            memcpy(h + 18, &width, 4); memcpy(h + 22, &height, 4); h[26] = 1; h[28] = 32;
            fwrite(h, 1, 54, f);
            for (size_t k = 0; k < n; k++) { uint8_t t = px[4 * k]; px[4 * k] = px[4 * k + 2]; px[4 * k + 2] = t; }
            fwrite(px, 4, n, f);
            fclose(f); ok = 1;
        }
    }
    rt_log(ok ? "screenshot written to %s" : "cannot write %s", path);
    free(px); free(top);
}

/* ---- options ------------------------------------------------------------------------------------------- */
static const char *opt(int argc, char **argv, const char *name, const char *def) {
    for (int k = 1; k + 1 < argc; k++) if (!strcmp(argv[k], name)) return argv[k + 1];
    return def;
}
static int flag(int argc, char **argv, const char *name) {
    for (int k = 1; k < argc; k++) if (!strcmp(argv[k], name)) return 1;
    return 0;
}

typedef struct { int frame; int x0, y0, x1, y1, n; } gesture; /* a tap is a gesture with n = 3 and equal ends */
typedef struct { int frame; char file[MAX_PATH]; } shot_req;

int main(int argc, char **argv) {
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "-v")) rt_verbose++;
        else if (!strcmp(argv[k], "-vv")) rt_verbose += 2;
        else if (!strcmp(argv[k], "-vvv")) rt_verbose += 3;
        else if (!strcmp(argv[k], "-h") || !strcmp(argv[k], "--help")) { puts("see the comment at the top of runtime/host_main.c"); return 0; }
    }
    const char *image = opt(argc, argv, "--image", "image.bin");
    /* default ANGLE folder: <folder of the exe>\angle if it exists, else the folder of the exe */
    char angle_default[MAX_PATH];
    GetModuleFileNameA(NULL, angle_default, MAX_PATH);
    char *slash = strrchr(angle_default, '\\');
    if (slash) *slash = 0;
    {
        char sub[MAX_PATH + 8];
        snprintf(sub, sizeof sub, "%s\\angle", angle_default);
        if (GetFileAttributesA(sub) != INVALID_FILE_ATTRIBUTES) snprintf(angle_default, sizeof angle_default, "%s", sub);
    }
    const char *angle = opt(argc, argv, "--angle", angle_default);
    const char *data = opt(argc, argv, "--data", "data");
    const char *home = opt(argc, argv, "--home", "home");
    const char *temp = opt(argc, argv, "--temp", "temp");
    const char *wav = opt(argc, argv, "--wav", NULL);
    int frames = atoi(opt(argc, argv, "--frames", "0"));
    int offscreen = flag(argc, argv, "--offscreen");
    width = atoi(opt(argc, argv, "--width", "854"));
    height = atoi(opt(argc, argv, "--height", "480"));
    if (width < 64 || height < 64 || width > 8192 || height > 8192) rt_fatal("bad --width/--height");

    shot_req shots[64]; int nshots = 0;
    gesture gestures[64]; int ngest = 0;
    int have_accel = 0; float accel[3] = {0, 0, 0};
    for (int k = 1; k + 1 < argc; k++) {
        const char *a = argv[k], *v = argv[k + 1];
        if (!strcmp(a, "--shot") && nshots < 64) {
            int f = -1, used = 0;
            if (sscanf(v, "%d:%n", &f, &used) == 1 && used > 0) { shots[nshots].frame = f; snprintf(shots[nshots].file, MAX_PATH, "%s", v + used); }
            else { shots[nshots].frame = frames > 0 ? frames - 1 : 0; snprintf(shots[nshots].file, MAX_PATH, "%s", v); }
            nshots++;
        } else if (!strcmp(a, "--tap") && ngest < 64) {
            int f = 0, x = -1, y = -1;
            sscanf(v, "%d:%d:%d", &f, &x, &y);
            if (x < 0) x = width / 2;
            if (y < 0) y = height / 2;
            gestures[ngest++] = (gesture){f, x, y, x, y, 3};
        } else if (!strcmp(a, "--drag") && ngest < 64) {
            int f = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, n = 10;
            if (sscanf(v, "%d:%d:%d:%d:%d:%d", &f, &x0, &y0, &x1, &y1, &n) < 5) rt_fatal("--drag FRAME:X0:Y0:X1:Y1[:N]");
            gestures[ngest++] = (gesture){f, x0, y0, x1, y1, n < 1 ? 1 : n};
        } else if (!strcmp(a, "--accel")) {
            have_accel = sscanf(v, "%f:%f:%f", &accel[0], &accel[1], &accel[2]) == 3;
        }
    }
    CreateDirectoryA(data, NULL);
    CreateDirectoryA(home, NULL);
    CreateDirectoryA(temp, NULL);
    if (wav) _putenv_s("NOVA_AUDIO_WAV", wav);
    AddVectoredExceptionHandler(1, on_exception);
    signal(SIGABRT, on_abort);
    SetProcessDPIAware(); /* a window of the render size must not be scaled by Windows */

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
    HWND win = NULL;
    WNDCLASSA wc = {CS_OWNDC, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, "r6sv"};
    RegisterClassA(&wc);
    /* a fixed-size window: the client area is exactly the render size, so touches need no scaling */
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, style, FALSE);
    win = CreateWindowA("r6sv", "Rainbow Six: Shadow Vanguard (recompiled)", style | (offscreen ? 0 : WS_VISIBLE), CW_USEDEFAULT,
                        CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    EGLDisplay dpy = eglGetDisplay(GetDC(win));
    EGLint major, minor, n;
    if (!eglInitialize(dpy, &major, &minor)) rt_fatal("eglInitialize failed (%#x)", eglGetError());
    const EGLint renderable = RT_EGL_CONTEXT_VERSION >= 3 ? 0x40 : RT_EGL_CONTEXT_VERSION == 2 ? 0x4 : 0x1;
    const EGLint cfg_attr[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3040 /* RENDERABLE_TYPE */, renderable,
                               0x3033 /* EGL_SURFACE_TYPE */, 0x1 | 0x4 /* PBUFFER | WINDOW */, 0x3038};
    EGLConfig cfg;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) rt_fatal("no EGL config");
    const EGLint pb_attr[] = {0x3057 /* EGL_WIDTH */, width, 0x3056 /* EGL_HEIGHT */, height, 0x3038};
    EGLSurface surf = offscreen ? eglCreatePbufferSurface(dpy, cfg, pb_attr) : eglCreateWindowSurface(dpy, cfg, win, NULL);
    const EGLint ctx_attr[] = {0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, RT_EGL_CONTEXT_VERSION, 0x3038};
    EGLContext ctx = eglCreateContext(dpy, cfg, NULL, ctx_attr);
    if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) rt_fatal("EGL context failed (%#x)", eglGetError());
    eglSwapInterval(dpy, 1);
    rt_log("EGL %d.%d, ES %d context, %s %dx%d", major, minor, RT_EGL_CONTEXT_VERSION, offscreen ? "offscreen" : "window", width, height);

    /* game memory, the folders, then the Java side's start-up sequence */
    rt_mem_init(image);
    rt_gl_getproc = getproc;
    rt_frontend_paths(data, home);
    {
        char probe[MAX_PATH];
        snprintf(probe, sizeof probe, "%s\\data\\Res.array", data);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) rt_log("warning: %s not found; --data must be the content of GloftR6HP", probe);
    }
    rt_game_boot(data, data, home, temp, width, height);

    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int frame = 0; !quit && (frames == 0 || frame < frames); frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        for (int k = 0; k < ngest; k++) {
            gesture *g = &gestures[k];
            int dt = frame - g->frame;
            if (dt < 0 || dt > g->n) continue;
            if (dt == 0) queue_touch(0, g->x0, g->y0);
            else if (dt < g->n) queue_touch(2, g->x0 + (g->x1 - g->x0) * dt / g->n, g->y0 + (g->y1 - g->y0) * dt / g->n);
            else queue_touch(1, g->x1, g->y1);
        }
        for (int k = 0; k < touch_n; k++) rt_game_touch(touch_q[k].action, touch_q[k].x, touch_q[k].y, 0);
        touch_n = 0;
        if (have_accel) rt_game_accel(accel[0], accel[1], accel[2]);
        rt_game_step();
        if (frame == 0) rt_log("first frame done");
        for (int k = 0; k < nshots; k++) if (shots[k].frame == frame) save_shot(shots[k].file);
        eglSwapBuffers(dpy, surf);
        if (offscreen) Sleep(16); /* a pbuffer has no vsync; the game's timing is in real seconds */
    }
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    rt_log("exiting after %.1f s", (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    return 0;
}
