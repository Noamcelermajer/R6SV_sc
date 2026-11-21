/* OpenSL ES for the game's audio engine (vox): an engine object, output mixes, and audio players fed by a buffer
 * queue, on top of a 16-bit PCM stream from the platform backend (audio_win.c, audio_android.c).
 *
 * An OpenSL object or interface is a pointer to a pointer to a table of functions. Each object is a block in game
 * memory whose words hold the addresses of method tables of host functions, one word per interface:
 *   +0 SLObjectItf  +4 SLEngineItf  +8 SLPlayItf  +12 SLBufferQueueItf
 * so an interface pointer, rounded down to the block, finds the object. The game enqueues buffers of PCM; the
 * backend pulls samples from the queue; when a buffer has been played, a thread of the player calls the game's
 * buffer-queue callback (never the backend's real-time callback, which must not run game code).
 */
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "rt.h"

#define A(k) rt_arg(c, k)
#define SL_SUCCESS 0
#define SL_PARAMETER_INVALID 2
#define SL_MEMORY_FAILURE 3
#define SL_FEATURE_UNSUPPORTED 12
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3

/* ---- small portable lock and event ---------------------------------------------------------------------- */
#ifdef _WIN32
typedef struct { CRITICAL_SECTION cs; CONDITION_VARIABLE cv; } sync_t;
static void sync_init(sync_t *s) { InitializeCriticalSection(&s->cs); InitializeConditionVariable(&s->cv); }
static void sync_lock(sync_t *s) { EnterCriticalSection(&s->cs); }
static void sync_unlock(sync_t *s) { LeaveCriticalSection(&s->cs); }
static void sync_wait(sync_t *s) { SleepConditionVariableCS(&s->cv, &s->cs, 100); }
static void sync_signal(sync_t *s) { WakeAllConditionVariable(&s->cv); }
#else
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; } sync_t;
static void sync_init(sync_t *s) { pthread_mutex_init(&s->mu, NULL); pthread_cond_init(&s->cv, NULL); }
static void sync_lock(sync_t *s) { pthread_mutex_lock(&s->mu); }
static void sync_unlock(sync_t *s) { pthread_mutex_unlock(&s->mu); }
static void sync_wait(sync_t *s) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&s->cv, &s->mu, &ts);
}
static void sync_signal(sync_t *s) { pthread_cond_broadcast(&s->cv); }
#endif

/* ---- objects ---------------------------------------------------------------------------------------------- */
enum { K_ENGINE = 1, K_MIX, K_PLAYER };
#define QMAX 64
typedef struct sl_obj {
    uint32_t block; /* game address of the interface words */
    int kind;
    /* player */
    int channels, rate, bits, state;
    uint8_t *qdata[QMAX];  /* a copy of each enqueued buffer (see bq_Enqueue) */
    uint32_t qsize[QMAX];
    int qhead, qcount;
    int qlimit;         /* the locator's numBuffers: Enqueue beyond it fails, as on Android */
    uint32_t qoff;      /* bytes of the head buffer already played */
    uint32_t qplayed;   /* buffers played since the last callback */
    uint32_t cb, cb_ctx;
    sync_t sync;
    void *stream;
    int thread_started, destroyed;
} sl_obj;

#define MAX_OBJS 64
static sl_obj *objs[MAX_OBJS];
static uint32_t vt_object, vt_engine, vt_play, vt_bq;
static int debug_left = -1; /* NOVA_AUDIO_DEBUG: log the first buffer-queue operations */

static sl_obj *find(uint32_t itf) {
    uint32_t block = itf & ~15u;
    for (int k = 0; k < MAX_OBJS; k++) if (objs[k] && objs[k]->block == block) return objs[k];
    return NULL;
}

static uint32_t new_object(int kind) {
    sl_obj *o = calloc(1, sizeof *o);
    o->kind = kind;
    o->block = rt_malloc(16); /* 16-byte aligned: interface pointers round down to it */
    rt_st32(o->block, vt_object);
    rt_st32(o->block + 4, vt_engine);
    rt_st32(o->block + 8, vt_play);
    rt_st32(o->block + 12, vt_bq);
    sync_init(&o->sync);
    for (int k = 0; k < MAX_OBJS; k++) if (!objs[k]) { objs[k] = o; break; }
    return o->block;
}

static int iid_is(uint32_t iid, int which) {
    return iid && (iid == rt_sl_iid[which] || !memcmp(G(iid), G(rt_sl_iid[which]), 16));
}

