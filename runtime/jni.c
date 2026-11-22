/* A small JVM for the recompiled Rainbow Six: Shadow Vanguard library: JavaVM and JNIEnv function tables in game
 * memory whose entries are host functions, Java objects as handles, and an emulation of the Java methods the native
 * code looks up and calls (docs/JAVA-SIDE.md, section 9; docs/notes/jni.md).
 *
 * Handles are game addresses in an unmapped range (RT_JOBJ_BASE + index), so the game can store and compare them but
 * never dereference them. Method, field and class lookups are resolved against the tables below: a (class, name,
 * signature) the table does not know is logged once and fails (NULL), as a JVM would with NoSuchMethodError. The
 * only Java behaviour that really does something is Game.db() (igli.bin), android.media.AudioTrack (forwarded to
 * media.h) and the Game statics that return values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media.h"
#include "rt.h"

#define RT_JOBJ_BASE 0x00d00000u
#define MAX_JOBJ 0x40000u
#define P "com/gameloft/android/ANMP/GloftR6HP/"

enum { J_CLASS = 1, J_STRING, J_OBJECT, J_ARRAY, J_METHOD, J_FIELD };
typedef struct jobj {
    int kind;
    char *cls;       /* class name (class, object, method, field) or element signature (array) */
    char *name;      /* method/field name */
    char *sig;       /* method/field signature */
    int is_static;
    char *str;       /* string value (UTF-8) */
    uint32_t utf;    /* strings: interned guest copy for GetStringUTFChars (never freed) */
    uint32_t data;   /* array storage in game memory */
    uint32_t length; /* array length */
    uint32_t elem;   /* array element size */
    int entry;       /* methods and fields: index into the emulation table, -1 when none */
    int track;       /* AudioTrack objects: the media.h track id */
} jobj;
static jobj objs[MAX_JOBJ];
static volatile uint32_t nobj = 1;
static volatile int lock_;
static void lock(void) { while (__atomic_exchange_n(&lock_, 1, __ATOMIC_ACQUIRE)) { } }
static void unlock(void) { __atomic_store_n(&lock_, 0, __ATOMIC_RELEASE); }

static char *dup(const char *s) { return s ? strdup(s) : NULL; }
static uint32_t new_obj(jobj o) {
    lock();
    uint32_t k = nobj;
    if (k >= MAX_JOBJ) rt_fatal("out of JNI handles (%u)", MAX_JOBJ);
    if (k == MAX_JOBJ - MAX_JOBJ / 16) rt_log("JNI: 94%% of the handle table is used");
    objs[k] = o;
    __atomic_store_n(&nobj, k + 1, __ATOMIC_RELEASE);
    unlock();
    return RT_JOBJ_BASE + k;
}
static jobj *get(uint32_t h) {
    if (h < RT_JOBJ_BASE + 1 || h >= RT_JOBJ_BASE + __atomic_load_n(&nobj, __ATOMIC_ACQUIRE)) return NULL;
    return &objs[h - RT_JOBJ_BASE];
}

static uint32_t class_handle(const char *name) {
    lock();
    for (uint32_t k = 1; k < nobj; k++)
        if (objs[k].kind == J_CLASS && !strcmp(objs[k].cls, name)) { unlock(); return RT_JOBJ_BASE + k; }
    unlock();
    return new_obj((jobj){.kind = J_CLASS, .cls = dup(name), .entry = -1});
}
uint32_t rt_jstring(const char *s) {
    return new_obj((jobj){.kind = J_STRING, .cls = dup("java/lang/String"), .str = dup(s ? s : ""), .entry = -1});
}
static uint32_t new_object(const char *cls) { return new_obj((jobj){.kind = J_OBJECT, .cls = dup(cls), .entry = -1}); }
static uint32_t elem_size(char t) { return t == 'J' || t == 'D' ? 8 : t == 'S' || t == 'C' ? 2 : t == 'Z' || t == 'B' ? 1 : 4; }
static uint32_t new_array(const char *elem_sig, uint32_t length) {
    uint32_t size = elem_size(elem_sig[0]);
    uint32_t data = rt_calloc(length ? length : 1, size);
    return new_obj((jobj){.kind = J_ARRAY, .cls = dup(elem_sig), .data = data, .length = length, .elem = size, .entry = -1});
}
static uint32_t new_bytes(const void *p, uint32_t n) {
    uint32_t h = new_array("B", n);
    if (n && p) memcpy(G(get(h)->data), p, n);
    return h;
}
uint32_t rt_jni_class(const char *name) { return class_handle(name); }
uint32_t rt_jni_object(const char *cls) { return new_object(cls); }

/* ---- strings: Java is UTF-16, the handles keep UTF-8 ------------------------------------------------------- */
static uint32_t utf16_length(const char *s) {
    uint32_t n = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        n += *p >= 0xF0 ? 2 : 1;
    }
    return n;
}
static uint32_t utf8_to_utf16(const char *s, uint16_t *out, uint32_t cap) { /* returns the length, writes up to cap */
    uint32_t n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p++; }
        else if (*p < 0xE0 && p[1]) { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
        else if (*p < 0xF0 && p[1] && p[2]) { cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); p += 3; }
        else if (p[1] && p[2] && p[3]) { cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu); p += 4; }
        else break;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (n < cap) out[n] = (uint16_t)(0xD800 + (cp >> 10));
            n++;
            if (n < cap) out[n] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            n++;
        } else {
            if (n < cap) out[n] = (uint16_t)cp;
            n++;
        }
    }
    return n;
}
static char *utf16_to_utf8(const uint16_t *u, uint32_t n) {
    char *out = malloc(3 * (size_t)n + 1), *o = out;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t cp = u[k];
        if (cp >= 0xD800 && cp < 0xDC00 && k + 1 < n && u[k + 1] >= 0xDC00 && u[k + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[k + 1] - 0xDC00);
            k++;
        }
        if (cp < 0x80) *o++ = (char)cp;
        else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
        else { *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    }
    *o = 0;
    return out;
}
static const char *str_of(uint32_t h) { jobj *s = get(h); return s && s->kind == J_STRING && s->str ? s->str : ""; }

