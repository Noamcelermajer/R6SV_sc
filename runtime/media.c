/* android.media.AudioTrack emulation: ring buffer between the Vox thread (write) and the platform backend (pull).
 * See media.h and docs/notes/audio.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

#include "media.h"
#include "rt.h"

#define MAX_TRACKS 4
#define RING_FRAMES 16384u /* power of two: ~370 ms at 44.1 kHz; Vox keeps well below that (it writes 23 ms every 23 ms) */

typedef struct track {
    volatile int used, playing;
    int rate, channels, bytes_per_sample;
    int16_t *ring;                /* RING_FRAMES * channels samples */
    volatile uint32_t rpos, wpos; /* in frames, free-running */
    void *stream;                 /* backend stream or NULL (no device: record only) */
    FILE *wav;
    uint32_t wav_bytes;
} track;

static track tracks[MAX_TRACKS];
static volatile int lock_;
static volatile int muted;
static void lock(void) { while (__atomic_exchange_n(&lock_, 1, __ATOMIC_ACQUIRE)) sleep_ms(0); }
static void unlock(void) { __atomic_store_n(&lock_, 0, __ATOMIC_RELEASE); }

/* ---- WAV capture ---------------------------------------------------------------------------------- */
static void wav_header(track *t) {
    uint32_t rate = (uint32_t)t->rate, ch = (uint32_t)t->channels, size = t->wav_bytes;
    uint8_t h[44] = {'R', 'I', 'F', 'F'};
    uint32_t riff = 36 + size, fmt_len = 16, byte_rate = rate * ch * 2;
    uint16_t pcm = 1, ch16 = (uint16_t)ch, align = (uint16_t)(ch * 2), bits = 16;
    memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt_len, 4); memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch16, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &byte_rate, 4); memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &size, 4);
    fseek(t->wav, 0, SEEK_SET);
    fwrite(h, 1, 44, t->wav);
    fseek(t->wav, 0, SEEK_END);
    fflush(t->wav);
}
static void wav_put(track *t, const int16_t *s, int frames) {
    if (!t->wav) return;
    fwrite(s, 2, (size_t)frames * t->channels, t->wav);
    uint32_t before = t->wav_bytes;
    t->wav_bytes += (uint32_t)(frames * t->channels * 2);
    if ((before >> 16) != (t->wav_bytes >> 16)) wav_header(t); /* keep the header valid if the process is killed */
}

/* ---- ring: single writer (Vox), single reader (backend callback) ------------------------------------ */
static uint32_t ring_free(track *t) {
    return RING_FRAMES - (__atomic_load_n(&t->wpos, __ATOMIC_ACQUIRE) - __atomic_load_n(&t->rpos, __ATOMIC_ACQUIRE));
}

static void pull(void *user, int16_t *out, int frames) {
    track *t = user;
    int ch = t->channels;
    uint32_t r = __atomic_load_n(&t->rpos, __ATOMIC_RELAXED);
    uint32_t avail = __atomic_load_n(&t->wpos, __ATOMIC_ACQUIRE) - r;
    uint32_t n = avail < (uint32_t)frames ? avail : (uint32_t)frames;
    for (uint32_t k = 0; k < n; k++)
        for (int c = 0; c < ch; c++) out[k * ch + c] = t->ring[((r + k) & (RING_FRAMES - 1)) * ch + c];
    if (n < (uint32_t)frames) memset(out + (size_t)n * ch, 0, (size_t)(frames - (int)n) * ch * 2);
    __atomic_store_n(&t->rpos, r + n, __ATOMIC_RELEASE);
    if (muted) memset(out, 0, (size_t)frames * ch * 2);
    if (n) wav_put(t, out, (int)n);
}

/* ---- AudioTrack ------------------------------------------------------------------------------------- */
static int channels_of(int cfg) { return cfg == 4 || cfg == 2 /* MONO, DEFAULT */ ? 1 : 2; }

int rt_media_min_buffer(int rate, int channel_config, int encoding) {
    (void)rate;
    int frame = channels_of(channel_config) * (encoding == 3 ? 1 : 2);
    return 8192 / frame * frame;
}