/* ---- the backend pulls samples from the player's queue ------------------------------------------------------ */
static void pull(void *user, int16_t *out, int frames) {
    sl_obj *o = user;
    uint32_t want = (uint32_t)frames * (uint32_t)o->channels * 2, got = 0;
    sync_lock(&o->sync);
    while (got < want && o->state == SL_PLAYSTATE_PLAYING && o->qcount) {
        uint32_t size = o->qsize[o->qhead];
        uint32_t n = size - o->qoff < want - got ? size - o->qoff : want - got;
        memcpy((uint8_t *)out + got, o->qdata[o->qhead] + o->qoff, n);
        got += n;
        o->qoff += n;
        if (o->qoff >= size) {
            o->qhead = (o->qhead + 1) % QMAX;
            o->qcount--;
            o->qoff = 0;
            o->qplayed++;
        }
    }
    if (o->qplayed) sync_signal(&o->sync);
    sync_unlock(&o->sync);
    if (got < want) memset((uint8_t *)out + got, 0, want - got); /* underrun or paused: silence */
}

/* the player's thread: calls the game's buffer-queue callback once per played buffer */
#ifdef _WIN32
static DWORD WINAPI callback_thread(void *p)
#else
static void *callback_thread(void *p)
#endif
{
    sl_obj *o = p;
    cpu_t *c = rt_thread_cpu();
    sync_lock(&o->sync);
    while (!o->destroyed) {
        if (!o->qplayed) { sync_wait(&o->sync); continue; }
        o->qplayed--;
        uint32_t cb = o->cb, ctx = o->cb_ctx;
        sync_unlock(&o->sync);
        if (debug_left > 0) { debug_left--; rt_log("callback"); }
        if (cb) rt_call(c, cb, 2, o->block + 12, ctx);
        sync_lock(&o->sync);
    }
    sync_unlock(&o->sync);
    return 0;
}

static void start_thread(sl_obj *o) {
    if (o->thread_started) return;
    o->thread_started = 1;
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 1 << 20, callback_thread, o, 0, NULL));
#else
    pthread_t t;
    pthread_create(&t, NULL, callback_thread, o);
    pthread_detach(t);
#endif
}

/* ---- SLObjectItf ------------------------------------------------------------------------------------------ */
static void obj_Realize(cpu_t *c, uint8_t *m) { (void)m; c->r[0] = SL_SUCCESS; }
static void obj_Resume(cpu_t *c, uint8_t *m) { (void)m; c->r[0] = SL_SUCCESS; }
static void obj_GetState(cpu_t *c, uint8_t *m) { (void)m; if (A(1)) rt_st32(A(1), 2 /* REALIZED */); c->r[0] = SL_SUCCESS; }
static void obj_GetInterface(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    uint32_t iid = A(1), out = A(2), itf = 0;
    if (o && o->kind == K_ENGINE && iid_is(iid, 0)) itf = o->block + 4;
    else if (o && o->kind == K_PLAYER && iid_is(iid, 1)) itf = o->block + 8;
    else if (o && o->kind == K_PLAYER && iid_is(iid, 2)) itf = o->block + 12;
    if (out) rt_st32(out, itf);
    c->r[0] = itf ? SL_SUCCESS : SL_FEATURE_UNSUPPORTED;
}
static void obj_RegisterCallback(cpu_t *c, uint8_t *m) { (void)m; c->r[0] = SL_SUCCESS; }
static void obj_AbortAsyncOperation(cpu_t *c, uint8_t *m) { (void)c; (void)m; }
static void obj_Destroy(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    if (!o) return;
    if (o->stream) { rt_audio_stop(o->stream); rt_audio_close(o->stream); o->stream = NULL; }
    sync_lock(&o->sync);
    o->destroyed = 1; /* the callback thread exits; the object stays allocated in case it is still running */
    o->state = SL_PLAYSTATE_STOPPED;
    sync_signal(&o->sync);
    sync_unlock(&o->sync);
    for (int k = 0; k < MAX_OBJS; k++) if (objs[k] == o) objs[k] = NULL;
}
static void unsupported(cpu_t *c, uint8_t *m) { (void)m; c->r[0] = SL_FEATURE_UNSUPPORTED; }

