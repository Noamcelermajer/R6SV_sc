/* Host test for the JNI layer (runtime/jni.c) and the boot sequence (runtime/game.c) with a fake "translated" library:
 * the fake game functions call the JNIEnv/JavaVM tables exactly the way the real library does (JAVA-SIDE.md 4 and 5).
 *
 *   py -m ziglang cc -O1 -target x86_64-windows-gnu -Iruntime -Itools/recomp/runtime runtime/test/jni_test.c \
 *       runtime/jni.c -o build/jni/jni_test.exe && build/jni/jni_test.exe
 *
 * jni.c is a separate object so that the media.h functions defined here override its weak defaults. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../core.c"
#include "../mem.c"
#include "../printf.c"
#include "../game.c"

/* ---- stubs for what image_meta.c, the other runtime files and the translated code provide ---------------------- */
const uint32_t rt_image_base = 0x01000000u, rt_image_size = 0x10000u, rt_import_stub_base = 0x01100000u;
const uint32_t rt_init_array[1] = {0}, rt_init_array_count = 0;
const char *const rt_import_names[1] = {"x"};
const uint32_t rt_import_count = 0;
const rt_named_addr rt_data_import_slots[1] = {{"x", 0}}, rt_data_symbols[1] = {{"x", 0}};
const uint32_t rt_data_import_slot_count = 0, rt_data_symbol_count = 0;
const uint32_t rt_func_addrs[1] = {0}, rt_func_count = 0;
const char *const rt_func_names[1] = {"?"};
void rt_libc_register(void) {}
void rt_pthread_register(void) {}
void rt_gl_register(void) {}
void rt_sles_register(void) {}
void rt_data_imports_init(void) {}

/* guest addresses of the fake functions: even, because rt_call clears bit 0 */
enum { F_ONLOAD = 0x01000800, F_RENDERER_INIT = 0x01000804, F_START = 0x01000808, F_RENDER = 0x0100080c, F_GAME_INIT = 0x01000810, F_DEVICE_INIT = 0x01000814, F_RES_INIT = 0x01000818, F_MP_INIT = 0x0100081c, F_MP_TOTAL = 0x01000820, F_MP_SAME = 0x01000824, F_RESIZE = 0x01000828, F_PAUSE = 0x0100082c, F_RESUME = 0x01000830, F_TP_R = 0x01000834, F_TP_L = 0x01000838, F_PRESSED = 0x0100083c, F_MOVED = 0x01000840, F_RELEASED = 0x01000844, F_KDOWN = 0x01000848, F_KUP = 0x0100084c, F_ACCEL = 0x01000850 };
#define NEXP 21
const rt_named_addr rt_exports[NEXP] = {
    {"JNI_OnLoad", F_ONLOAD},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GameRenderer_nativeInit", F_RENDERER_INIT},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GameRenderer_nativeStart", F_START},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GameRenderer_nativeRender", F_RENDER},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeInit", F_GAME_INIT},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GLUtils_Device_nativeInit", F_DEVICE_INIT},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GLResLoader_nativeInit", F_RES_INIT},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GLMediaPlayer_nativeInit", F_MP_INIT},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GLMediaPlayer_nativeGetTotalSounds", F_MP_TOTAL},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GLMediaPlayer_nativeGetTotalSoundsOfSameInstance", F_MP_SAME},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GameRenderer_nativeResize", F_RESIZE},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GameGLSurfaceView_nativePause", F_PAUSE},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_GameGLSurfaceView_nativeResume", F_RESUME},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeIsRightTouchPad", F_TP_R},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeIsLeftTouchPad", F_TP_L},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeTouchPressed", F_PRESSED},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeTouchMoved", F_MOVED},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeTouchReleased", F_RELEASED},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeSetOnKeyDown", F_KDOWN},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeSetOnKeyUp", F_KUP},
    {"Java_com_gameloft_android_ANMP_GloftR6HP_Game_nativeAccelerometer", F_ACCEL},
};
const uint32_t rt_export_count = NEXP;

