/* pthread and semaphore imports. Game-side objects keep bionic's 32-bit layouts (a mutex, condition or
 * semaphore is 4 bytes); each maps to a host object through a table keyed by its game address, created on
 * first use so static initialisers work. Every game thread is a host thread with its own CPU state. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <time.h>
#endif

#include "rt.h"

#define A(k) rt_arg(c, k)
#define RET(v) rt_ret(c, (uint32_t)(v))
#define REG(name) rt_import_register(#name, imp_##name)
#define IMP(name) static void imp_##name(cpu_t *c)

/* ---- host primitives -------------------------------------------------------------------------- */
#ifdef _WIN32
typedef CRITICAL_SECTION hmutex;
typedef CONDITION_VARIABLE hcond;
typedef HANDLE hsem;
static void hm_init(hmutex *m) { InitializeCriticalSection(m); }
static void hm_lock(hmutex *m) { EnterCriticalSection(m); }
static int hm_trylock(hmutex *m) { return TryEnterCriticalSection(m) ? 0 : 16; }
static void hm_unlock(hmutex *m) { LeaveCriticalSection(m); }
static void hc_init(hcond *cv) { InitializeConditionVariable(cv); }
static void hc_wait(hcond *cv, hmutex *m) { SleepConditionVariableCS(cv, m, INFINITE); }
static int hc_timedwait(hcond *cv, hmutex *m, uint32_t ms) { return SleepConditionVariableCS(cv, m, ms) ? 0 : 110; }
static void hc_signal(hcond *cv) { WakeConditionVariable(cv); }
static void hc_broadcast(hcond *cv) { WakeAllConditionVariable(cv); }
static void hs_init(hsem *s, uint32_t v) { *s = CreateSemaphoreA(NULL, (LONG)v, 0x7FFFFFFF, NULL); }
static void hs_post(hsem *s) { ReleaseSemaphore(*s, 1, NULL); }
static void hs_wait(hsem *s) { WaitForSingleObject(*s, INFINITE); }
static int hs_trywait(hsem *s) { return WaitForSingleObject(*s, 0) == WAIT_OBJECT_0 ? 0 : -1; }
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
typedef pthread_mutex_t hmutex;
typedef pthread_cond_t hcond;
typedef sem_t hsem;
static void hm_init(hmutex *m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
}
static void hm_lock(hmutex *m) { pthread_mutex_lock(m); }
static int hm_trylock(hmutex *m) { return pthread_mutex_trylock(m); }
static void hm_unlock(hmutex *m) { pthread_mutex_unlock(m); }
static void hc_init(hcond *cv) { pthread_cond_init(cv, NULL); }
static void hc_wait(hcond *cv, hmutex *m) { pthread_cond_wait(cv, m); }
static int hc_timedwait(hcond *cv, hmutex *m, uint32_t ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(cv, m, &ts) ? 110 : 0;
}
static void hc_signal(hcond *cv) { pthread_cond_signal(cv); }
static void hc_broadcast(hcond *cv) { pthread_cond_broadcast(cv); }
static void hs_init(hsem *s, uint32_t v) { sem_init(s, 0, v); }
static void hs_post(hsem *s) { sem_post(s); }
static void hs_wait(hsem *s) { sem_wait(s); }
static int hs_trywait(hsem *s) { return sem_trywait(s); }
static uint64_t now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000; }
#endif

/* ---- game address -> host object ------------------------------------------------------------------- */
enum { K_MUTEX = 1, K_COND, K_SEM };
#define OBJ_SLOTS 65536
static struct { uint32_t addr; int kind; void *obj; } objs[OBJ_SLOTS];
static volatile int objs_lock;