/* ---- settings the platform layer and game.c give us ---------------------------------------------------------- */
static char *paths[4]; /* obb, data, home, temp */
void rt_jni_set_paths(const char *obb, const char *data, const char *home, const char *temp) {
    const char *in[4] = {obb, data, home, temp};
    for (int k = 0; k < 4; k++) { free(paths[k]); paths[k] = dup(in[k]); }
}
static int screen_w = 854, screen_h = 480;
void rt_jni_set_screen(int w, int h) { screen_w = w; screen_h = h; }

/* Locale: Game.d (passed to nativeStart) and Game.GetPhoneLanguage use different numbering (JAVA-SIDE.md 4.1) */
static int phone_lang, game_lang, locale_set;
void rt_jni_set_locale(const char *tag) {
    locale_set = 1;
    char l[3] = {0, 0, 0};
    if (tag) for (int k = 0; k < 2 && tag[k]; k++) l[k] = (char)((tag[k] | 0x20));
    int p = 0, g = 0;
    if (!strcmp(l, "fr")) { p = 1; g = 2; }
    else if (!strcmp(l, "de")) { p = 2; g = 1; }
    else if (!strcmp(l, "it")) { p = 3; g = 3; }
    else if (!strcmp(l, "es")) { p = 4; g = 4; }
    else if (!strcmp(l, "ja")) { p = 5; g = 5; }
    else if (!strcmp(l, "pt")) { p = 8; g = 6; }
    else if (!strcmp(l, "ru")) { p = 9; g = 0; }
    else if (!strcmp(l, "zh")) { p = tag && (strstr(tag, "TW") || strstr(tag, "tw") || strstr(tag, "HK") || strstr(tag, "Hant")) ? 11 : 10; g = 0; }
    phone_lang = p;
    game_lang = g;
}
static void default_locale(void) {
    if (locale_set) return;
    const char *e = getenv("R6SV_LANG");
    rt_jni_set_locale(e && *e ? e : "en");
}
int rt_jni_game_language(void) { default_locale(); return game_lang; }

static char *manufacturer, *model;
void rt_jni_set_device(const char *manufacturer_name, const char *model_name) {
    free(manufacturer); free(model);
    manufacturer = dup(manufacturer_name);
    model = dup(model_name);
}

/* res/raw/igli.bin: Game.db() returns it verbatim. An embedded copy wins, then files. */
static uint8_t *igli_embedded;
static uint32_t igli_embedded_size;
void rt_jni_set_igli(const void *data, uint32_t size) {
    free(igli_embedded);
    igli_embedded = malloc(size ? size : 1);
    memcpy(igli_embedded, data, size);
    igli_embedded_size = size;
}
static uint32_t igli_handle;
static uint32_t igli_array(void) {
    if (igli_handle) return igli_handle;
    uint8_t *buf = NULL;
    size_t n = 0;
    if (igli_embedded) { buf = igli_embedded; n = igli_embedded_size; }
    char path[1100];
    const char *env = getenv("R6SV_IGLI");
    const char *cand[6] = {env, NULL, NULL, NULL, "original/apk/res/raw/igli.bin", NULL};
    char p1[1100], p2[1100], p3[1100];
    if (paths[1]) { snprintf(p1, sizeof p1, "%s/igli.bin", paths[1]); cand[1] = p1; }
    if (paths[2]) { snprintf(p2, sizeof p2, "%s/igli.bin", paths[2]); cand[2] = p2; }
    if (paths[3]) { snprintf(p3, sizeof p3, "%s/igli.bin", paths[3]); cand[3] = p3; }
    for (int k = 0; !buf && k < 5; k++) {
        if (!cand[k] || !*cand[k]) continue;
        FILE *f = fopen(cand[k], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz <= (1 << 20)) {
            buf = malloc((size_t)sz);
            if (fread(buf, 1, (size_t)sz, f) == (size_t)sz) { n = (size_t)sz; snprintf(path, sizeof path, "%s", cand[k]); rt_log("JNI: Game.db() = %s (%zu bytes)", path, n); }
            else { free(buf); buf = NULL; }
        }
        fclose(f);
    }
    if (!buf) {
        n = 0x40000;
        buf = calloc(1, n);
        rt_log("JNI: igli.bin not found (rt_jni_set_igli, R6SV_IGLI, <data>/igli.bin, <home>/igli.bin): Game.db() returns %zu zero bytes", n);
    } else if (n != 0x40000) {
        rt_log("JNI: warning: igli.bin has %zu bytes, the game expects 262144", n);
    }
    igli_handle = new_bytes(buf, (uint32_t)n);
    if (buf != igli_embedded) free(buf);
    return igli_handle;
}

/* ---- host services with defaults (a platform layer or media.c overrides them) --------------------------------- */
__attribute__((weak)) void rt_host_open_url(const char *url) { rt_log("JNI: Game.openBrowser(%s) ignored", url); }
__attribute__((weak)) void rt_host_background(void) { rt_log("JNI: Game.sendAppToBackground ignored"); }
__attribute__((weak)) void rt_host_exit(void) { rt_log("JNI: Game.Exit"); fflush(NULL); exit(0); }
static volatile int exit_requested;
int rt_game_exit_requested(void) { return exit_requested; }

/* media.h defaults: no sound backend linked */
static void no_media(const char *what) {
    static int once;
    if (!once) { once = 1; rt_log("JNI: no media backend linked (%s): audio is discarded", what); }
}
__attribute__((weak)) int rt_media_min_buffer(int rate, int channel_config, int encoding) {
    (void)rate; (void)channel_config; (void)encoding;
    return 8192;
}
__attribute__((weak)) int rt_media_track_create(int stream_type, int rate, int channel_config, int encoding, int buffer_bytes, int mode) {
    (void)stream_type; (void)rate; (void)channel_config; (void)encoding; (void)buffer_bytes; (void)mode;
    no_media("track_create");
    return 1;
}
__attribute__((weak)) void rt_media_track_play(int id) { (void)id; }
__attribute__((weak)) void rt_media_track_pause(int id) { (void)id; }
__attribute__((weak)) void rt_media_track_stop(int id) { (void)id; }
__attribute__((weak)) void rt_media_track_release(int id) { (void)id; }
__attribute__((weak)) int rt_media_track_write(int id, const void *pcm, int bytes) { (void)id; (void)pcm; return bytes; }
__attribute__((weak)) void rt_media_set_mute(int mute) { (void)mute; }
__attribute__((weak)) int rt_media_get_mute(void) { return 0; }

