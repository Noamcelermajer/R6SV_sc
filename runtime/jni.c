/* A small JVM for the host runner: JavaVM and JNIEnv function tables in game memory whose entries are host
 * functions, Java objects as handles, and an emulation of the Java methods the native code calls.
 *
 * Handles are game addresses in an unmapped range (RT_JOBJ_BASE + index), so the game can store and compare
 * them but never dereference them. Unknown Java calls return 0/null and are logged once each: that log is the
 * list of what to emulate next. On Android the same tables forward to the real JNIEnv instead (not here yet). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define RT_JOBJ_BASE 0x00d00000u
#define MAX_JOBJ 0x40000u

enum { J_CLASS = 1, J_STRING, J_OBJECT, J_ARRAY, J_METHOD, J_FIELD };
typedef struct jobj {
    int kind;
    char *cls;       /* class name (class, object) or element signature (array) */
    char *name;      /* method/field name */
    char *sig;       /* method/field signature */
    int is_static;
    char *str;       /* string value */
    uint32_t data;   /* array storage in game memory */
    uint32_t length; /* array length */
    uint32_t elem;   /* array element size */
} jobj;
static jobj objs[MAX_JOBJ];
static uint32_t nobj = 1;
static volatile int lock_;
static void lock(void) { while (__atomic_exchange_n(&lock_, 1, __ATOMIC_ACQUIRE)) { } }
static void unlock(void) { __atomic_store_n(&lock_, 0, __ATOMIC_RELEASE); }

static uint32_t new_obj(jobj o) {
    lock();
    uint32_t k = nobj++;
    if (k >= MAX_JOBJ) rt_fatal("out of JNI handles");
    objs[k] = o;
    unlock();
    return RT_JOBJ_BASE + k;
}
static jobj *get(uint32_t h) {
    if (h < RT_JOBJ_BASE + 1 || h >= RT_JOBJ_BASE + nobj) return NULL;
    return &objs[h - RT_JOBJ_BASE];
}
static char *dup(const char *s) { return s ? strdup(s) : NULL; }

static uint32_t class_handle(const char *name) {
    for (uint32_t k = 1; k < nobj; k++)
        if (objs[k].kind == J_CLASS && !strcmp(objs[k].cls, name)) return RT_JOBJ_BASE + k;
    return new_obj((jobj){.kind = J_CLASS, .cls = dup(name)});
}
uint32_t rt_jstring(const char *s) { return new_obj((jobj){.kind = J_STRING, .cls = dup("java/lang/String"), .str = dup(s)}); }
static uint32_t new_object(const char *cls) { return new_obj((jobj){.kind = J_OBJECT, .cls = dup(cls)}); }
static uint32_t new_array(const char *elem_sig, uint32_t length) {
    uint32_t size = elem_sig[0] == 'J' || elem_sig[0] == 'D' ? 8 : elem_sig[0] == 'S' || elem_sig[0] == 'C' ? 2
                  : elem_sig[0] == 'Z' || elem_sig[0] == 'B' ? 1 : 4;
    uint32_t data = rt_calloc(length ? length : 1, size);
    return new_obj((jobj){.kind = J_ARRAY, .cls = dup(elem_sig), .data = data, .length = length, .elem = size});
}

/* ---- Java emulation ---------------------------------------------------------------------------------
 * The return is the raw value: an int/boolean/handle in the low word, a long/double in both words. */