static void *obj_get(uint32_t addr, int kind, uint32_t init) {
    uint32_t h = (addr >> 2) * 2654435761u;
    void *found = NULL;
    while (__atomic_exchange_n(&objs_lock, 1, __ATOMIC_ACQUIRE)) { }
    for (uint32_t k = 0; k < OBJ_SLOTS; k++) {
        uint32_t i = (h + k) & (OBJ_SLOTS - 1);
        if (objs[i].addr == addr && objs[i].kind == kind) { found = objs[i].obj; break; }
        if (!objs[i].addr) {
            if (kind == K_MUTEX) { found = malloc(sizeof(hmutex)); hm_init(found); }
            else if (kind == K_COND) { found = malloc(sizeof(hcond)); hc_init(found); }
            else { found = malloc(sizeof(hsem)); hs_init(found, init); }
            objs[i].addr = addr; objs[i].kind = kind; objs[i].obj = found;
            break;
        }
    }
    __atomic_store_n(&objs_lock, 0, __ATOMIC_RELEASE);
    if (!found) rt_fatal("too many synchronisation objects");
    return found;
}

/* ---- mutexes (always recursive on the host: a non-recursive relock would deadlock on the device too) -- */
IMP(pthread_mutexattr_init) { rt_st32(A(0), 0); RET(0); }
IMP(pthread_mutexattr_settype) { rt_st32(A(0), A(1)); RET(0); }
IMP(pthread_mutexattr_destroy) { RET(0); }
IMP(pthread_mutex_init) { rt_st32(A(0), A(1) && rt_ld32(A(1)) == 1 ? 0x4000u : 0); obj_get(A(0), K_MUTEX, 0); RET(0); }
IMP(pthread_mutex_destroy) { RET(0); }
IMP(pthread_mutex_lock) { hm_lock(obj_get(A(0), K_MUTEX, 0)); RET(0); }
IMP(pthread_mutex_trylock) { RET(hm_trylock(obj_get(A(0), K_MUTEX, 0))); }
IMP(pthread_mutex_unlock) { hm_unlock(obj_get(A(0), K_MUTEX, 0)); RET(0); }

/* ---- condition variables ---------------------------------------------------------------------------- */
IMP(pthread_cond_init) { obj_get(A(0), K_COND, 0); RET(0); }
IMP(pthread_cond_destroy) { RET(0); }
IMP(pthread_cond_signal) { hc_signal(obj_get(A(0), K_COND, 0)); RET(0); }
IMP(pthread_cond_broadcast) { hc_broadcast(obj_get(A(0), K_COND, 0)); RET(0); }
IMP(pthread_cond_wait) { hc_wait(obj_get(A(0), K_COND, 0), obj_get(A(1), K_MUTEX, 0)); RET(0); }
IMP(pthread_cond_timedwait) {
    uint32_t ts = A(2);
    uint64_t deadline = (uint64_t)rt_ld32(ts) * 1000 + rt_ld32(ts + 4) / 1000000;
    uint64_t wall;
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    wall = ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000) - 11644473600000ull;
#else
    wall = now_ms();
#endif
    uint32_t ms = deadline > wall ? (uint32_t)(deadline - wall) : 0;
    RET(hc_timedwait(obj_get(A(0), K_COND, 0), obj_get(A(1), K_MUTEX, 0), ms));
}

/* ---- semaphores --------------------------------------------------------------------------------------- */
IMP(sem_init) { obj_get(A(0), K_SEM, A(2)); RET(0); }
IMP(sem_destroy) { RET(0); }
IMP(sem_post) { hs_post(obj_get(A(0), K_SEM, 0)); RET(0); }
IMP(sem_wait) { hs_wait(obj_get(A(0), K_SEM, 0)); RET(0); }
IMP(sem_trywait) { RET(hs_trywait(obj_get(A(0), K_SEM, 0))); }

/* ---- keys ----------------------------------------------------------------------------------------------- */
#define MAX_KEYS 128
static volatile int next_key = 1;
static __thread uint32_t key_values[MAX_KEYS];
IMP(pthread_key_create) {
    int k = __atomic_fetch_add(&next_key, 1, __ATOMIC_SEQ_CST);
    if (k >= MAX_KEYS) { RET(11); return; }
    rt_st32(A(0), (uint32_t)k);
    RET(0);
}
IMP(pthread_key_delete) { RET(0); }
IMP(pthread_getspecific) { RET(A(0) < MAX_KEYS ? key_values[A(0)] : 0); }
IMP(pthread_setspecific) { if (A(0) < MAX_KEYS) key_values[A(0)] = A(1); RET(0); }

