/* Host test for runtime/media.c + audio_win.c: reads two real sounds from data/audio.zip (stored zip), decodes them
 * (MS IMA ADPCM or PCM16), resamples to 44.1 kHz stereo, mixes them and pushes them through the AudioTrack emulation
 * exactly like Vox does (1024-frame writes every ~23 ms), recording into a WAV file, then checks the WAV is not silence.
 *
 *   py -m ziglang cc -O1 -target x86_64-windows-gnu -Iruntime -Itools/recomp/runtime runtime/test/media_test.c \
 *       runtime/media.c runtime/audio_win.c -lwinmm -o build/audio/media_test.exe
 *   build/audio/media_test.exe <audio.zip> <out.wav> [name1.wav name2.wav]   (R6_AUDIO_NULL=1: no sound device)
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../media.h"

void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(1);
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* find a stored entry in the zip's central directory; returns malloc'ed data */
static uint8_t *zip_read(FILE *f, const char *name, uint32_t *size) {
    fseek(f, 0, SEEK_END);
    long end = ftell(f), n = end < 70000 ? end : 70000;
    uint8_t *tail = malloc((size_t)n);
    fseek(f, end - n, SEEK_SET);
    fread(tail, 1, (size_t)n, f);
    long e = n - 22;
    while (e >= 0 && le32(tail + e) != 0x06054b50) e--;
    if (e < 0) return NULL;
    uint32_t cd_size = le32(tail + e + 12), cd_off = le32(tail + e + 16);
    uint8_t *cd = malloc(cd_size);
    fseek(f, (long)cd_off, SEEK_SET);
    fread(cd, 1, cd_size, f);
    for (uint32_t p = 0; p + 46 <= cd_size;) {
        uint32_t nl = le16(cd + p + 28), xl = le16(cd + p + 30), cl = le16(cd + p + 32);
        if (nl == strlen(name) && !strncmp((char *)cd + p + 46, name, nl)) {
            uint32_t sz = le32(cd + p + 24), lo = le32(cd + p + 42);
            uint8_t lh[30];
            fseek(f, (long)lo, SEEK_SET);
            fread(lh, 1, 30, f);
            fseek(f, (long)(lo + 30 + le16(lh + 26) + le16(lh + 28)), SEEK_SET);
            uint8_t *d = malloc(sz);
            fread(d, 1, sz, f);
            *size = sz;
            return d;
        }
        p += 46 + nl + xl + cl;
    }
    return NULL;
}