typedef struct { uint64_t v; } jret;
static uint64_t jlong_of(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static char *seen[4096];
static void log_once(const char *what) {
    for (int k = 0; k < 4096 && seen[k]; k++) if (!strcmp(seen[k], what)) return;
    for (int k = 0; k < 4096; k++) if (!seen[k]) { seen[k] = strdup(what); break; }
    rt_log("JNI: unemulated %s", what);
}

static int screen_w = 1280, screen_h = 720;
void rt_jni_set_screen(int w, int h) { screen_w = w; screen_h = h; }
static const char *paths[4]; /* obb, resources (data), home, temp */
void rt_jni_set_paths(const char *obb, const char *data, const char *home, const char *temp) {
    paths[0] = obb; paths[1] = data; paths[2] = home; paths[3] = temp;
}
static uint32_t env_addr;

static uint64_t java_call(cpu_t *c, const char *cls, const char *name, const char *sig, uint32_t *args, int nargs) {
    (void)args; (void)nargs;
    char what[512];
    snprintf(what, sizeof what, "%s.%s%s", cls ? cls : "?", name, sig);
    if (rt_verbose) rt_log("JNI call %s", what);
    const char *ret = strchr(sig, ')') + 1;
    /* GL2JNILib.setupPaths(): the Java side works out the folders and calls the native setPaths */
    if (!strcmp(name, "setupPaths")) {
        rt_call(c, rt_export("Java_com_gameloft_android_ANMP_GloftN3HM_GL2JNILib_setPaths"), 6, env_addr,
                rt_jni_class("com/gameloft/android/ANMP/GloftN3HM/GL2JNILib"), rt_jstring(paths[0]),
                rt_jstring(paths[1]), rt_jstring(paths[2]), rt_jstring(paths[3]));
        return 0;
    }
    /* device and system information */
    if (!strcmp(name, "getDeviceName") || !strcmp(name, "getPhoneModel") || !strcmp(name, "getModel")) return rt_jstring("Nexus 7");
    if (!strcmp(name, "getPhoneManufacturer") || !strcmp(name, "getManufacturer")) return rt_jstring("asus");
    if (!strcmp(name, "getPhoneFirmware") || !strcmp(name, "getFirmware") || !strcmp(name, "getSDKVersion")) return *ret == 'I' ? 17 : rt_jstring("4.2.2");
    if (!strcmp(name, "getDeviceLanguage") || !strcmp(name, "getLanguage") || !strcmp(name, "GetPhoneLanguage")) return rt_jstring("en");
    if (!strcmp(name, "getCountry") || !strcmp(name, "getDeviceCountry")) return rt_jstring("US");
    if (!strcmp(name, "getScreenWidth")) return (uint64_t)screen_w;
    if (!strcmp(name, "getScreenHeight")) return (uint64_t)screen_h;
    if (!strcmp(name, "isWifiEnabled") || !strcmp(name, "hasConnectivity") || !strcmp(name, "isOnline")) return 0;
    log_once(what);
    if (*ret == 'L' || *ret == '[') {
        if (!strcmp(ret, "Ljava/lang/String;")) return rt_jstring("");
        return 0;
    }
    if (*ret == 'D') return jlong_of(0.0);
    return 0;
}

static uint64_t java_field(const char *cls, const char *name, const char *sig) {
    char what[512];
    snprintf(what, sizeof what, "field %s.%s %s", cls ? cls : "?", name, sig);
    log_once(what);
    if (!strcmp(sig, "Ljava/lang/String;")) return rt_jstring("");
    return 0;
}

/* ---- argument decoding for Call*Method (varargs), Call*MethodV (va_list) and Call*MethodA (jvalue[]) ---- */
enum { ARGS_VAR, ARGS_VA, ARGS_A };
static int decode_args(cpu_t *c, const char *sig, int mode, int first_word, uint32_t va_or_array, uint32_t *out) {
    rt_args a = {c, first_word, mode == ARGS_VA ? va_or_array : 0};
    int n = 0;
    uint32_t jv = va_or_array;
    for (const char *p = sig + 1; *p && *p != ')'; p++) {
        char t = *p;
        while (*p == '[') p++;
        if (*p == 'L') p = strchr(p, ';');
        if (t == '[') t = 'L';
        if (mode == ARGS_A) {
            out[n++] = rt_ld32(jv);
            if (t == 'J' || t == 'D') out[n++] = rt_ld32(jv + 4);
            jv += 8;
        } else if (t == 'J' || t == 'D') {
            uint64_t v = t == 'J' ? rt_args_u64(&a) : jlong_of(rt_args_double(&a));
            out[n++] = (uint32_t)v;
            out[n++] = (uint32_t)(v >> 32);
        } else if (t == 'F') { /* varargs promote float to double */
            float f = (float)rt_args_double(&a);
            memcpy(&out[n++], &f, 4);
        } else {
            out[n++] = rt_args_u32(&a);
        }
        if (n > 30) break;
    }
    return n;
}

/* ---- the JNIEnv functions ------------------------------------------------------------------------------ */
static uint32_t vm_addr, env_first_slot;
#define A(k) rt_arg(c, k)

static void ret64(cpu_t *c, uint64_t v) { c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }

/* Call<Type>Method families: index -> (static?, nonvirtual?, mode). The value type does not matter: the raw
 * 64-bit result goes to r0:r1 (floats are returned in r0 as bits). */
static void call_method(cpu_t *c, int idx) {
    int is_static = idx >= 114 && idx <= 143, nonvirtual = idx >= 64 && idx <= 93;
    int base = is_static ? 114 : nonvirtual ? 64 : 34;
    int mode = (idx - base) % 3;
    int mid_word = nonvirtual ? 3 : 2;
    jobj *m = get(A(mid_word));
    if (!m || m->kind != J_METHOD) { log_once("call with a bad method ID"); ret64(c, 0); return; }
    uint32_t args[32];
    int n = decode_args(c, m->sig, mode == 0 ? ARGS_VAR : mode == 1 ? ARGS_VA : ARGS_A, mid_word + 1,
                        mode ? A(mid_word + 1) : 0, args);
    uint64_t v = java_call(c, m->cls, m->name, m->sig, args, n);
    const char *ret = strchr(m->sig, ')') + 1;
    if (*ret == 'F') { double d; memcpy(&d, &v, 8); float f = (float)d; uint32_t u; memcpy(&u, &f, 4); v = u; }
    ret64(c, v);
}

static void jni_fn(cpu_t *c, uint8_t *m) {
    (void)m;
    int idx = (int)((rt_current_hostfn - env_first_slot) / 4);
    if (rt_verbose > 1) {
        jobj *o = get(A(1));
        rt_log("JNIEnv[%d](%#x, %#x, %#x) %s %s", idx, A(1), A(2), A(3), o && o->cls ? o->cls : "",
               idx == 6 ? (char *)G(A(1)) : idx == 33 || idx == 113 || idx == 94 || idx == 144 ? (char *)G(A(2)) : "");
    }
    switch (idx) {
    case 4: c->r[0] = 0x00010006; return;                                  /* GetVersion: JNI 1.6 */
    case 6: c->r[0] = class_handle((char *)G(A(1))); return;              /* FindClass */
    case 10: c->r[0] = class_handle("java/lang/Object"); return;          /* GetSuperclass */
    case 11: c->r[0] = 1; return;                                          /* IsAssignableFrom */
    case 13: case 14: c->r[0] = 0; return;                                 /* Throw, ThrowNew */
    case 15: c->r[0] = 0; return;                                          /* ExceptionOccurred */
    case 16: case 17: return;                                              /* ExceptionDescribe, ExceptionClear */
    case 18: rt_fatal("JNI FatalError: %s", (char *)G(A(1)));
    case 19: c->r[0] = 0; return;                                          /* PushLocalFrame */
    case 20: return;                                                       /* PopLocalFrame: returns its argument */
    case 21: case 25: case 226: c->r[0] = A(1); return;                    /* NewGlobalRef, NewLocalRef, NewWeakGlobalRef */
    case 22: case 23: case 227: return;                                    /* Delete*Ref */
    case 24: c->r[0] = A(1) == A(2); return;                               /* IsSameObject */
    case 26: c->r[0] = 0; return;                                          /* EnsureLocalCapacity */
    case 27: case 28: case 29: case 30: {                                  /* AllocObject, NewObject* */
        jobj *k = get(A(1));
        c->r[0] = new_object(k ? k->cls : "?");
        return;
    }
    case 31: { jobj *o = get(A(1)); c->r[0] = class_handle(o && o->cls ? o->cls : "java/lang/Object"); return; }
    case 32: c->r[0] = 1; return;                                          /* IsInstanceOf */
    case 33: case 113: case 94: case 144: {                                /* Get(Static)MethodID / FieldID */
        jobj *k = get(A(1));
        c->r[0] = new_obj((jobj){.kind = idx == 33 || idx == 113 ? J_METHOD : J_FIELD, .cls = dup(k ? k->cls : "?"),
                                 .name = dup((char *)G(A(2))), .sig = dup((char *)G(A(3))),
                                 .is_static = idx == 113 || idx == 144});
        return;
    }
    case 163: case 167: {                                                  /* NewString, NewStringUTF */
        if (idx == 167) { c->r[0] = rt_jstring((char *)G(A(1))); return; }
        char buf[4096];
        uint32_t n = A(2) < sizeof buf - 1 ? A(2) : sizeof buf - 1;
        for (uint32_t k = 0; k < n; k++) buf[k] = (char)*(uint16_t *)G(A(1) + 2 * k);
        buf[n] = 0;
        c->r[0] = rt_jstring(buf);
        return;
    }
    case 164: case 168: { jobj *s = get(A(1)); c->r[0] = s && s->str ? (uint32_t)strlen(s->str) : 0; return; }
    case 165: case 224: {                                                  /* GetStringChars, GetStringCritical */
        jobj *s = get(A(1));
        const char *v = s && s->str ? s->str : "";
        uint32_t n = (uint32_t)strlen(v), p = rt_malloc(2 * n + 2);
        for (uint32_t k = 0; k <= n; k++) *(uint16_t *)G(p + 2 * k) = (uint8_t)v[k];
        if (A(2)) rt_mem[A(2)] = 1;
        c->r[0] = p;
        return;
    }
    case 169: {                                                            /* GetStringUTFChars */
        jobj *s = get(A(1));
        if (A(2)) rt_mem[A(2)] = 1;
        c->r[0] = rt_strdup_guest(s && s->str ? s->str : "");
        return;
    }
    case 166: case 170: case 225: rt_free(A(2)); return;                   /* Release* */
    case 220: {                                                            /* GetStringRegion */
        jobj *s = get(A(1));
        const char *v = s && s->str ? s->str : "";
        for (uint32_t k = 0; k < A(3); k++) *(uint16_t *)G(A(4) + 2 * k) = (uint8_t)v[A(2) + k];
        return;
    }
    case 221: {                                                            /* GetStringUTFRegion */
        jobj *s = get(A(1));
        const char *v = s && s->str ? s->str : "";
        memcpy(G(A(4)), v + A(2), A(3));
        rt_mem[A(4) + A(3)] = 0;
        return;
    }
    case 171: { jobj *a = get(A(1)); c->r[0] = a ? a->length : 0; return; } /* GetArrayLength */
    case 172: c->r[0] = new_array("L", A(1)); return;                    /* NewObjectArray */
    case 173: { jobj *a = get(A(1)); c->r[0] = a && A(2) < a->length ? rt_ld32(a->data + 4 * A(2)) : 0; return; }
    case 174: { jobj *a = get(A(1)); if (a && A(2) < a->length) rt_st32(a->data + 4 * A(2), A(3)); return; }
    case 175: case 176: case 177: case 178: case 179: case 180: case 181: case 182: {
        static const char *sigs = "ZBCSIJFD";
        char s[2] = {sigs[idx - 175], 0};
        c->r[0] = new_array(s, A(1));
        return;
    }
    case 183: case 184: case 185: case 186: case 187: case 188: case 189: case 190: case 222: { /* Get*Elements */
        jobj *a = get(A(1));
        if (A(2)) rt_mem[A(2)] = 0;
        c->r[0] = a ? a->data : 0;
        return;
    }
    case 191: case 192: case 193: case 194: case 195: case 196: case 197: case 198: case 223: return;
    case 199: case 200: case 201: case 202: case 203: case 204: case 205: case 206: { /* Get*ArrayRegion */
        jobj *a = get(A(1));
        if (a) memcpy(G(A(4)), G(a->data + A(2) * a->elem), A(3) * a->elem);
        return;
    }
    case 207: case 208: case 209: case 210: case 211: case 212: case 213: case 214: { /* Set*ArrayRegion */
        jobj *a = get(A(1));
        if (a) memcpy(G(a->data + A(2) * a->elem), G(A(4)), A(3) * a->elem);
        return;
    }
    case 215: c->r[0] = 0; return;                                         /* RegisterNatives */
    case 216: c->r[0] = 0; return;
    case 217: case 218: c->r[0] = 0; return;                               /* MonitorEnter/Exit */
    case 219: rt_st32(A(1), vm_addr); c->r[0] = 0; return;               /* GetJavaVM */
    case 228: c->r[0] = 0; return;                                         /* ExceptionCheck */
    case 229: c->r[0] = new_obj((jobj){.kind = J_ARRAY, .cls = dup("B"), .data = A(1), .length = A(2), .elem = 1}); return;
    case 230: { jobj *a = get(A(1)); c->r[0] = a ? a->data : 0; return; }
    case 231: { jobj *a = get(A(1)); c->r[0] = a ? a->length : 0; return; }
    case 232: c->r[0] = 1; return;                                         /* GetObjectRefType: local */
    default: break;
    }
    if ((idx >= 34 && idx <= 93) || (idx >= 114 && idx <= 143)) { call_method(c, idx); return; }
    if ((idx >= 95 && idx <= 103) || (idx >= 145 && idx <= 153)) {        /* Get(Static)<Type>Field */
        jobj *f = get(A(2));
        ret64(c, f ? java_field(f->cls, f->name, f->sig) : 0);
        return;
    }
    if ((idx >= 104 && idx <= 112) || (idx >= 154 && idx <= 162)) return; /* Set(Static)<Type>Field */
    char what[64];
    snprintf(what, sizeof what, "JNIEnv function %d", idx);
    log_once(what);
    c->r[0] = 0;
}

/* ---- JavaVM -------------------------------------------------------------------------------------------- */
static uint32_t vm_first_slot;
static void vm_fn(cpu_t *c, uint8_t *m) {
    (void)m;
    int idx = (int)((rt_current_hostfn - vm_first_slot) / 4);
    switch (idx) {
    case 4: case 7: rt_st32(A(1), env_addr); c->r[0] = 0; return; /* AttachCurrentThread(AsDaemon) */
    case 5: c->r[0] = 0; return;                                   /* DetachCurrentThread */
    case 6: rt_st32(A(1), env_addr); c->r[0] = 0; return;          /* GetEnv */
    default: c->r[0] = 0; return;
    }
}

uint32_t rt_jni_init(void) {
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
uint32_t rt_jni_class(const char *name) { return class_handle(name); }
