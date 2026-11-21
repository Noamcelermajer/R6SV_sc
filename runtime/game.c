/* Driving the game the way its Java side does: constructors, JNI_OnLoad, the installer's state, init, initGL,
 * resize, then step() per frame and touch events. Shared by the Windows runner and the Android activity; the
 * platform layer owns the window, the GL context and input, and calls these on the GL thread. */
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"

static cpu_t *cpu;
static uint32_t env, lib_class;

static void check(const char *what) {
    if (cpu->fault) {
        rt_log("%s: translated code faulted (fault %u, r15 %#x)", what, cpu->fault, cpu->r[15]);
        rt_print_trace(cpu);
        rt_fatal("stopping after a fault");
    }
}

static uint32_t java(const char *method, int nargs, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    char name[256];
    snprintf(name, sizeof name, "Java_com_gameloft_android_ANMP_GloftN3HM_GL2JNILib_%s", method);
    if (rt_verbose) rt_log("-> %s", method);
    uint32_t r = rt_call(cpu, rt_export(name), 2 + nargs, env, lib_class, a0, a1, a2, a3);
    check(method);
    return r;
}

/* The installer's licensing state. On a phone, Play licensing answers, GDRMPolicy.nativeAllow records the grant
 * (lockPointer2 -> 1, lockPointer3 -> [1]) and GameInstaller.nativeStart starts the game (lockPointer4 -> [1]).
 * That path cannot run here: N.O.V.A. 3 is no longer sold and its license servers are gone, and the GDRMPolicy
 * natives also read lockPointer1, which no code in the library sets, so they fault. The runtime therefore writes
 * the state a successful check leaves, with the values those functions store; lockPointer1 points at a block whose
 * second word is 1, so the integrity divisions in those functions do not fault if anything calls them. No game code
 * is changed. GameInstaller.initNative still runs, for the JNIEnv and installer state it saves. */
static uint32_t boxed(uint32_t v) { uint32_t p = rt_malloc(8); rt_st32(p, v); rt_st32(p + 4, v); return p; }
static void installer(void) {
    uint32_t cls = rt_jni_class("com/gameloft/android/ANMP/GloftN3HM/installer/GameInstaller");
    rt_call(cpu, rt_export("Java_com_gameloft_android_ANMP_GloftN3HM_installer_GameInstaller_initNative"), 2, env, cls);
    check("GameInstaller.initNative");
    rt_st32(rt_data_symbol("lockPointer1"), boxed(1));
    rt_st32(rt_data_symbol("lockPointer2"), boxed(1));
    rt_st32(rt_data_symbol("lockPointer3"), boxed(1));
    rt_st32(rt_data_symbol("lockPointer4"), boxed(1));
}

/* Game memory must be loaded (rt_mem_init*) and rt_gl_getproc set, with the GL context current. */
void rt_game_boot(const char *obb, const char *data, const char *home, const char *temp, int width, int height) {
    rt_data_imports_init();
    cpu = rt_thread_cpu();
    rt_imports_init();
    uint32_t vm = rt_jni_init();
    env = rt_jni_env();
    lib_class = rt_jni_class("com/gameloft/android/ANMP/GloftN3HM/GL2JNILib");
    rt_jni_set_screen(width, height);

    rt_log("running %u constructors", rt_init_array_count);
    for (uint32_t k = 0; k < rt_init_array_count; k++) {
        rt_call(cpu, rt_init_array[k], 0);
        if (cpu->fault) rt_fatal("constructor %u at %#x faulted (fault %u, r15 %#x)", k, rt_init_array[k], cpu->fault, cpu->r[15]);
    }
    rt_log("JNI_OnLoad -> %#x", rt_call(cpu, rt_export("JNI_OnLoad"), 2, vm, 0));
    check("JNI_OnLoad");

    /* the Java side's order: init (which calls back GL2JNILib.setupPaths -> setPaths), initGL, InitViewSettings,
     * SetDepthValue, resize, then step per frame */
    rt_jni_set_paths(obb, data, home, temp);
    rt_path_alias("/data/data/com.gameloft.android.ANMP.GloftN3HM", home); /* the game's saves and settings */
    /* The game's CPU-features code reads /proc/cpuinfo, and vox picks its NEON assembly mixers and decoders when it
     * finds "neon". That assembly is not translated, so the runtime describes an ARMv7 CPU with VFP and no NEON: vox
     * then uses its C++ mixers and decoders, which are. */
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
    installer();
    java("init", 0, 0, 0, 0, 0);
    java("initGL", 0, 0, 0, 0, 0);
    java("InitViewSettings", 0, 0, 0, 0, 0);
    java("SetDepthValue", 1, 24, 0, 0, 0);
    java("resize", 2, (uint32_t)width, (uint32_t)height, 0, 0);
}

void rt_game_resize(int width, int height) {
    rt_jni_set_screen(width, height);
    java("resize", 2, (uint32_t)width, (uint32_t)height, 0, 0);
}

/* action: 0 down, 1 up, 2 move (Android MotionEvent codes). The game's Java side passes its own codes: 1 down,
 * 0 up, 2 move. glf::AndroidAddTouchEvent maps those to the touch queue's 0 (touchBegan), 2 (touchEnded) and
 * 1 (touchMoved), as TouchScreenBase::update dispatches them. */
void rt_game_touch(int action, int x, int y, int pointer) {
    int code = action == 0 ? 1 : action == 1 ? 0 : 2;
    java("touchEvent", 4, (uint32_t)code, (uint32_t)x, (uint32_t)y, (uint32_t)pointer);
}

void rt_game_step(void) {
    cpu->budget = 0xFFFFFFFFu;
    java("step", 0, 0, 0, 0, 0);
}

void rt_game_pause(void) { java("GamePause", 0, 0, 0, 0, 0); }
void rt_game_resume(void) { java("GameResume", 0, 0, 0, 0, 0); }
cpu_t *rt_game_cpu(void) { return cpu; }