/* ---- Java emulation ---------------------------------------------------------------------------------------
 * A handler gets the receiver (the object, or the class for statics) and the decoded argument words; it returns the
 * raw value: an int/boolean/handle in the low word, a float as its bits, a long/double in both words. */
typedef uint64_t (*jfn)(jobj *self, const uint32_t *a);
typedef struct { const char *cls, *name, *sig; int is_static; jfn fn; } jmethod;

static char *seen[4096];
static void log_once(const char *what) {
    lock();
    for (int k = 0; k < 4096 && seen[k]; k++) if (!strcmp(seen[k], what)) { unlock(); return; }
    for (int k = 0; k < 4096; k++) if (!seen[k]) { seen[k] = strdup(what); break; }
    unlock();
    rt_log("JNI: %s", what);
}

static uint64_t h_nop(jobj *s, const uint32_t *a) { (void)s; (void)a; return 0; }
static uint64_t h_one(jobj *s, const uint32_t *a) { (void)s; (void)a; return 1; }
static uint64_t h_m1(jobj *s, const uint32_t *a) { (void)s; (void)a; return 0xFFFFFFFFu; }
static uint64_t h_two(jobj *s, const uint32_t *a) { (void)s; (void)a; return 2; }

/* Game */
static uint64_t g_send_bg(jobj *s, const uint32_t *a) { (void)s; (void)a; rt_host_background(); return 0; }
static uint64_t g_open_browser(jobj *s, const uint32_t *a) { (void)s; rt_host_open_url(str_of(a[0])); return 0; }
static uint64_t g_exit(jobj *s, const uint32_t *a) { (void)s; (void)a; exit_requested = 1; rt_host_exit(); return 0; }
static uint64_t g_phone_lang(jobj *s, const uint32_t *a) { (void)s; (void)a; default_locale(); return (uint32_t)phone_lang; }
static uint64_t g_manufacturer(jobj *s, const uint32_t *a) {
    (void)s; (void)a;
    static uint32_t h;
    if (!h) h = rt_jstring(manufacturer ? manufacturer : "google");
    return h;
}
static uint64_t g_model(jobj *s, const uint32_t *a) {
    (void)s; (void)a;
    static uint32_t h;
    if (!h) h = rt_jstring(model ? model : "pixel");
    return h;
}
static uint64_t g_launch(jobj *s, const uint32_t *a) { (void)s; log_once("Game.launchGLLive/launchIGP/launchGetGames ignored (online portals are gone)"); (void)a; return 0; }
static uint64_t g_trophy(jobj *s, const uint32_t *a) { (void)s; if (rt_verbose) rt_log("JNI: Game.notifyTrophy(%u)", a[0]); return 0; }
static uint64_t g_spinner(jobj *s, const uint32_t *a) { (void)s; (void)a; return 0; }
static uint64_t g_mute(jobj *s, const uint32_t *a) { (void)s; rt_media_set_mute(a[0] == 1); return 0; }
static uint64_t g_db(jobj *s, const uint32_t *a) { (void)s; (void)a; return igli_array(); }
static uint64_t g_dc(jobj *s, const uint32_t *a) { (void)s; (void)a; static uint32_t h; if (!h) h = new_bytes("0", 1); return h; }
static uint64_t g_da(jobj *s, const uint32_t *a) { (void)s; (void)a; static uint32_t h; if (!h) h = new_object("android/app/Activity"); return h; }

/* GLUtils.Device */
static uint64_t d_zero_bytes(jobj *s, const uint32_t *a) { (void)s; (void)a; return new_bytes("", 1); }
static uint64_t d_empty_bytes(jobj *s, const uint32_t *a) { (void)s; (void)a; return new_bytes("", 0); }
static uint64_t d_line(jobj *s, const uint32_t *a) { (void)s; (void)a; return new_bytes("00", 2); }
static uint64_t d_version(jobj *s, const uint32_t *a) { (void)s; (void)a; return new_bytes("1.1.6", 5); }
static uint64_t d_host(jobj *s, const uint32_t *a) {
    (void)s; (void)a;
    char b[256];
    snprintf(b, sizeof b, "%s_%s", model ? model : "pixel", model ? model : "pixel");
    return new_bytes(b, (uint32_t)strlen(b));
}
static uint64_t d_useragent(jobj *s, const uint32_t *a) { (void)s; (void)a; return rt_jstring("GL_EMU_001"); }

/* GLMediaPlayer: the Java sound engine is not used by the game (the native Vox engine plays through AudioTrack);
 * the lookups succeed, the volume and "all" controls are forwarded as far as media.h has an equivalent. */
static float master_volume = 1.0f;
static uint64_t mp_set_master(jobj *s, const uint32_t *a) { (void)s; memcpy(&master_volume, &a[0], 4); return 0; }
static uint64_t mp_get_master(jobj *s, const uint32_t *a) { (void)s; (void)a; uint32_t u; memcpy(&u, &master_volume, 4); return u; }
static uint64_t mp_stop_all(jobj *s, const uint32_t *a) { (void)s; (void)a; log_once("GLMediaPlayer.stopAll/pauseAll/resumeAll called (not expected): ignored"); return 0; }

