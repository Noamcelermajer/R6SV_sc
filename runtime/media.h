/* Sound: the emulation of android.media.AudioTrack that the native Vox engine drives through JNI.
 * Vox mixes 44.1 kHz stereo int16 itself and pushes it with AudioTrack.write from its own thread; this module
 * turns those writes into a ring buffer drained by the platform backend (audio_win.c / audio_android.c).
 * No game memory access here: the JNI layer passes host pointers (G(addr)). Thread-safe for one writer thread
 * (vox) plus control calls from another (game thread). See docs/notes/audio.md. */
#ifndef R6_MEDIA_H
#define R6_MEDIA_H

#include <stdint.h>

/* AudioTrack.getMinBufferSize(rate, channelConfig, encoding): bytes, a multiple of the frame size */
int rt_media_min_buffer(int rate, int channel_config, int encoding);

/* new AudioTrack(streamType, rate, channelConfig, encoding, bufferBytes, mode): returns a track id (> 0), 0 on failure.
 * channelConfig 12 = stereo, 4 = mono; encoding 2 = 16-bit, 3 = 8-bit. */
int rt_media_track_create(int stream_type, int rate, int channel_config, int encoding, int buffer_bytes, int mode);
void rt_media_track_play(int id);
void rt_media_track_pause(int id);
void rt_media_track_stop(int id);
void rt_media_track_release(int id);
/* AudioTrack.write(byte[], offset, size): copies `bytes` of PCM, returns the bytes accepted (blocks up to ~100 ms if full) */
int rt_media_track_write(int id, const void *pcm, int bytes);

/* Game.SetDeviceMute(1/0) */
void rt_media_set_mute(int mute);
int rt_media_get_mute(void);

/* Environment: NOVA_AUDIO_WAV=<file> records everything played into a WAV file (also when no device is available);
 * R6_AUDIO_NULL=1 never opens an output device (writes are paced to real time and only recorded). */

#endif