/* ---- media.h: record what the JNI layer sends --------------------------------------------------------------- */
static struct { int created, play, pause, stop, release, writes, bytes, mute; int args[6]; uint32_t sum; } media;
int rt_media_min_buffer(int rate, int channel_config, int encoding) { return rate == 44100 && channel_config == 12 && encoding == 2 ? 7040 : 0; }
int rt_media_track_create(int st, int rate, int ch, int enc, int bytes, int mode) {
    media.created++;
    media.args[0] = st; media.args[1] = rate; media.args[2] = ch; media.args[3] = enc; media.args[4] = bytes; media.args[5] = mode;
    return 7;
}
void rt_media_track_play(int id) { if (id == 7) media.play++; }
void rt_media_track_pause(int id) { if (id == 7) media.pause++; }
void rt_media_track_stop(int id) { if (id == 7) media.stop++; }
void rt_media_track_release(int id) { if (id == 7) media.release++; }
int rt_media_track_write(int id, const void *pcm, int bytes) {
    if (id != 7) return -1;
    media.writes++;
    media.bytes += bytes;
    for (int k = 0; k < bytes; k++) media.sum += ((const uint8_t *)pcm)[k];
    return bytes;
}
void rt_media_set_mute(int m) { media.mute = m; }
int rt_media_get_mute(void) { return media.mute; }

/* ---- harness -------------------------------------------------------------------------------------------- */
static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* a guest call of a JNIEnv function: table slot idx, up to 8 words (the rest on the guest stack) */
static uint32_t env_ptr, vm_ptr;
static uint32_t jcall(cpu_t *c, int idx, int n, ...) {
    uint32_t w[8];
    va_list ap;
    va_start(ap, n);
    for (int k = 0; k < n; k++) w[k] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t sp = (c->r[13] - 64) & ~7u;
    for (int k = 4; k < n; k++) rt_st32(sp + 4 * (uint32_t)(k - 4), w[k]);
    uint32_t saved_sp = c->r[13];
    for (int k = 0; k < 4 && k < n; k++) c->r[k] = w[k];
    c->r[13] = sp;
    c->r[14] = RT_RETURN_SENTINEL;
    uint32_t fn = rt_ld32(rt_ld32(w[0]) + 4u * (uint32_t)idx);
    rc_hostcall(c, rt_mem, (fn - RT_HOSTFN_BASE) / 4);
    c->r[13] = saved_sp;
    return c->r[0];
}
static uint32_t vcall(cpu_t *c, int idx, int n, ...) { /* a call through the VM table (JavaVM*) */
    uint32_t w[4];
    va_list ap;
    va_start(ap, n);
    for (int k = 0; k < n; k++) w[k] = va_arg(ap, uint32_t);
    va_end(ap);
    for (int k = 0; k < n; k++) c->r[k] = w[k];
    uint32_t fn = rt_ld32(rt_ld32(w[0]) + 4u * (uint32_t)idx);
    rc_hostcall(c, rt_mem, (fn - RT_HOSTFN_BASE) / 4);
    return c->r[0];
}
static uint32_t gs(const char *s) { return rt_static_string(s); }
/* JNIEnv indices */
enum { FindClass = 6, NewGlobalRef = 21, DeleteLocalRef = 23, NewObjectV = 29, GetMethodID = 33, CallObjectMethodV = 35,
       CallNonvirtualIntMethodV = 80, CallNonvirtualVoidMethodV = 92, GetStaticMethodID = 113, CallStaticObjectMethod = 114,
       CallStaticObjectMethodV = 115, CallStaticIntMethodV = 130, CallStaticIntMethod = 129, CallStaticVoidMethodV = 142, CallStaticFloatMethodV = 136,
       GetStaticFieldID = 144, GetStaticObjectField = 145, NewStringUTF = 167, GetStringUTFChars = 169, GetArrayLength = 171,
       NewByteArray = 176, GetByteArrayRegion = 200, GetPrimitiveArrayCritical = 222, ReleasePrimitiveArrayCritical = 223,
       PushLocalFrame = 19, PopLocalFrame = 20, ExceptionCheck = 228 };