int rt_media_track_create(int stream_type, int rate, int channel_config, int encoding, int buffer_bytes, int mode) {
    (void)stream_type; (void)buffer_bytes; (void)mode;
    if (rate < 4000 || rate > 192000) rate = 44100;
    lock();
    int id = 0;
    for (int k = 0; k < MAX_TRACKS && !id; k++)
        if (!tracks[k].used) { id = k + 1; tracks[k].used = 1; }
    unlock();
    if (!id) { rt_log("audio: no free AudioTrack"); return 0; }
    track *t = &tracks[id - 1];
    t->playing = 0;
    t->rate = rate;
    t->channels = channels_of(channel_config);
    t->bytes_per_sample = encoding == 3 ? 1 : 2;
    t->rpos = t->wpos = 0;
    t->ring = calloc((size_t)RING_FRAMES * t->channels, 2);
    t->wav = NULL;
    t->wav_bytes = 0;
    const char *wav = getenv("NOVA_AUDIO_WAV");
    if (wav && id == 1 && (t->wav = fopen(wav, "wb"))) wav_header(t);
    const char *none = getenv("R6_AUDIO_NULL");
    t->stream = (none && *none == '1') ? NULL : rt_audio_open(rate, t->channels, pull, t);
    rt_log("audio: AudioTrack %d: %d Hz, %d ch, %d-bit, %s", id, rate, t->channels, t->bytes_per_sample * 8,
           t->stream ? "device" : "no device (recording only)");
    return id;
}

static track *get(int id) { return id >= 1 && id <= MAX_TRACKS && tracks[id - 1].used ? &tracks[id - 1] : NULL; }

void rt_media_track_play(int id) {
    track *t = get(id);
    if (!t) return;
    t->playing = 1;
    if (t->stream) rt_audio_start(t->stream);
}
void rt_media_track_pause(int id) {
    track *t = get(id);
    if (!t) return;
    t->playing = 0;
    if (t->stream) rt_audio_stop(t->stream);
}
void rt_media_track_stop(int id) { rt_media_track_pause(id); }

void rt_media_track_release(int id) {
    track *t = get(id);
    if (!t) return;
    t->playing = 0;
    if (t->stream) { rt_audio_stop(t->stream); rt_audio_close(t->stream); t->stream = NULL; }
    if (t->wav) { wav_header(t); fclose(t->wav); t->wav = NULL; }
    free(t->ring);
    t->ring = NULL;
    t->used = 0;
}

int rt_media_track_write(int id, const void *pcm, int bytes) {
    track *t = get(id);
    if (!t || !pcm || bytes <= 0) return 0;
    int frame_bytes = t->channels * t->bytes_per_sample, frames = bytes / frame_bytes;
    if (!frames) return 0;
    const uint8_t *src = pcm;
    int done = 0;
    while (done < frames) {
        if (!t->stream) { /* no device: record only (8-bit input is not recorded) */
            if (t->bytes_per_sample == 2 && t->playing && !muted)
                wav_put(t, (const int16_t *)src + (size_t)done * t->channels, frames - done);
            done = frames;
            break;
        }
        uint32_t room = ring_free(t);
        if (!room) {
            if (!t->playing) { done = frames; break; } /* paused: drop */
            int waited = 0;
            while (!(room = ring_free(t)) && waited < 100 && t->used) { sleep_ms(2); waited += 2; }
            if (!room) break; /* the device is not draining: drop the rest */
        }
        int n = frames - done < (int)room ? frames - done : (int)room;
        uint32_t w = __atomic_load_n(&t->wpos, __ATOMIC_RELAXED);
        for (int k = 0; k < n; k++)
            for (int c = 0; c < t->channels; c++) {
                int16_t v;
                size_t i = (size_t)(done + k) * t->channels + c;
                if (t->bytes_per_sample == 2) memcpy(&v, src + i * 2, 2);
                else v = (int16_t)(((int)src[i] - 128) << 8);
                t->ring[((w + k) & (RING_FRAMES - 1)) * t->channels + c] = v;
            }
        __atomic_store_n(&t->wpos, w + (uint32_t)n, __ATOMIC_RELEASE);
        done += n;
    }
    return frames * frame_bytes;
}

void rt_media_set_mute(int mute) { muted = mute != 0; }
int rt_media_get_mute(void) { return muted; }