/* ---- SLEngineItf ------------------------------------------------------------------------------------------ */
static void eng_CreateOutputMix(cpu_t *c, uint8_t *m) {
    (void)m;
    rt_st32(A(1), new_object(K_MIX));
    c->r[0] = SL_SUCCESS;
}
static void eng_CreateAudioPlayer(cpu_t *c, uint8_t *m) {
    (void)m;
    uint32_t out = A(1), src = A(2);
    uint32_t fmt = src ? rt_ld32(src + 4) : 0;
    if (!fmt || rt_ld32(fmt) != 2 /* SL_DATAFORMAT_PCM */) { c->r[0] = SL_PARAMETER_INVALID; return; }
    uint32_t block = new_object(K_PLAYER);
    sl_obj *o = find(block);
    o->channels = (int)rt_ld32(fmt + 4);
    o->rate = (int)(rt_ld32(fmt + 8) / 1000); /* milliHertz */
    o->bits = (int)rt_ld32(fmt + 12);
    o->state = SL_PLAYSTATE_STOPPED;
    /* The game refills its few buffers in turn and relies on Enqueue failing while they are all queued; accepting
     * more would let it overwrite audio that has not played yet. */
    uint32_t loc = rt_ld32(src);
    uint32_t nbuf = loc ? rt_ld32(loc + 4) : 0;
    o->qlimit = nbuf >= 1 && nbuf <= QMAX ? (int)nbuf : 2;
    rt_log("OpenSL: audio player %d Hz, %d channels, %d bits, %d buffers", o->rate, o->channels, o->bits, o->qlimit);
    if (o->bits != 16) rt_log("OpenSL: only 16-bit PCM is supported");
    o->stream = rt_audio_open(o->rate, o->channels, pull, o);
    if (!o->stream) rt_log("OpenSL: the audio backend could not open a stream");
    rt_st32(out, block);
    c->r[0] = SL_SUCCESS;
}

/* ---- SLPlayItf -------------------------------------------------------------------------------------------- */
static void play_SetPlayState(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    if (!o) { c->r[0] = SL_PARAMETER_INVALID; return; }
    uint32_t st = A(1);
    sync_lock(&o->sync);
    o->state = (int)st;
    if (st == SL_PLAYSTATE_STOPPED) { o->qcount = 0; o->qoff = 0; o->qplayed = 0; }
    sync_unlock(&o->sync);
    if (st == SL_PLAYSTATE_PLAYING) {
        start_thread(o);
        if (o->stream) rt_audio_start(o->stream);
    }
    c->r[0] = SL_SUCCESS;
}
static void play_GetPlayState(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    if (A(1)) rt_st32(A(1), o ? (uint32_t)o->state : SL_PLAYSTATE_STOPPED);
    c->r[0] = SL_SUCCESS;
}
static void play_zero(cpu_t *c, uint8_t *m) { (void)m; if (A(1)) rt_st32(A(1), 0); c->r[0] = SL_SUCCESS; }
static void play_ok(cpu_t *c, uint8_t *m) { (void)m; c->r[0] = SL_SUCCESS; }

/* ---- SLBufferQueueItf ------------------------------------------------------------------------------------- */
static void bq_Enqueue(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    if (debug_left < 0) debug_left = getenv("NOVA_AUDIO_DEBUG") ? 400 : 0;
    if (debug_left > 0 && o) {
        debug_left--;
        rt_log("Enqueue %#x %u bytes (queued %d, head %d, off %u)", A(1), A(2), o->qcount, o->qhead, o->qoff);
    }
    if (!o) { c->r[0] = SL_PARAMETER_INVALID; return; }
    sync_lock(&o->sync);
    if (o->qcount >= o->qlimit) { sync_unlock(&o->sync); c->r[0] = 7 /* SL_RESULT_BUFFER_INSUFFICIENT */; return; }
    int tail = (o->qhead + o->qcount) % QMAX;
    /* vox refills one buffer and enqueues it again while the previous enqueue of it may still be queued: Android
     * copies a buffer out as soon as it reaches the head, so the game never noticed. Copying at enqueue gives the
     * same result. */
    o->qdata[tail] = realloc(o->qdata[tail], A(2) ? A(2) : 1);
    memcpy(o->qdata[tail], G(A(1)), A(2));
    o->qsize[tail] = A(2);
    o->qcount++;
    sync_unlock(&o->sync);
    c->r[0] = SL_SUCCESS;
}
static void bq_Clear(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    if (o) { sync_lock(&o->sync); o->qcount = 0; o->qoff = 0; sync_unlock(&o->sync); }
    c->r[0] = SL_SUCCESS;
}
static void bq_GetState(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    if (debug_left > 0 && o) { debug_left--; rt_log("GetState -> count %d, index %d", o->qcount, o->qhead); }
    if (A(1)) {
        rt_st32(A(1), o ? (uint32_t)o->qcount : 0);
        rt_st32(A(1) + 4, o ? (uint32_t)o->qhead : 0);
    }
    c->r[0] = SL_SUCCESS;
}
static void bq_RegisterCallback(cpu_t *c, uint8_t *m) {
    (void)m;
    sl_obj *o = find(A(0));
    if (o) { sync_lock(&o->sync); o->cb = A(1); o->cb_ctx = A(2); sync_unlock(&o->sync); }
    c->r[0] = SL_SUCCESS;
}

