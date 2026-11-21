/* Android audio backend: an AAudio output stream whose data callback pulls from the OpenSL player.
 * The pull only copies from the player's queue, so it is safe in AAudio's real-time callback. */
#include <aaudio/AAudio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

typedef struct stream {
    AAudioStream *s;
    rt_audio_pull pull;
    void *user;
    int channels;
    volatile int running;
} stream;

static aaudio_data_callback_result_t on_data(AAudioStream *as, void *user, void *data, int32_t frames) {
    (void)as;
    stream *s = user;
    if (s->running) s->pull(s->user, data, frames);
    else memset(data, 0, (size_t)frames * s->channels * 2);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void *rt_audio_open(int rate, int channels, rt_audio_pull pull, void *user) {
    stream *s = calloc(1, sizeof *s);
    s->pull = pull;
    s->user = user;
    s->channels = channels;
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { free(s); return NULL; }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, channels);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, on_data, s);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s->s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        rt_log("audio: AAudio openStream failed: %s", AAudio_convertResultToText(r));
        free(s);
        return NULL;
    }
    rt_log("audio: AAudio stream %d Hz, %d channels", AAudioStream_getSampleRate(s->s), channels);
    return s;
}

void rt_audio_start(void *p) {
    stream *s = p;
    s->running = 1;
    AAudioStream_requestStart(s->s);
}

void rt_audio_stop(void *p) {
    stream *s = p;
    s->running = 0;
    AAudioStream_requestPause(s->s);
}

void rt_audio_close(void *p) {
    stream *s = p;
    AAudioStream_close(s->s);
    free(s);
}