/* android.media.AudioTrack -> media.h */
static uint64_t at_init(jobj *s, const uint32_t *a) {
    s->track = rt_media_track_create((int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5]);
    if (!s->track) rt_log("JNI: AudioTrack(%d, %d, %d, %d, %d, %d): the media backend refused the track", (int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5]);
    return 0;
}
static uint64_t at_min_buffer(jobj *s, const uint32_t *a) { (void)s; return (uint32_t)rt_media_min_buffer((int)a[0], (int)a[1], (int)a[2]); }
static uint64_t at_play(jobj *s, const uint32_t *a) { (void)a; if (s->track) rt_media_track_play(s->track); return 0; }
static uint64_t at_pause(jobj *s, const uint32_t *a) { (void)a; if (s->track) rt_media_track_pause(s->track); return 0; }
static uint64_t at_stop(jobj *s, const uint32_t *a) { (void)a; if (s->track) rt_media_track_stop(s->track); return 0; }
static uint64_t at_release(jobj *s, const uint32_t *a) { (void)a; if (s->track) rt_media_track_release(s->track); s->track = 0; return 0; }
static uint64_t at_write(jobj *s, const uint32_t *a) {
    jobj *arr = get(a[0]);
    int off = (int)a[1], n = (int)a[2];
    if (!s->track || !arr || arr->kind != J_ARRAY || off < 0 || n < 0 || (uint64_t)off + (uint64_t)n > (uint64_t)arr->length * arr->elem) {
        log_once("AudioTrack.write: bad arguments (-1 returned)");
        return (uint32_t)-1; /* ERROR_BAD_VALUE-like */
    }
    return (uint32_t)rt_media_track_write(s->track, G(arr->data + (uint32_t)off), n);
}

/* android.content.Context / TelephonyManager: ALicenseCheck's dead path */
static uint64_t ctx_service(jobj *s, const uint32_t *a) {
    (void)s;
    return strcmp(str_of(a[0]), "phone") ? 0 : new_object("android/telephony/TelephonyManager");
}
static uint64_t tm_device_id(jobj *s, const uint32_t *a) { (void)s; (void)a; return rt_jstring("000000000000000"); }

#define S(c, n, sg, f) {c, n, sg, 1, f}
#define I(c, n, sg, f) {c, n, sg, 0, f}
#define MP P "GLMediaPlayer"
static const jmethod methods[] = {
    /* Game: statics cached by Game.nativeInit (21) and ALicenseCheck (4) */
    S(P "Game", "sendAppToBackground", "()V", g_send_bg),
    S(P "Game", "openBrowser", "(Ljava/lang/String;)V", g_open_browser),
    S(P "Game", "Exit", "()V", g_exit),
    S(P "Game", "IsWifiEnabled", "()I", h_one),
    S(P "Game", "GetPhoneLanguage", "()I", g_phone_lang),
    S(P "Game", "GetManufacturer", "()Ljava/lang/String;", g_manufacturer),
    S(P "Game", "GetModel", "()Ljava/lang/String;", g_model),
    S(P "Game", "launchGetGames", "()V", g_launch),
    S(P "Game", "launchGLLive", "(I)V", g_launch),
    S(P "Game", "notifyTrophy", "(I)V", g_trophy),
    S(P "Game", "launchIGP", "(I)V", g_launch),
    S(P "Game", "SetReorientation", "(I)V", h_nop),
    S(P "Game", "ShowSpinner", "(Ljava/lang/String;)V", g_spinner),
    S(P "Game", "DismissSpinner", "()V", g_spinner),
    S(P "Game", "isMoveMC", "()I", h_m1),
    S(P "Game", "getMoveCamera_X", "()I", h_m1),
    S(P "Game", "getMoveCamera_Y", "()I", h_m1),
    S(P "Game", "getMoveCamera_Flexible_X", "()I", h_m1),
    S(P "Game", "getMoveCamera_Flexible_Y", "()I", h_m1),
    S(P "Game", "getGPUInfo", "()I", h_two),
    S(P "Game", "SetDeviceMute", "(I)V", g_mute),
    S(P "Game", "d", "()V", h_nop),
    S(P "Game", "db", "()[B", g_db),
    S(P "Game", "dc", "()[B", g_dc),
    S(P "Game", "da", "()Landroid/app/Activity;", g_da),
    /* GLResLoader (lookup only) */
    S(P "GLResLoader", "getResourceFull", "(Ljava/lang/String;)[B", h_nop),
    S(P "GLResLoader", "getResourceBytes", "(Ljava/lang/String;II)[B", h_nop),
    S(P "GLResLoader", "getResourceLength", "(Ljava/lang/String;)I", h_nop),
    S(P "GLResLoader", "getSoundRaw", "(I)[B", h_nop),
    S(P "GLResLoader", "getResourceLengthSoundRaw", "(I)I", h_nop),
    /* GameRenderer (lookup only) */
    S(P "GameRenderer", "getKeyboardText", "()[B", h_nop),
    S(P "GameRenderer", "setKeyboard", "(ILjava/lang/String;I)V", h_nop),
    S(P "GameRenderer", "isKeyboardRequired", "()I", h_nop),
    S(P "GameRenderer", "swapEGLBuffers", "()V", h_nop),
    /* GLMediaPlayer (lookup only, 34 methods) */
    S(MP, "isLoaded", "(I)I", h_nop), S(MP, "unload", "(I)V", h_nop), S(MP, "load", "(I)V", h_nop),
    S(MP, "registerSoundFile", "(ILjava/lang/String;I)V", h_nop), S(MP, "play", "(IFI)I", h_nop),
    S(MP, "play", "(IFIF)I", h_nop), S(MP, "pause", "(I)V", h_nop), S(MP, "pauseAll", "()V", mp_stop_all),
    S(MP, "resume", "(IF)V", h_nop), S(MP, "resumeAll", "(F)V", mp_stop_all), S(MP, "stop", "(I)V", h_nop),
    S(MP, "setVolume", "(FI)V", h_nop), S(MP, "getVolume", "(I)F", h_nop), S(MP, "setMasterVolume", "(F)V", mp_set_master),
    S(MP, "getMasterVolume", "()F", mp_get_master), S(MP, "stopAll", "()V", mp_stop_all), S(MP, "onRecoverAudio", "()V", h_nop),
    S(MP, "recoverAudio", "()V", h_nop), S(MP, "reinit", "()V", h_nop), S(MP, "isRecoveringAudio", "()I", h_nop),
    S(MP, "isPlaying", "(I)Z", h_nop), S(MP, "isEmitterPlaying", "(II)Z", h_nop), S(MP, "setEmitterVolume", "(IIF)V", h_nop),
    S(MP, "setEmitterPitch", "(IIF)V", h_nop), S(MP, "setEmitterParams", "(IIFF)V", h_nop), S(MP, "stopEmitter", "(II)V", h_nop),
    S(MP, "pauseEmitter", "(II)V", h_nop), S(MP, "resumeEmitter", "(II)V", h_nop), S(MP, "getEmitter", "(I)I", h_nop),
    S(MP, "update", "()V", h_nop), S(MP, "loadSoundGroup", "(IZ)V", h_nop), S(MP, "swapPool", "(I)V", h_nop),
    S(MP, "swapAllPools", "()V", h_nop), S(MP, "setSwapTimer", "(Z)V", h_nop),
    /* GLUtils.Device (ADevice::Init) */
    S(P "GLUtils/Device", "a", "()[B", d_zero_bytes),
    S(P "GLUtils/Device", "b", "()[B", d_empty_bytes),
    S(P "GLUtils/Device", "c", "()[B", d_line),
    S(P "GLUtils/Device", "d", "()[B", d_version),
    S(P "GLUtils/Device", "IsWifiEnable", "()Z", h_one),
    S(P "GLUtils/Device", "getHostName", "()[B", d_host),
    S(P "GLUtils/Device", "e", "(I)V", h_nop),
    S(P "GLUtils/Device", "getUserAgent", "()Ljava/lang/String;", d_useragent),
    /* android.media.AudioTrack (the Vox driver) */
    I("android/media/AudioTrack", "<init>", "(IIIIII)V", at_init),
    S("android/media/AudioTrack", "getMinBufferSize", "(III)I", at_min_buffer),
    I("android/media/AudioTrack", "play", "()V", at_play),
    I("android/media/AudioTrack", "pause", "()V", at_pause),
    I("android/media/AudioTrack", "stop", "()V", at_stop),
    I("android/media/AudioTrack", "release", "()V", at_release),
    I("android/media/AudioTrack", "write", "([BII)I", at_write),
    /* ALicenseCheck's dead path */
    I("android/content/Context", "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", ctx_service),
    I("android/app/Activity", "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", ctx_service),
    I("android/telephony/TelephonyManager", "getDeviceId", "()Ljava/lang/String;", tm_device_id),
};
#define NMETHODS ((int)(sizeof methods / sizeof *methods))