/* ---- tables and slCreateEngine ------------------------------------------------------------------------------ */
static uint32_t table(rt_hostfn *fns, const char *const *names, int n) {
    uint32_t t = rt_calloc((uint32_t)n, 4);
    for (int k = 0; k < n; k++) rt_st32(t + 4 * (uint32_t)k, rt_hostfn_add(fns[k] ? fns[k] : unsupported, names[k]));
    return t;
}

static void imp_slCreateEngine(cpu_t *c) {
    if (!vt_object) {
        rt_hostfn obj[10] = {obj_Realize, obj_Resume, obj_GetState, obj_GetInterface, obj_RegisterCallback,
                             obj_AbortAsyncOperation, obj_Destroy, NULL, NULL, NULL};
        static const char *const obj_n[10] = {"SLObject.Realize", "SLObject.Resume", "SLObject.GetState",
            "SLObject.GetInterface", "SLObject.RegisterCallback", "SLObject.AbortAsyncOperation", "SLObject.Destroy",
            "SLObject.SetPriority", "SLObject.GetPriority", "SLObject.SetLossOfControlInterfaces"};
        rt_hostfn eng[15] = {NULL, NULL, eng_CreateAudioPlayer, NULL, NULL, NULL, NULL, eng_CreateOutputMix};
        static const char *const eng_n[15] = {"SLEngine.CreateLEDDevice", "SLEngine.CreateVibraDevice",
            "SLEngine.CreateAudioPlayer", "SLEngine.CreateAudioRecorder", "SLEngine.CreateMidiPlayer",
            "SLEngine.CreateListener", "SLEngine.Create3DGroup", "SLEngine.CreateOutputMix",
            "SLEngine.CreateMetadataExtractor", "SLEngine.CreateExtensionObject", "SLEngine.QueryNumSupportedInterfaces",
            "SLEngine.QuerySupportedInterfaces", "SLEngine.QueryNumSupportedExtensions", "SLEngine.QuerySupportedExtension",
            "SLEngine.IsExtensionSupported"};
        rt_hostfn play[12] = {play_SetPlayState, play_GetPlayState, play_zero, play_zero, play_ok, play_ok, play_zero,
                              play_ok, play_ok, play_zero, play_ok, play_zero};
        static const char *const play_n[12] = {"SLPlay.SetPlayState", "SLPlay.GetPlayState", "SLPlay.GetDuration",
            "SLPlay.GetPosition", "SLPlay.RegisterCallback", "SLPlay.SetCallbackEventsMask",
            "SLPlay.GetCallbackEventsMask", "SLPlay.SetMarkerPosition", "SLPlay.ClearMarkerPosition",
            "SLPlay.GetMarkerPosition", "SLPlay.SetPositionUpdatePeriod", "SLPlay.GetPositionUpdatePeriod"};
        rt_hostfn bq[4] = {bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback};
        static const char *const bq_n[4] = {"SLBufferQueue.Enqueue", "SLBufferQueue.Clear", "SLBufferQueue.GetState",
                                            "SLBufferQueue.RegisterCallback"};
        vt_object = table(obj, obj_n, 10);
        vt_engine = table(eng, eng_n, 15);
        vt_play = table(play, play_n, 12);
        vt_bq = table(bq, bq_n, 4);
    }
    if (A(0)) rt_st32(A(0), new_object(K_ENGINE));
    rt_log("OpenSL: engine created");
    rt_ret(c, SL_SUCCESS);
}

void rt_sles_register(void) { rt_import_register("slCreateEngine", imp_slCreateEngine); }