/* ---- once -------------------------------------------------------------------------------------------- */
static hmutex once_lock;
IMP(pthread_once) {
    hm_lock(&once_lock);
    if (rt_ld32(A(0)) == 0) {
        rt_st32(A(0), 1);
        rt_call(c, A(1), 0);
    }
    hm_unlock(&once_lock);
    RET(0);
}

/* ---- threads -------------------------------------------------------------------------------------------- */
typedef struct gthread {
    uint32_t id, start, arg, result;
    jmp_buf exit_jmp;
#ifdef _WIN32
    HANDLE h;
#else
    pthread_t h;
#endif
} gthread;
#define MAX_THREADS 256
static gthread *threads[MAX_THREADS];
static volatile uint32_t next_tid = 2; /* 1 is the main thread */
static __thread gthread *self;

#ifdef _WIN32
static DWORD WINAPI thread_main(void *p)
#else
static void *thread_main(void *p)
#endif
{
    gthread *t = p;
    self = t;
    cpu_t *c = rt_thread_cpu();
    if (!setjmp(t->exit_jmp)) t->result = rt_call(c, t->start, 1, t->arg);
    return 0;
}

IMP(pthread_create) {
    gthread *t = calloc(1, sizeof *t);
    t->id = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
    t->start = A(2);
    t->arg = A(3);
    for (int k = 0; k < MAX_THREADS; k++) if (!threads[k]) { threads[k] = t; break; }
    rt_st32(A(0), t->id);
    if (rt_verbose) rt_log("pthread_create: thread %u at %#x", t->id, t->start);
#ifdef _WIN32
    t->h = CreateThread(NULL, 1 << 20, thread_main, t, 0, NULL);
    RET(t->h ? 0 : 11);
#else
    RET(pthread_create(&t->h, NULL, thread_main, t) ? 11 : 0);
#endif
}
static gthread *find_thread(uint32_t id) { for (int k = 0; k < MAX_THREADS; k++) if (threads[k] && threads[k]->id == id) return threads[k]; return NULL; }
IMP(pthread_join) {
    gthread *t = find_thread(A(0));
    if (!t) { RET(3); return; }
#ifdef _WIN32
    WaitForSingleObject(t->h, INFINITE);
#else
    pthread_join(t->h, NULL);
#endif
    if (A(1)) rt_st32(A(1), t->result);
    RET(0);
}
IMP(pthread_exit) {
    if (!self) rt_fatal("pthread_exit on the main thread");
    self->result = A(0);
    longjmp(self->exit_jmp, 1); /* only host C frames and translated code (no destructors) lie in between */
}
IMP(pthread_self) { RET(self ? self->id : 1); }
IMP(pthread_equal) { RET(A(0) == A(1)); }
IMP(pthread_kill) { RET(0); }
IMP(pthread_attr_init) { memset(G(A(0)), 0, 24); RET(0); }
IMP(pthread_attr_destroy) { RET(0); }
IMP(pthread_attr_setdetachstate) { RET(0); }
IMP(pthread_attr_setstacksize) { RET(0); }
IMP(sched_yield) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
    RET(0);
}

void rt_pthread_register(void) {
    hm_init(&once_lock);
    (void)now_ms;
    REG(pthread_mutexattr_init); REG(pthread_mutexattr_settype); REG(pthread_mutexattr_destroy);
    REG(pthread_mutex_init); REG(pthread_mutex_destroy); REG(pthread_mutex_lock); REG(pthread_mutex_trylock);
    REG(pthread_mutex_unlock); REG(pthread_cond_init); REG(pthread_cond_destroy); REG(pthread_cond_signal);
    REG(pthread_cond_broadcast); REG(pthread_cond_wait); REG(pthread_cond_timedwait); REG(sem_init);
    REG(sem_destroy); REG(sem_post); REG(sem_wait); REG(sem_trywait); REG(pthread_key_create);
    REG(pthread_key_delete); REG(pthread_getspecific); REG(pthread_setspecific); REG(pthread_once);
    REG(pthread_create); REG(pthread_join); REG(pthread_exit); REG(pthread_self); REG(pthread_equal);
    REG(pthread_kill); REG(pthread_attr_init); REG(pthread_attr_destroy); REG(pthread_attr_setdetachstate);
    REG(pthread_attr_setstacksize); REG(sched_yield);
}