static const int idx_tab[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int step_tab[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

typedef struct { int16_t pred; int idx; } adpcm_st;
static int16_t adpcm_nibble(adpcm_st *s, int nib) {
    int step = step_tab[s->idx], d = step >> 3;
    if (nib & 1) d += step >> 2;
    if (nib & 2) d += step >> 1;
    if (nib & 4) d += step;
    int p = s->pred + (nib & 8 ? -d : d);
    s->pred = (int16_t)(p > 32767 ? 32767 : p < -32768 ? -32768 : p);
    s->idx += idx_tab[nib];
    s->idx = s->idx < 0 ? 0 : s->idx > 88 ? 88 : s->idx;
    return s->pred;
}

typedef struct { int16_t *pcm; int frames, rate, channels; } sound;

static int load_wav(FILE *z, const char *name, sound *out) {
    uint32_t size;
    uint8_t *w = zip_read(z, name, &size);
    if (!w || memcmp(w, "RIFF", 4)) return 0;
    int tag = 0, ch = 0, rate = 0, align = 0, spb = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    for (uint32_t p = 12; p + 8 <= size;) {
        uint32_t cl = le32(w + p + 4);
        if (!memcmp(w + p, "fmt ", 4)) { tag = (int)le16(w + p + 8); ch = (int)le16(w + p + 10); rate = (int)le32(w + p + 12); align = (int)le16(w + p + 20); if (cl >= 20) spb = (int)le16(w + p + 26); }
        if (!memcmp(w + p, "data", 4)) { data = w + p + 8; dlen = cl > size - p - 8 ? size - p - 8 : cl; break; }
        p += 8 + cl + (cl & 1);
    }
    if (!data || ch < 1 || ch > 2) return 0;
    int frames = 0;
    int16_t *pcm = NULL;
    if (tag == 1) {
        frames = (int)(dlen / (2 * ch));
        pcm = malloc((size_t)frames * ch * 2);
        memcpy(pcm, data, (size_t)frames * ch * 2);
    } else if (tag == 17) {
        int blocks = (int)(dlen / align);
        pcm = malloc((size_t)blocks * spb * ch * 2);
        for (int b = 0; b < blocks; b++) {
            const uint8_t *bp = data + (size_t)b * align;
            adpcm_st st[2];
            int16_t *o = pcm + (size_t)b * spb * ch;
            for (int c = 0; c < ch; c++) { st[c].pred = (int16_t)le16(bp + 4 * c); st[c].idx = bp[4 * c + 2]; o[c] = st[c].pred; }
            const uint8_t *q = bp + 4 * ch;
            int n = 1;
            while (n < spb) { /* groups of 8 samples per channel, 4 bytes each, channels one after another */
                for (int c = 0; c < ch; c++)
                    for (int k = 0; k < 8; k++) {
                        int nib = (q[4 * c + (k >> 1)] >> ((k & 1) * 4)) & 15;
                        if (n + k < spb) o[(size_t)(n + k) * ch + c] = adpcm_nibble(&st[c], nib);
                    }
                q += 4 * ch; n += 8;
            }
        }
        frames = blocks * spb;
    } else return 0;
    free(w);
    out->pcm = pcm; out->frames = frames; out->rate = rate; out->channels = ch;
    return 1;
}

/* sample at fractional frame position, channel mapped to stereo */
static void sample_at(const sound *s, double pos, int16_t *l, int16_t *r) {
    int i = (int)pos;
    double f = pos - i;
    int j = i + 1 < s->frames ? i + 1 : i;
    int ch = s->channels;
    double a0 = s->pcm[(size_t)i * ch], a1 = s->pcm[(size_t)j * ch];
    double b0 = s->pcm[(size_t)i * ch + ch - 1], b1 = s->pcm[(size_t)j * ch + ch - 1];
    *l = (int16_t)(a0 + (a1 - a0) * f);
    *r = (int16_t)(b0 + (b1 - b0) * f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: media_test audio.zip out.wav [a.wav b.wav]\n"); return 2; }
    const char *n1 = argc > 3 ? argv[3] : "CSS_M05_CS2_003.wav", *n2 = argc > 4 ? argv[4] : "lvl_sniper_shoot.wav";
    _putenv_s("NOVA_AUDIO_WAV", argv[2]);
    FILE *z = fopen(argv[1], "rb");
    if (!z) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    sound s[2];
    const char *names[2] = {n1, n2};
    for (int k = 0; k < 2; k++) {
        if (!load_wav(z, names[k], &s[k])) { fprintf(stderr, "cannot load %s\n", names[k]); return 2; }
        fprintf(stderr, "%s: %d frames, %d Hz, %d ch (%.2f s)\n", names[k], s[k].frames, s[k].rate, s[k].channels, (double)s[k].frames / s[k].rate);
    }
    /* second sound starts 0.5 s after the first, like an effect over speech */
    double total_s = fmax((double)s[0].frames / s[0].rate, 0.5 + (double)s[1].frames / s[1].rate) + 0.3;
    int total = (int)(total_s * 44100);
    int track = rt_media_track_create(3, 44100, 12, 2, rt_media_min_buffer(44100, 12, 2), 1);
    if (!track) return 1;
    rt_media_track_play(track);
    int16_t chunk[1024 * 2];
    timeBeginPeriod(1);
    DWORD t0 = GetTickCount();
    for (int pos = 0; pos < total; pos += 1024) {
        for (int k = 0; k < 1024; k++) {
            int fr = pos + k, L = 0, R = 0;
            for (int q = 0; q < 2; q++) {
                double t = (double)fr / 44100 - (q ? 0.5 : 0.0);
                if (t < 0 || t * s[q].rate >= s[q].frames - 1) continue;
                int16_t l, r;
                sample_at(&s[q], t * s[q].rate, &l, &r);
                L += l; R += r;
            }
            chunk[k * 2] = (int16_t)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
            chunk[k * 2 + 1] = (int16_t)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
        }
        int w = rt_media_track_write(track, chunk, sizeof chunk);
        if (w != (int)sizeof chunk) { fprintf(stderr, "short write %d\n", w); return 1; }
        while ((double)(GetTickCount() - t0) < (pos / 1024 + 1) * 1024 * 1000.0 / 44100) Sleep(1);
    }
    Sleep(700); /* let the ring drain */
    rt_media_track_stop(track);
    rt_media_track_release(track);
    fprintf(stderr, "played %.2f s of audio in %.2f s\n", total_s, (GetTickCount() - t0) / 1000.0);

    FILE *w = fopen(argv[2], "rb");
    if (!w) { fprintf(stderr, "FAIL: no wav written\n"); return 1; }
    fseek(w, 0, SEEK_END);
    long len = ftell(w) - 44;
    fseek(w, 44, SEEK_SET);
    int16_t *d = malloc((size_t)len);
    fread(d, 1, (size_t)len, w);
    fclose(w);
    double sum = 0, peak = 0;
    long n = len / 2, active = 0;
    for (long k = 0; k < n; k++) { double v = d[k]; sum += v * v; if (fabs(v) > peak) peak = fabs(v); if (fabs(v) > 100) active++; }
    double rms = sqrt(sum / (double)(n ? n : 1));
    fprintf(stderr, "recorded %ld frames (%.2f s), rms %.1f, peak %.0f, %.1f%% of samples above 100\n", n / 2, n / 2 / 44100.0, rms, peak, 100.0 * active / (double)(n ? n : 1));
    if (n < 44100 || rms < 50 || peak < 1000) { fprintf(stderr, "FAIL: recording is silent or too short\n"); return 1; }
    fprintf(stderr, "PASS\n");
    return 0;
}
