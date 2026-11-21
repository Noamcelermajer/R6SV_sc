/* Windows audio backend: waveOut with four blocks in flight, refilled by a thread that pulls from the OpenSL player.
 * NOVA_AUDIO_WAV=<file> also records the stream into a WAV file (for checking audio without listening). */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <mmsystem.h>

#include "rt.h"

#define BLOCKS 4
#define FRAMES 1024

typedef struct stream {
    HWAVEOUT wo;
    HANDLE event, thread;
    WAVEHDR hdr[BLOCKS];
    int16_t *buf[BLOCKS];
    int channels, rate;
    rt_audio_pull pull;
    void *user;
    volatile int running, quit;
    FILE *wav;
    uint32_t wav_bytes;
} stream;

static void wav_header(stream *s) {
    uint32_t rate = (uint32_t)s->rate, ch = (uint32_t)s->channels, byte_rate = rate * ch * 2, size = s->wav_bytes;
    uint8_t h[44] = {'R', 'I', 'F', 'F'};
    uint32_t riff = 36 + size, fmt_len = 16, data_len = size;
    uint16_t pcm = 1, ch16 = (uint16_t)ch, align = (uint16_t)(ch * 2), bits = 16;
    memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt_len, 4); memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch16, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &byte_rate, 4); memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data_len, 4);
    fseek(s->wav, 0, SEEK_SET);
    fwrite(h, 1, 44, s->wav);
    fseek(s->wav, 0, SEEK_END);
    fflush(s->wav);
}

static void fill(stream *s, int k) {
    if (s->running) s->pull(s->user, s->buf[k], FRAMES);
    else memset(s->buf[k], 0, (size_t)FRAMES * s->channels * 2);
    if (s->wav && s->running) {
        fwrite(s->buf[k], 2, (size_t)FRAMES * s->channels, s->wav);
        s->wav_bytes += (uint32_t)(FRAMES * s->channels * 2);
        if ((s->wav_bytes / (FRAMES * s->channels * 2)) % 64 == 0) wav_header(s);
    }
    waveOutWrite(s->wo, &s->hdr[k], sizeof(WAVEHDR));
}

static DWORD WINAPI feeder(void *p) {
    stream *s = p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (int k = 0; k < BLOCKS; k++) fill(s, k);
    while (!s->quit) {
        WaitForSingleObject(s->event, 50);
        for (int k = 0; k < BLOCKS; k++)
            if (s->hdr[k].dwFlags & WHDR_DONE) fill(s, k);
    }
    return 0;
}

void *rt_audio_open(int rate, int channels, rt_audio_pull pull, void *user) {
    stream *s = calloc(1, sizeof *s);
    s->rate = rate;
    s->channels = channels;
    s->pull = pull;
    s->user = user;
    s->event = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, (WORD)channels, (DWORD)rate, (DWORD)(rate * channels * 2), (WORD)(channels * 2), 16, 0};
    if (waveOutOpen(&s->wo, WAVE_MAPPER, &f, (DWORD_PTR)s->event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        rt_log("audio: waveOutOpen failed (%d Hz, %d channels)", rate, channels);
        free(s);
        return NULL;
    }
    for (int k = 0; k < BLOCKS; k++) {
        s->buf[k] = calloc((size_t)FRAMES * channels, 2);
        s->hdr[k].lpData = (LPSTR)s->buf[k];
        s->hdr[k].dwBufferLength = (DWORD)(FRAMES * channels * 2);
        waveOutPrepareHeader(s->wo, &s->hdr[k], sizeof(WAVEHDR));
    }
    const char *wav = getenv("NOVA_AUDIO_WAV");
    if (wav && (s->wav = fopen(wav, "wb"))) wav_header(s);
    s->thread = CreateThread(NULL, 0, feeder, s, 0, NULL);
    return s;
}

void rt_audio_start(void *p) { ((stream *)p)->running = 1; }
void rt_audio_stop(void *p) { ((stream *)p)->running = 0; }

void rt_audio_close(void *p) {
    stream *s = p;
    s->quit = 1;
    SetEvent(s->event);
    WaitForSingleObject(s->thread, 1000);
    waveOutReset(s->wo);
    for (int k = 0; k < BLOCKS; k++) waveOutUnprepareHeader(s->wo, &s->hdr[k], sizeof(WAVEHDR));
    waveOutClose(s->wo);
    if (s->wav) { wav_header(s); fclose(s->wav); }
}