static uint32_t manuf_ptr, g_env_seen, game_cls_seen;
static int native_start_args[4], render_count, touch_log[16][4], ntouch, key_log[8][2], nkey, resume_count, pause_count;
static uint32_t accel_bits[3];
static uint32_t statics[32];
static const char *static_names[][2] = {
    {"sendAppToBackground", "()V"}, {"openBrowser", "(Ljava/lang/String;)V"}, {"Exit", "()V"}, {"IsWifiEnabled", "()I"},
    {"GetPhoneLanguage", "()I"}, {"GetManufacturer", "()Ljava/lang/String;"}, {"GetModel", "()Ljava/lang/String;"},
    {"launchGetGames", "()V"}, {"launchGLLive", "(I)V"}, {"notifyTrophy", "(I)V"}, {"launchIGP", "(I)V"},
    {"SetReorientation", "(I)V"}, {"ShowSpinner", "(Ljava/lang/String;)V"}, {"DismissSpinner", "()V"}, {"isMoveMC", "()I"},
    {"getMoveCamera_X", "()I"}, {"getMoveCamera_Y", "()I"}, {"getMoveCamera_Flexible_X", "()I"}, {"getMoveCamera_Flexible_Y", "()I"},
    {"getGPUInfo", "()I"}, {"SetDeviceMute", "(I)V"}, {"d", "()V"}, {"db", "()[B"}, {"dc", "()[B"}, {"da", "()Landroid/app/Activity;"}};
#define NSTATIC 25

static void check_lookup_sets(cpu_t *c, uint32_t env, const char *cls, const char *names[][2], int n, int is_static, int *ok) {
    uint32_t k = (uint32_t)jcall(c, FindClass, 2, env, gs(cls));
    for (int i = 0; i < n; i++) {
        uint32_t m = (uint32_t)jcall(c, is_static ? GetStaticMethodID : GetMethodID, 4, env, k, gs(names[i][0]), gs(names[i][1]));
        if (!m) { printf("lookup failed: %s.%s%s\n", cls, names[i][0], names[i][1]); *ok = 0; }
    }
}

/* AudioTrack driven the way vox does (JAVA-SIDE.md 5.3); runs on a second host thread with its own CPU state */
static DWORD WINAPI vox_thread(void *p) {
    (void)p;
    cpu_t *c = rt_thread_cpu();
    uint32_t envp = rt_calloc(1, 4);
    CHECK(vcall(c, 4, 3, vm_ptr, envp, 0) == 0); /* AttachCurrentThread */
    uint32_t env = rt_ld32(envp);
    CHECK(env == env_ptr);
    uint32_t cls = (uint32_t)jcall(c, FindClass, 2, env, gs("android/media/AudioTrack"));
    uint32_t gcls = (uint32_t)jcall(c, NewGlobalRef, 2, env, cls);
    CHECK(gcls == cls);
    uint32_t init = (uint32_t)jcall(c, GetMethodID, 4, env, gcls, gs("<init>"), gs("(IIIIII)V"));
    uint32_t play = (uint32_t)jcall(c, GetMethodID, 4, env, gcls, gs("play"), gs("()V"));
    uint32_t stop = (uint32_t)jcall(c, GetMethodID, 4, env, gcls, gs("stop"), gs("()V"));
    uint32_t rel = (uint32_t)jcall(c, GetMethodID, 4, env, gcls, gs("release"), gs("()V"));
    uint32_t wr = (uint32_t)jcall(c, GetMethodID, 4, env, gcls, gs("write"), gs("([BII)I"));
    CHECK(init && play && stop && rel && wr);
    jcall(c, PushLocalFrame, 2, env, 2);
    uint32_t va = rt_calloc(1, 32);
    uint32_t args[6] = {3, 44100, 12, 2, 7040, 1};
    for (int k = 0; k < 6; k++) rt_st32(va + 4u * (uint32_t)k, args[k]);
    uint32_t track = (uint32_t)jcall(c, NewObjectV, 4, env, gcls, init, va);
    CHECK(track != 0);
    jcall(c, CallNonvirtualVoidMethodV, 5, env, track, gcls, play, 0);
    uint32_t arr = (uint32_t)jcall(c, NewByteArray, 2, env, 7040);
    CHECK(arr != 0 && (uint32_t)jcall(c, GetArrayLength, 2, env, arr) == 7040);
    for (int round = 0; round < 3; round++) {
        uint32_t ptr = (uint32_t)jcall(c, GetPrimitiveArrayCritical, 3, env, arr, 0);
        CHECK(ptr != 0);
        for (int k = 0; k < 4096; k++) rt_mem[ptr + (uint32_t)k] = (uint8_t)(k + round);
        jcall(c, ReleasePrimitiveArrayCritical, 4, env, arr, ptr, 0);
        uint32_t va2 = rt_calloc(1, 16);
        rt_st32(va2, arr); rt_st32(va2 + 4, 0); rt_st32(va2 + 8, 4096);
        CHECK((uint32_t)jcall(c, CallNonvirtualIntMethodV, 5, env, track, gcls, wr, va2) == 4096);
    }
    jcall(c, CallNonvirtualVoidMethodV, 5, env, track, gcls, stop, 0);
    jcall(c, CallNonvirtualVoidMethodV, 5, env, track, gcls, rel, 0);
    jcall(c, PopLocalFrame, 2, env, 0);
    CHECK(vcall(c, 5, 1, vm_ptr) == 0); /* DetachCurrentThread */
    return 0;
}