typedef struct { const char *cls, *name, *sig, *value; } jfield;
static const jfield fields[] = {
    {"android/content/Context", "TELEPHONY_SERVICE", "Ljava/lang/String;", "phone"},
};
#define NFIELDS ((int)(sizeof fields / sizeof *fields))

static const char *const known_classes[] = {
    P "Game", P "GameRenderer", P "GameGLSurfaceView", P "GLResLoader", P "GLMediaPlayer", P "GLUtils/Device",
    "android/media/AudioTrack", "android/content/Context", "android/app/Activity", "android/telephony/TelephonyManager",
    "java/lang/Object", "java/lang/String", NULL};

/* ---- lookups ------------------------------------------------------------------------------------------------ */
static uint32_t lookup_method(uint32_t clazz, const char *name, const char *sig, int is_static) {
    jobj *k = get(clazz);
    if (!k || (k->kind != J_CLASS && k->kind != J_OBJECT)) { log_once("GetMethodID with a bad class handle"); return 0; }
    for (int i = 0; i < NMETHODS; i++) {
        if (strcmp(methods[i].cls, k->cls) || strcmp(methods[i].name, name) || strcmp(methods[i].sig, sig)) continue;
        if (methods[i].is_static != is_static) break;
        lock();
        for (uint32_t h = 1; h < nobj; h++)
            if (objs[h].kind == J_METHOD && objs[h].entry == i) { unlock(); return RT_JOBJ_BASE + h; }
        unlock();
        return new_obj((jobj){.kind = J_METHOD, .cls = dup(k->cls), .name = dup(name), .sig = dup(sig), .is_static = is_static, .entry = i});
    }
    char what[600];
    snprintf(what, sizeof what, "no such %smethod %s.%s%s (NULL)", is_static ? "static " : "", k->cls, name, sig);
    log_once(what);
    return 0;
}
static uint32_t lookup_field(uint32_t clazz, const char *name, const char *sig, int is_static) {
    jobj *k = get(clazz);
    if (!k || (k->kind != J_CLASS && k->kind != J_OBJECT)) { log_once("GetFieldID with a bad class handle"); return 0; }
    for (int i = 0; i < NFIELDS; i++)
        if (!strcmp(fields[i].cls, k->cls) && !strcmp(fields[i].name, name) && !strcmp(fields[i].sig, sig))
            return new_obj((jobj){.kind = J_FIELD, .cls = dup(k->cls), .name = dup(name), .sig = dup(sig), .is_static = is_static, .entry = i});
    char what[600];
    snprintf(what, sizeof what, "no such %sfield %s.%s %s (NULL)", is_static ? "static " : "", k->cls, name, sig);
    log_once(what);
    return 0;
}

static int pending_exception; /* FindClass of a class the platform lacks */
static uint32_t find_class(const char *name) {
    if (!strcmp(name, "android/view/ViewRoot")) { /* removed from Android long ago; processTouchpadAsPointer copes */
        pending_exception = 1;
        log_once("FindClass(android/view/ViewRoot) fails (NoClassDefFoundError), as on a current Android");
        return 0;
    }
    int known = 0;
    for (int i = 0; known_classes[i]; i++) known |= !strcmp(known_classes[i], name);
    if (!known) {
        char what[300];
        snprintf(what, sizeof what, "FindClass(%s): not a known class, a bare class handle is returned", name);
        log_once(what);
    }
    return class_handle(name);
}

static uint64_t java_call(jobj *m, jobj *self, const uint32_t *args) {
    if (m->entry < 0 || m->entry >= NMETHODS) return 0;
    if (rt_verbose > 1) rt_log("JNI call %s.%s%s", m->cls, m->name, m->sig);
    return methods[m->entry].fn(self, args);
}

