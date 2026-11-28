# Recording: `--record out.mp4`

`--record out.mp4` writes what the game shows **and what it plays** to an
mp4: H.264 video at 30 fps and AAC stereo sound at 44.1 kHz. It needs
**ffmpeg on `PATH`**. It works headless and muted: a test run's speakers stay
silent, and the recording still has the sound.

```
swgbrecomp.exe --run --multi --headless --watchdog 160 --films "<Steam>\Game" --record run.mp4
```

The log says what went in:

```
[softdd] recording 800x600 at 30 fps -> ...\run.mp4
[recaudio] DirectSound: the game's sound is recorded (a proxy over the real one)
[recaudio] 159.70 s of sound, 26 buffers (13 plays), 2 streams, 0 cursor jumps; peak 0.0 dBFS, rms -21.0 dBFS
[recaudio] muxed into ...\run.mp4
```

## The picture (`softdd.c`)

The first frame the game presents opens a `ffmpeg` on a pipe (raw RGB565,
`libx264`). Frames go out at 30 a second by the recording's clock: a slow
frame is held, a fast one dropped, so the mp4 plays at the game's real speed.
A mode other than the first (the films' 640x480) is scaled to the first's
size. When the recording is closed (the game's exit, `--watchdog`, a fault,
`--frames`), it stays closed: a frame presented after that opens no new
recording over the file. If `ffmpeg` exits early and stops reading the
pipe, `softdd.c` logs once, stops recording, and lets the game continue.

## The sound (`recaudio.c`)

Nothing here listens to what reaches the speakers. Test runs mute the
process's audio session (`mute_process`), and Windows' process loopback
capture (WASAPI, `PROCESS_LOOPBACK`) hears a muted session as silence: tried,
RMS 0.5 of 32768. Instead, the mix is made again from what the game asks
for, on the recording's clock.

- **The game's sound is DirectSound** (`DSOUND.dll#1`, `DirectSoundCreate`).
  The host's shim always hands the game a proxy over the real
  `IDirectSound`, and every secondary buffer it creates (or duplicates) is a
  proxy over the real buffer so `recaudio_set_effects_gain` can apply the
  sound slider to sound effects alone (adding `DSBCAPS_CTRLVOLUME` and hiding
  the slider's offset from `GetVolume`). Without `--record`, no shadows or
  mixer thread are created. With `--record`, beside each buffer each proxy
  keeps a shadow: a copy of its samples (taken at each `Unlock`, before the
  real buffer has them back), its format, volume, pan and frequency, the
  primary buffer's volume as the master, and when it was played and stopped
  (the clock's time of the call, not the mixer's next tick).
- **The game streams its music** (`sound\stream\*.mp3`: the menus, the
  game, `won1.mp3` and `lost.mp3`) into a looping buffer it rewrites behind
  the play cursor. A shadow must read it where the real one reads, so every
  10 ms the mixer asks each playing buffer for its real play cursor and
  compares it with where its shadow will be by then: a small rate correction
  (at most 0.5%) keeps the shadow on it, and a shadow more than 100 ms off
  jumps to it (`cursor jumps` in the summary; 0 in every run so far). A
  buffer the real one says has stopped stops in the recording too, unless
  its shadow is within 100 ms of its end.
- **The films** are the host's own waveOut (`movie.c`). Every chunk it
  queues also goes to the recording through the **stream tap**,
  `recaudio_stream_write(id, pcm, frames, plays_in_ms)`: 16-bit PCM at any
  rate, mono or stereo, and how long until it is heard (what waveOut has
  queued before it). A stream is placed on the timeline by that and kept
  there: appended chunk after chunk, but moved (silence added, or frames
  dropped) when the player's own position says it is more than 60 ms off.
  That keeps a long stream from drifting with its sound card's clock. Pausing
  holds a stream; closing it drops what was not heard yet, as `waveOutReset`
  does.
- **The CD music** (`cdaudio.c`) plays on `waveOut` as the films do and calls
  the same stream tap functions (`recaudio_stream_open`, `_write`, `_close`
  in `recaudio.h`), so in-game CD music is recorded into `--record`'s mp4 as
  well. Without `--record` they do nothing (`open` gives -1).

The mixer is a thread of ours: every 10 ms it mixes 44.1 kHz stereo (linear
interpolation, DirectSound's volume and pan in hundredths of a dB) up to the
clock's now, into `out.mp4.wav`.

## In sync, however long it runs

There is one clock, `recaudio_clock_ms` (`QueryPerformanceCounter`): softdd
counts the video's frames by it and the mixer its samples. Frame *k* and
sample *k* x 1470 are the same moment, at the start of a recording and an
hour into it; nothing is resampled to fit. When the recording closes, the
sound is cut or padded to exactly the video's length (its frames x 1470
samples), and a second ffmpeg muxes the two: the video copied, the sound as
AAC 192 kb/s, into the same mp4. If the mux fails, the video stays in
`out.mp4` and the sound in `out.mp4.wav`, and the log says so.

Measured (`record/base`, `record/cc` in `tools/test.py`): the sound's
duration equals the video's to the millisecond (159.700 s and 159.767 s).
The logo film's first sound is 40-50 ms after its first picture, against
10 ms in the AVI itself; softdd puts a new picture into its frame up to one
frame (33 ms) early. `won1.mp3` in the recording matches the file, decoded by
ffmpeg, half a second at a time at a correlation of 0.88-0.997, a constant
33 ms from where the log puts it, for the 12 s measured (the first seconds
are under the victory fanfare).

## Checking it

- `py -3 tools\test.py --level unit --only unit/recaudio`: the proxy over a
  fake DirectSound driven on a simulated clock (`recaudio_test_clock_ms` and
  `recaudio_tick`, no real-time sleeps), split into one case per check: a
  one-shot's time and pitch, volume and pan, a looping buffer rewritten while
  it plays, the stream tap (its time, 22050 Hz mono resampled, a pause), a
  player whose sound card runs 3% fast for 4 s kept within 60 ms (120 ms
  uncorrected), the sound slider's effects gain (with and without `--record`),
  and the WAV cut to the video's exact length. No sound card needed.
- `record/base` and `record/cc` (L2, about 3 minutes each): the logo film
  with its sound, the intro skipped, a skirmish (the command center clicked,
  its hotkey), then `skywalker` and the victory music over the score screen,
  recorded. They check that the mp4 has a sound stream as long as the video
  (within two frames), at least 5 DirectSound plays, the film's first 12 s
  and the last 15 s (the victory) loud, and the sound above -60 dBFS for at
  least a quarter of the run.

## Not done

- **No audio device:** `DirectSoundCreate` fails, so the game plays nothing,
  and the recording's sound is silence (the films too: without a device they
  are timed by the wall clock, and their sound is not decoded).
- **DirectSound 3D, `IDirectSound8`, notifications:** the game uses none of
  them. A `QueryInterface` the proxy does not answer itself goes to the real
  object, and the log says so the first time.
- **Linux and macOS** (`src/linux`): `--record` there has no sound yet. The
  mix is win32hle's (`pcrecomp/runtime/win32hle/dsound.c`, `mix()` on SDL's
  audio thread, films and music through `hle_audio_stream_*`). Two ways:
  a tap in `mix()` that hands each mixed block to the host (a pcrecomp
  change), timed by SDL's audio clock and cut to the video at the end, as
  here; or SDL's `disk` audio driver (`SDL_AUDIODRIVER=disk`,
  `SDL_DISKAUDIOFILE`) for a muted run, which writes the mix to a file in
  real time.