void rc_dispatch(cpu_t *c, uint8_t *m, uint32_t target) {
    (void)m;
    uint32_t env = c->r[0], a0 = c->r[2], a1 = c->r[3], a2 = rt_ld32(c->r[13]);
    switch (target) {
    case F_ONLOAD: {
        vm_ptr = c->r[0];
        uint32_t out = rt_calloc(1, 4);
        CHECK(vcall(c, 6, 3, vm_ptr, out, 0x10002) == 0 && rt_ld32(out) == rt_jni_env()); /* GetEnv */
        c->r[0] = 0x00010004;
        break;
    }
    case F_DEVICE_INIT: case F_RES_INIT: case F_MP_INIT: break;
    case F_MP_TOTAL: c->r[0] = 635; break;
    case F_MP_SAME: c->r[0] = 2; break;
    case F_RENDERER_INIT: g_env_seen = env; env_ptr = env; break;
    case F_GAME_INIT: {
        int ok = 1;
        game_cls_seen = c->r[1];
        check_lookup_sets(c, env, "com/gameloft/android/ANMP/GloftR6HP/Game", static_names, NSTATIC, 1, &ok);
        CHECK(ok);
        uint32_t k = c->r[1];
        for (int i = 0; i < NSTATIC; i++) statics[i] = (uint32_t)jcall(c, GetStaticMethodID, 4, env, k, gs(static_names[i][0]), gs(static_names[i][1]));
        CHECK(jcall(c, GetStaticMethodID, 4, env, k, gs("noSuchMethod"), gs("()V")) == 0);
        CHECK(jcall(c, GetStaticMethodID, 4, env, k, gs("IsWifiEnabled"), gs("()V")) == 0); /* wrong signature */
        CHECK(jcall(c, GetMethodID, 4, env, k, gs("IsWifiEnabled"), gs("()I")) == 0);        /* static looked up as instance */
        CHECK(jcall(c, FindClass, 2, env, gs("android/content/Context")) != 0);
        CHECK(jcall(c, FindClass, 2, env, gs("android/view/ViewRoot")) == 0);
        CHECK(jcall(c, ExceptionCheck, 1, env) == 1);
        jcall(c, 17, 1, env); /* ExceptionClear */
        CHECK(jcall(c, ExceptionCheck, 1, env) == 0);
        /* db(): igli.bin verbatim */
        uint32_t va = rt_calloc(1, 16);
        uint32_t arr = (uint32_t)jcall(c, CallStaticObjectMethodV, 4, env, k, statics[22], va);
        CHECK(arr && (uint32_t)jcall(c, GetArrayLength, 2, env, arr) == 0x40000);
        uint32_t buf = rt_malloc(0x40000);
        jcall(c, GetByteArrayRegion, 5, env, arr, 0, 0x40000, buf);
        int same = 1;
        for (uint32_t i = 0; i < 0x40000; i++) same &= rt_mem[buf + i] == (uint8_t)(i * 7 + 3);
        CHECK(same);
        /* the plain (non-V) form too */
        CHECK((uint32_t)jcall(c, CallStaticIntMethod, 3, env, k, statics[14]) == 0xFFFFFFFFu); /* isMoveMC */
        CHECK((uint32_t)jcall(c, CallStaticIntMethodV, 4, env, k, statics[19], va) == 2);      /* getGPUInfo */
        CHECK((uint32_t)jcall(c, CallStaticIntMethodV, 4, env, k, statics[15], va) == 0xFFFFFFFFu);
        /* GetPhoneLanguage with the locale of the test */
        CHECK((uint32_t)jcall(c, CallStaticIntMethodV, 4, env, k, statics[4], va) == 2); /* "de" */
        /* SetDeviceMute(1) with a va_list holding one int */
        rt_st32(va, 1);
        jcall(c, CallStaticVoidMethodV, 4, env, k, statics[20], va);
        CHECK(media.mute == 1);
        rt_st32(va, 0);
        jcall(c, CallStaticVoidMethodV, 4, env, k, statics[20], va);
        CHECK(media.mute == 0);
        /* ShowSpinner(String): va holds a string handle */
        uint32_t str = (uint32_t)jcall(c, NewStringUTF, 2, env, gs("Loading"));
        rt_st32(va, str);
        jcall(c, CallStaticVoidMethodV, 4, env, k, statics[12], va);
        /* manufacturer pointer must survive DeleteLocalRef */
        uint32_t ms = (uint32_t)jcall(c, CallStaticObjectMethodV, 4, env, k, statics[5], va);
        uint32_t p = (uint32_t)jcall(c, GetStringUTFChars, 3, env, ms, 0);
        jcall(c, DeleteLocalRef, 2, env, ms);
        manuf_ptr = p;
        CHECK(!strcmp((char *)G(p), "google"));
        CHECK((uint32_t)jcall(c, GetStringUTFChars, 3, env, ms, 0) == p); /* interned: same pointer every time */
        /* Context.TELEPHONY_SERVICE (ALicenseCheck's dead path) */
        uint32_t ctx = (uint32_t)jcall(c, FindClass, 2, env, gs("android/content/Context"));
        uint32_t fid = (uint32_t)jcall(c, GetStaticFieldID, 4, env, ctx, gs("TELEPHONY_SERVICE"), gs("Ljava/lang/String;"));
        uint32_t sv = (uint32_t)jcall(c, GetStaticObjectField, 3, env, ctx, fid);
        CHECK(fid && sv && !strcmp((char *)G((uint32_t)jcall(c, GetStringUTFChars, 3, env, sv, 0)), "phone"));
        /* a float argument promoted to double in a va_list: GLMediaPlayer.setMasterVolume(F) */
        uint32_t mp = (uint32_t)jcall(c, FindClass, 2, env, gs("com/gameloft/android/ANMP/GloftR6HP/GLMediaPlayer"));
        uint32_t smv = (uint32_t)jcall(c, GetStaticMethodID, 4, env, mp, gs("setMasterVolume"), gs("(F)V"));
        uint32_t gmv = (uint32_t)jcall(c, GetStaticMethodID, 4, env, mp, gs("getMasterVolume"), gs("()F"));
        CHECK(smv && gmv);
        double d = 0.25;
        memcpy(G(va), &d, 8);
        jcall(c, CallStaticVoidMethodV, 4, env, mp, smv, va);
        uint32_t fbits = (uint32_t)jcall(c, CallStaticFloatMethodV, 4, env, mp, gmv, va);
        float f; memcpy(&f, &fbits, 4);
        CHECK(f == 0.25f);
        /* the other classes the library caches: every method must resolve */
        break;
    }
    case F_START: {
        for (int i = 0; i < 4; i++) native_start_args[i] = (int)(i < 2 ? c->r[2 + i] : i == 2 ? rt_ld32(c->r[13]) : rt_ld32(c->r[13] + 4));
        /* vox: GetEnv, AudioTrack lookups, getMinBufferSize on this thread, the track on another */
        uint32_t cls = (uint32_t)jcall(c, FindClass, 2, env, gs("android/media/AudioTrack"));
        uint32_t gm = (uint32_t)jcall(c, GetStaticMethodID, 4, env, cls, gs("getMinBufferSize"), gs("(III)I"));
        uint32_t va = rt_calloc(1, 16);
        rt_st32(va, 44100); rt_st32(va + 4, 12); rt_st32(va + 8, 2);
        CHECK((uint32_t)jcall(c, CallStaticIntMethodV, 4, env, cls, gm, va) == 7040);
        HANDLE h = CreateThread(NULL, 0, vox_thread, NULL, 0, NULL);
        WaitForSingleObject(h, 20000);
        CloseHandle(h);
        break;
    }
    case F_RENDER: render_count++; break;
    case F_RESIZE: break;
    case F_PAUSE: pause_count++; break;
    case F_RESUME: resume_count++; break;
    case F_TP_R: case F_TP_L: CHECK(a0 == 0xFFFFFFFFu); break;
    case F_PRESSED: case F_MOVED: case F_RELEASED:
        touch_log[ntouch][0] = target == F_PRESSED ? 0 : target == F_RELEASED ? 1 : 2;
        touch_log[ntouch][1] = (int)a0; touch_log[ntouch][2] = (int)a1; touch_log[ntouch][3] = (int)a2;
        ntouch++;
        break;
    case F_KDOWN: case F_KUP: key_log[nkey][0] = (int)a0; key_log[nkey][1] = target == F_KDOWN; nkey++; break;
    case F_ACCEL: accel_bits[0] = a0; accel_bits[1] = a1; accel_bits[2] = a2; break;
    default: printf("unexpected dispatch %#x\n", target); fails++; break;
    }
    c->r[15] = c->r[14];
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) rt_verbose = 3;
    static uint8_t image[0x10000];
    rt_mem_init_from(image, sizeof image);
    static uint8_t igli[0x40000];
    for (uint32_t i = 0; i < sizeof igli; i++) igli[i] = (uint8_t)(i * 7 + 3);
    rt_jni_set_igli(igli, sizeof igli);
    rt_jni_set_locale("de_DE");
    CHECK(rt_jni_game_language() == 1);
    rt_game_boot(NULL, "build/jni", "build/jni", "build/jni", 854, 480);
    CHECK(native_start_args[0] == 1 && native_start_args[1] == 854 && native_start_args[2] == 480 && native_start_args[3] == 1);
    CHECK(g_env_seen == rt_jni_env());
    CHECK(resume_count == 1);
    CHECK(media.created == 1 && media.play == 1 && media.stop == 1 && media.release == 1);
    CHECK(media.args[0] == 3 && media.args[1] == 44100 && media.args[2] == 12 && media.args[3] == 2 && media.args[4] == 7040 && media.args[5] == 1);
    CHECK(media.writes == 3 && media.bytes == 3 * 4096);
    uint32_t expect = 0;
    for (int r = 0; r < 3; r++) for (int k = 0; k < 4096; k++) expect += (uint8_t)(k + r);
    CHECK(media.sum == expect);
    CHECK(!strcmp((char *)G(manuf_ptr), "google")); /* still valid after DeleteLocalRef */

    /* frames and input */
    rt_game_touch(0, 100, 200, 0);
    rt_game_touch(2, 110, 210, 0);
    rt_game_touch(1, 110, 210, 0);
    rt_game_key(4, 1);
    rt_game_key(24, 1); /* filtered */
    rt_game_key(4, 0);
    rt_game_accel(1.5f, -2.0f, 9.75f);
    rt_game_step();
    CHECK(render_count == 1);
    CHECK(ntouch == 3 && touch_log[0][0] == 0 && touch_log[0][1] == 100 && touch_log[0][2] == 200 && touch_log[0][3] == 0);
    CHECK(touch_log[1][0] == 2 && touch_log[2][0] == 1 && touch_log[2][1] == 110);
    CHECK(nkey == 2 && key_log[0][0] == 4 && key_log[0][1] == 1 && key_log[1][1] == 0);
    float ax; memcpy(&ax, &accel_bits[0], 4);
    CHECK(ax == 1.5f);
    rt_game_pause();
    rt_game_touch(0, 1, 1, 0);
    rt_game_step();
    CHECK(pause_count == 1 && render_count == 1 && ntouch == 3); /* paused: no frame, input dropped */
    rt_game_resume();
    rt_game_step();
    CHECK(resume_count == 2 && render_count == 2);
    rt_game_restart_gl(480, 854); /* surface_created again with the larger side first */
    CHECK(native_start_args[1] == 854 && native_start_args[2] == 480);
    printf("jni_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