/* ---- argument decoding for Call*Method (varargs), Call*MethodV (va_list) and Call*MethodA (jvalue[]) ------ */
enum { ARGS_VAR, ARGS_VA, ARGS_A };
static uint64_t bits_of(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static int decode_args(cpu_t *c, const char *sig, int mode, int first_word, uint32_t va_or_array, uint32_t *out) {
    rt_args a = {c, first_word, mode == ARGS_VA ? va_or_array : 0};
    int n = 0;
    uint32_t jv = va_or_array;
    for (const char *p = sig + 1; *p && *p != ')' && n < 28; p++) {
        char t = *p;
        while (*p == '[') p++;
        if (*p == 'L') p = strchr(p, ';');
        if (!p) break;
        if (t == '[') t = 'L';
        if (mode == ARGS_A) {
            out[n++] = rt_ld32(jv);
            if (t == 'J' || t == 'D') out[n++] = rt_ld32(jv + 4);
            jv += 8;
        } else if (t == 'J' || t == 'D') {
            uint64_t v = t == 'J' ? rt_args_u64(&a) : bits_of(rt_args_double(&a));
            out[n++] = (uint32_t)v;
            out[n++] = (uint32_t)(v >> 32);
        } else if (t == 'F') { /* varargs and va_list promote float to double */
            float f = (float)rt_args_double(&a);
            memcpy(&out[n++], &f, 4);
        } else {
            out[n++] = rt_args_u32(&a);
        }
    }
    return n;
}

/* ---- the JNIEnv functions ------------------------------------------------------------------------------------ */
static uint32_t env_addr, vm_addr, env_first_slot, vm_first_slot;
#define A(k) rt_arg(c, k)

static void ret64(cpu_t *c, uint64_t v) { c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }

/* JNI function names, for diagnostics */
static const char *const types[10] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
static const char *const atypes[8] = {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
static const char *jni_name(int idx, char *buf, size_t cap) {
    static const char *const base[] = {
        [4] = "GetVersion", [5] = "DefineClass", [6] = "FindClass", [7] = "FromReflectedMethod", [8] = "FromReflectedField",
        [9] = "ToReflectedMethod", [10] = "GetSuperclass", [11] = "IsAssignableFrom", [12] = "ToReflectedField", [13] = "Throw",
        [14] = "ThrowNew", [15] = "ExceptionOccurred", [16] = "ExceptionDescribe", [17] = "ExceptionClear", [18] = "FatalError",
        [19] = "PushLocalFrame", [20] = "PopLocalFrame", [21] = "NewGlobalRef", [22] = "DeleteGlobalRef", [23] = "DeleteLocalRef",
        [24] = "IsSameObject", [25] = "NewLocalRef", [26] = "EnsureLocalCapacity", [27] = "AllocObject", [28] = "NewObject",
        [29] = "NewObjectV", [30] = "NewObjectA", [31] = "GetObjectClass", [32] = "IsInstanceOf", [33] = "GetMethodID",
        [94] = "GetFieldID", [113] = "GetStaticMethodID", [144] = "GetStaticFieldID", [163] = "NewString", [164] = "GetStringLength",
        [165] = "GetStringChars", [166] = "ReleaseStringChars", [167] = "NewStringUTF", [168] = "GetStringUTFLength",
        [169] = "GetStringUTFChars", [170] = "ReleaseStringUTFChars", [171] = "GetArrayLength", [172] = "NewObjectArray",
        [173] = "GetObjectArrayElement", [174] = "SetObjectArrayElement", [215] = "RegisterNatives", [216] = "UnregisterNatives",
        [217] = "MonitorEnter", [218] = "MonitorExit", [219] = "GetJavaVM", [220] = "GetStringRegion", [221] = "GetStringUTFRegion",
        [222] = "GetPrimitiveArrayCritical", [223] = "ReleasePrimitiveArrayCritical", [224] = "GetStringCritical",
        [225] = "ReleaseStringCritical", [226] = "NewWeakGlobalRef", [227] = "DeleteWeakGlobalRef", [228] = "ExceptionCheck",
        [229] = "NewDirectByteBuffer", [230] = "GetDirectBufferAddress", [231] = "GetDirectBufferCapacity", [232] = "GetObjectRefType"};
    static const char *const fam[3] = {"", "V", "A"};
    if (idx < (int)(sizeof base / sizeof *base) && base[idx]) return base[idx];
    if (idx >= 34 && idx < 64) snprintf(buf, cap, "Call%sMethod%s", types[(idx - 34) / 3], fam[(idx - 34) % 3]);
    else if (idx >= 64 && idx < 94) snprintf(buf, cap, "CallNonvirtual%sMethod%s", types[(idx - 64) / 3], fam[(idx - 64) % 3]);
    else if (idx >= 95 && idx < 104) snprintf(buf, cap, "Get%sField", types[idx - 95]);
    else if (idx >= 104 && idx < 113) snprintf(buf, cap, "Set%sField", types[idx - 104]);
    else if (idx >= 114 && idx < 144) snprintf(buf, cap, "CallStatic%sMethod%s", types[(idx - 114) / 3], fam[(idx - 114) % 3]);
    else if (idx >= 145 && idx < 154) snprintf(buf, cap, "GetStatic%sField", types[idx - 145]);
    else if (idx >= 154 && idx < 163) snprintf(buf, cap, "SetStatic%sField", types[idx - 154]);
    else if (idx >= 175 && idx < 183) snprintf(buf, cap, "New%sArray", atypes[idx - 175]);
    else if (idx >= 183 && idx < 191) snprintf(buf, cap, "Get%sArrayElements", atypes[idx - 183]);
    else if (idx >= 191 && idx < 199) snprintf(buf, cap, "Release%sArrayElements", atypes[idx - 191]);
    else if (idx >= 199 && idx < 207) snprintf(buf, cap, "Get%sArrayRegion", atypes[idx - 199]);
    else if (idx >= 207 && idx < 215) snprintf(buf, cap, "Set%sArrayRegion", atypes[idx - 207]);
    else snprintf(buf, cap, "reserved%d", idx);
    return buf;
}

/* Call<Type>Method, CallNonvirtual<Type>Method, CallStatic<Type>Method and NewObject, each in the plain, V and A forms */
static void call_method(cpu_t *c, int idx) {
    int is_new = idx >= 28 && idx <= 30;
    int is_static = idx >= 114 && idx <= 143, nonvirtual = idx >= 64 && idx <= 93;
    int base = is_new ? 28 : is_static ? 114 : nonvirtual ? 64 : 34;
    int mode = (idx - base) % 3;
    int mid_word = nonvirtual ? 3 : 2;
    jobj *recv = get(A(1));
    jobj *m = get(A(mid_word));
    char nm[64];
    if (!m || m->kind != J_METHOD) {
        char what[160];
        snprintf(what, sizeof what, "%s with a bad method ID (%#x): 0 returned", jni_name(idx, nm, sizeof nm), A(mid_word));
        log_once(what);
        ret64(c, 0);
        return;
    }
    uint32_t args[32];
    int n = decode_args(c, m->sig, mode == 0 ? ARGS_VAR : mode == 1 ? ARGS_VA : ARGS_A, mid_word + 1,
                        mode ? A(mid_word + 1) : 0, args);
    (void)n;
    if (is_new) {
        uint32_t h = new_object(recv && recv->cls ? recv->cls : m->cls);
        java_call(m, get(h), args);
        c->r[0] = h;
        return;
    }
    uint64_t v = java_call(m, recv, args);
    ret64(c, v);
}

static void jni_fn(cpu_t *c, uint8_t *mem) {
    (void)mem;
    int idx = (int)((rt_current_hostfn - env_first_slot) / 4);
    if (rt_verbose > 2) {
        char nm[64];
        rt_log("JNIEnv.%s(%#x, %#x, %#x)", jni_name(idx, nm, sizeof nm), A(1), A(2), A(3));
    }
    switch (idx) {
    case 4: c->r[0] = 0x00010006; return;                                  /* GetVersion: JNI 1.6 */
    case 6: c->r[0] = find_class((char *)G(A(1))); return;                 /* FindClass */
    case 10: c->r[0] = class_handle("java/lang/Object"); return;           /* GetSuperclass */
    case 11: c->r[0] = 1; return;                                          /* IsAssignableFrom */
    case 13: case 14: pending_exception = 1; c->r[0] = 0; return;          /* Throw, ThrowNew */
    case 15: c->r[0] = pending_exception ? new_object("java/lang/Throwable") : 0; return; /* ExceptionOccurred */
    case 16: return;                                                       /* ExceptionDescribe */
    case 17: pending_exception = 0; return;                                /* ExceptionClear */
    case 18: rt_fatal("JNI FatalError: %s", (char *)G(A(1)));
    case 19: c->r[0] = 0; return;                                          /* PushLocalFrame */
    case 20: c->r[0] = A(1); return;                                       /* PopLocalFrame: returns its argument */
    case 21: case 25: case 226: c->r[0] = A(1); return;                    /* NewGlobalRef, NewLocalRef, NewWeakGlobalRef */
    case 22: case 23: case 227: return;                                    /* Delete*Ref */
    case 24: c->r[0] = A(1) == A(2); return;                               /* IsSameObject */
    case 26: c->r[0] = 0; return;                                          /* EnsureLocalCapacity */
    case 27: {                                                             /* AllocObject */
        jobj *k = get(A(1));
        c->r[0] = new_object(k && k->cls ? k->cls : "java/lang/Object");
        return;
    }
    case 28: case 29: case 30: call_method(c, idx); return;                /* NewObject, V, A */
    case 31: { jobj *o = get(A(1)); c->r[0] = class_handle(o && o->cls ? o->cls : "java/lang/Object"); return; }
    case 32: c->r[0] = 1; return;                                          /* IsInstanceOf */
    case 33: case 113:                                                     /* Get(Static)MethodID */
        c->r[0] = lookup_method(A(1), (char *)G(A(2)), (char *)G(A(3)), idx == 113);
        return;
    case 94: case 144:                                                     /* Get(Static)FieldID */
        c->r[0] = lookup_field(A(1), (char *)G(A(2)), (char *)G(A(3)), idx == 144);
        return;
    case 163: {                                                            /* NewString */
        char *u = utf16_to_utf8((const uint16_t *)G(A(1)), A(2));
        c->r[0] = rt_jstring(u);
        free(u);
        return;
    }
    case 167: c->r[0] = A(1) ? rt_jstring((char *)G(A(1))) : 0; return;    /* NewStringUTF */
    case 164: c->r[0] = utf16_length(str_of(A(1))); return;                /* GetStringLength */
    case 168: c->r[0] = (uint32_t)strlen(str_of(A(1))); return;            /* GetStringUTFLength */
    case 165: case 224: {                                                  /* GetStringChars, GetStringCritical */
        const char *v = str_of(A(1));
        uint32_t n = utf16_length(v), p = rt_malloc(2 * n + 2);
        utf8_to_utf16(v, (uint16_t *)G(p), n);
        *(uint16_t *)G(p + 2 * n) = 0;
        if (A(2)) rt_mem[A(2)] = 1;
        c->r[0] = p;
        return;
    }
    case 166: case 225: rt_free(A(2)); return;                             /* ReleaseStringChars, ReleaseStringCritical */
    case 169: {                                                            /* GetStringUTFChars: valid for ever */
        jobj *s = get(A(1));
        if (A(2)) rt_mem[A(2)] = 0;
        if (!s || s->kind != J_STRING) { c->r[0] = 0; return; }
        if (!s->utf) s->utf = rt_strdup_guest(s->str ? s->str : "");
        c->r[0] = s->utf;
        return;
    }
    case 170: return;                                                      /* ReleaseStringUTFChars: the copy stays */
    case 220: {                                                            /* GetStringRegion(str, start, len, buf) */
        const char *v = str_of(A(1));
        uint32_t n = utf16_length(v), start = A(2), len = A(3);
        if (start > n || len > n - start) { log_once("GetStringRegion out of range"); pending_exception = 1; return; }
        uint16_t *tmp = malloc(2 * (size_t)n + 2);
        utf8_to_utf16(v, tmp, n);
        memcpy(G(A(4)), tmp + start, 2 * (size_t)len);
        free(tmp);
        return;
    }
    case 221: {                                                            /* GetStringUTFRegion: (approximation: UTF-16 units of ASCII) */
        const char *v = str_of(A(1));
        uint32_t start = A(2), len = A(3), n = (uint32_t)strlen(v);
        if (start > n || len > n - start) { log_once("GetStringUTFRegion out of range"); pending_exception = 1; return; }
        memcpy(G(A(4)), v + start, len);
        rt_mem[A(4) + len] = 0;
        return;
    }
    case 171: { jobj *a = get(A(1)); c->r[0] = a && a->kind == J_ARRAY ? a->length : 0; return; } /* GetArrayLength */
    case 172: {                                                            /* NewObjectArray(len, class, initial) */
        uint32_t h = new_array("L", A(1));
        jobj *a = get(h);
        for (uint32_t k = 0; k < a->length; k++) rt_st32(a->data + 4 * k, A(3));
        c->r[0] = h;
        return;
    }
    case 173: { jobj *a = get(A(1)); c->r[0] = a && A(2) < a->length ? rt_ld32(a->data + 4 * A(2)) : 0; return; }
    case 174: { jobj *a = get(A(1)); if (a && A(2) < a->length) rt_st32(a->data + 4 * A(2), A(3)); return; }
    case 175: case 176: case 177: case 178: case 179: case 180: case 181: case 182: {
        static const char *sigs = "ZBCSIJFD";
        char s[2] = {sigs[idx - 175], 0};
        c->r[0] = new_array(s, A(1));
        return;
    }
    case 183: case 184: case 185: case 186: case 187: case 188: case 189: case 190: case 222: { /* Get*Elements, critical */
        jobj *a = get(A(1));
        if (A(2)) rt_mem[A(2)] = 0; /* no copy: the pointer is the array's own storage */
        c->r[0] = a ? a->data : 0;
        return;
    }
    case 191: case 192: case 193: case 194: case 195: case 196: case 197: case 198: case 223: return; /* Release*: nothing to copy back */
    case 199: case 200: case 201: case 202: case 203: case 204: case 205: case 206:   /* Get*ArrayRegion(arr, start, len, buf) */
    case 207: case 208: case 209: case 210: case 211: case 212: case 213: case 214: { /* Set*ArrayRegion */
        jobj *a = get(A(1));
        uint32_t start = A(2), len = A(3);
        if (!a || a->kind != J_ARRAY || start > a->length || len > a->length - start) {
            log_once("Get/Set<Type>ArrayRegion out of range or bad array (ArrayIndexOutOfBounds)");
            pending_exception = 1;
            return;
        }
        if (idx < 207) memcpy(G(A(4)), G(a->data + start * a->elem), (size_t)len * a->elem);
        else memcpy(G(a->data + start * a->elem), G(A(4)), (size_t)len * a->elem);
        return;
    }
    case 215: c->r[0] = 0; return;                                         /* RegisterNatives: the symbols are exported */
    case 216: c->r[0] = 0; return;
    case 217: case 218: c->r[0] = 0; return;                               /* MonitorEnter/Exit */
    case 219: rt_st32(A(1), vm_addr); c->r[0] = 0; return;                 /* GetJavaVM */
    case 228: c->r[0] = (uint32_t)pending_exception; return;               /* ExceptionCheck */
    case 229: c->r[0] = new_obj((jobj){.kind = J_ARRAY, .cls = dup("B"), .data = A(1), .length = A(2), .elem = 1, .entry = -1}); return;
    case 230: { jobj *a = get(A(1)); c->r[0] = a ? a->data : 0; return; }
    case 231: { jobj *a = get(A(1)); c->r[0] = a ? a->length : 0; return; }
    case 232: c->r[0] = 1; return;                                         /* GetObjectRefType: local */
    default: break;
    }
    if ((idx >= 34 && idx <= 93) || (idx >= 114 && idx <= 143)) { call_method(c, idx); return; }
    if ((idx >= 95 && idx <= 103) || (idx >= 145 && idx <= 153)) {         /* Get(Static)<Type>Field */
        jobj *f = get(A(2));
        if (f && f->kind == J_FIELD && f->entry >= 0 && f->entry < NFIELDS) ret64(c, idx == 145 ? rt_jstring(fields[f->entry].value) : 0);
        else ret64(c, 0);
        return;
    }
    if ((idx >= 104 && idx <= 112) || (idx >= 154 && idx <= 162)) return;  /* Set(Static)<Type>Field */
    char what[96], nm[64];
    snprintf(what, sizeof what, "JNIEnv function %d (%s) is not implemented (returns 0)", idx, jni_name(idx, nm, sizeof nm));
    log_once(what);
    c->r[0] = 0;
}

/* ---- JavaVM -------------------------------------------------------------------------------------------------- */
static void vm_fn(cpu_t *c, uint8_t *mem) {
    (void)mem;
    int idx = (int)((rt_current_hostfn - vm_first_slot) / 4);
    switch (idx) {
    case 4: case 7: rt_st32(A(1), env_addr); c->r[0] = 0; return; /* AttachCurrentThread(AsDaemon): the one env serves every thread */
    case 5: c->r[0] = 0; return;                                   /* DetachCurrentThread */
    case 6: rt_st32(A(1), env_addr); c->r[0] = 0; return;          /* GetEnv (any version, any thread) */
    default: c->r[0] = 0; return;                                  /* DestroyJavaVM */
    }
}

uint32_t rt_jni_init(void) {
    if (vm_addr) return vm_addr;
    default_locale();
    /* JNIEnv: env -> table of 233 host functions */
    uint32_t table = rt_calloc(233, 4);
    for (int k = 0; k < 233; k++) {
        uint32_t a = rt_hostfn_add(jni_fn, "JNIEnv");
        if (k == 0) env_first_slot = a;
        rt_st32(table + 4 * (uint32_t)k, a);
    }
    env_addr = rt_calloc(1, 16);
    rt_st32(env_addr, table);
    /* JavaVM: vm -> table of 8 */
    uint32_t vtable = rt_calloc(8, 4);
    for (int k = 0; k < 8; k++) {
        uint32_t a = rt_hostfn_add(vm_fn, "JavaVM");
        if (k == 0) vm_first_slot = a;
        rt_st32(vtable + 4 * (uint32_t)k, a);
    }
    vm_addr = rt_calloc(1, 16);
    rt_st32(vm_addr, vtable);
    return vm_addr;
}

uint32_t rt_jni_env(void) { return env_addr; }
