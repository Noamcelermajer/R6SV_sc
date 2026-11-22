# Audio

## Finding

The game has no Java audio on its live path and no OpenSL ES. The statically linked **Vox** engine decodes and mixes
everything itself (44.1 kHz, stereo, int16) and hands PCM to Java only through `android/media/AudioTrack.write`
(docs/JAVA-SIDE.md section 5). So the port needs no decoder: it needs an `AudioTrack` emulation. Nothing of the
Java `GLMediaPlayer`/`SoundPool` engine is called.

## Files

- `runtime/media.h`, `runtime/media.c`: AudioTrack emulation (`rt_media_*`). Track table (4), one ring buffer per track
  (16384 frames, single writer = Vox thread, single reader = backend callback, lock-free with atomics), mute, WAV capture.
- `runtime/audio_win.c` (waveOut, 4 x 1024-frame blocks), `runtime/audio_android.c` (AAudio, data callback, needs API 26):
  both unchanged in interface (`rt_audio_open/start/stop/close`); media.c passes its `pull`.
- `runtime/sles.c`: now an empty `rt_sles_register()` (OpenSL is unused). `core.c` still calls it; delete the call in core.c
  and then this file. `rt_sl_iid` in libc.c is also dead.
- `runtime/test/media_test.c`: host test (command in its header).

## Interface for runtime/jni.c (host pointers, no game memory access in media.c)

| Java call | call |
|---|---|
| `AudioTrack.getMinBufferSize(III)I` | `rt_media_min_buffer(rate, cfg, enc)` (returns 8192) |
| `new AudioTrack(IIIIII)V` | `id = rt_media_track_create(stream, rate, cfg, enc, bytes, mode)`; keep `id` in the jobject |
| `play/pause/stop/release()V` | `rt_media_track_play/pause/stop/release(id)` |
| `write([BII)I` | `rt_media_track_write(id, (uint8_t*)G(array_data) + offset, size)`; return value is the byte count |
| `Game.SetDeviceMute(I)V` | `rt_media_set_mute(arg == 1)` |

`write` is called from the Vox thread; `play/pause` also from the game thread. Create/release are not concurrent with
write. `write` blocks up to 100 ms when the ring is full and playing, drops when paused (Vox paces itself with usleep, so
in practice it never blocks).

Environment: `NOVA_AUDIO_WAV=<file>` (host_main `--wav`) records what the device would play (post mute) into a 16-bit WAV
(first track only; header kept valid while recording). `R6_AUDIO_NULL=1` never opens a device (record only), for unattended runs.

## Formats found in data/audio.zip (1,668 entries, all stored, 290 MB)

| format | files |
|---|---|
| WAV, IMA ADPCM (tag 17), mono 32000 Hz, 2041 samples/block of 1024 bytes | 1443 |
| WAV, IMA ADPCM, stereo 32000 Hz | 173 |
| WAV, IMA ADPCM, 44100 Hz mono / stereo | 10 / 8 |
| WAV, IMA ADPCM, mono 22050 Hz | 6 |
| WAV, PCM16, 22050 / 32000 / 44100 Hz mono | 11 / 2 / 1 |
| `.vxn` (VoxN container, header `VoxN`, version string `0.0.1`, then a `fmt ` chunk; 5-7 MB each, music `m_*.vxn`) | 14 |

No ogg/mp3/mpc files in the pack. Vox decodes all of these natively, so the port does not.

## Verified

- `runtime/test/media_test.c` reads `CSS_M05_CS2_003.wav` (ADPCM mono 32 kHz, 7.4 s) and `lvl_sniper_shoot.wav` (PCM16 mono 44.1 kHz) from
  the real audio.zip, decodes (test-only decoder), resamples to 44.1 kHz stereo, mixes, and writes 1024-frame chunks every 23 ms
  through `rt_media_track_write`. Results: no-device mode and waveOut mode both PASS (7.71 s recorded, rms 3751, peak at full scale,
  71% of samples above 100). Playback through waveOut ran in real time (8.6 s for 7.7 s of audio). Output WAVs in `build/audio/`.
- `media.c`, `audio_android.c`, `sles.c` compile for aarch64-linux-android26 (clang from the NDK); not run on a device.

## Not verified / open

- The real Vox thread has not run: needs jni.c to call the table above and the translated game to boot. The first sign of
  trouble would be `audio: AudioTrack 1: ...` missing from the log, or `rt_jni` logging an unemulated `AudioTrack` member.
- `.vxn` music decoding is Vox's; not tested here.
- Latency: ring up to ~370 ms capacity but normally holds one or two chunks; AAudio low-latency mode is requested.
- Pause/resume: `pause` stops pulling (ring keeps its contents), `play` resumes.
- Windows pacing: `Sleep(23)` has 15.6 ms granularity unless the process calls `timeBeginPeriod(1)`; the runner's `usleep` shim
  should (otherwise Vox, which paces itself with usleep, runs at 2/3 speed and the output underruns).
- Audio is 16-bit only for recording; 8-bit input is converted for playback but not recorded in no-device mode.
