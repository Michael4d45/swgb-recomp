# Music

Galactic Battlegrounds plays its in-game music from disc 2's audio tracks,
through WINMM's MCI `cdaudio` device. A Steam install has no audio CD: it has
`Game\Music\Track02.ogg` ... `Track50.ogg` (CD track 1 is the data track).
`src/runtime/cdaudio.c` is that CD, on both hosts: the MCI commands the
game sends, answered over the Ogg files, and both lines of the mixer — the
CD line for the music's volume and the wave-out line for the sound effects'
(`cdaudio_effects_volume`). Neither line ever touches the system's mixer or
the process's audio session volume.

```
swgbrecomp --music off      no CD: the game goes on without in-game music
swgbrecomp --music DIR      the tracks from DIR (default: the game's Music folder)
swgbrecomp --music native   Windows: the real WINMM (Clone Campaigns: Steam's WIN32.dll), every call logged
```

Test runs stay silent: on Windows the music plays on `waveOut`, in the
process's own audio session, which the host mutes unless `--sound` (or a
plain run with no `--keys` and no `--watchdog`); on Linux and macOS it plays
on win32hle's audio stream, under `hle_dsound_set_master`, with SDL's dummy
driver when headless.

## What the game does (measured)

Traced in `Battlegrounds.exe` (the music player is `0x004ACC00`-`0x004AE2F1`;
Clone Campaigns' is the same code at `0x004AD1F0`-) and in the host's log
(`[cdaudio]` lines):

| When | What |
|---|---|
| Start-up | `MCI_OPEN` (`MCI_OPEN_TYPE_ID`, `MCI_DEVTYPE_CD_AUDIO`, shareable, wait), `MCI_SET` `MCI_FORMAT_TMSF`, `MCI_STATUS` `MCI_STATUS_NUMBER_OF_TRACKS` (50). Then the mixer: the first device with a `MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC` line, that line's volume control (`MIXERCONTROL_CONTROLTYPE_VOLUME`), and its value, which it restores when it closes the CD. Then the music slider's value: `music volume 50% (-18.1 dB)` from the registry's `Music Volume` 5000. |
| Main menu, setup screens | No CD. The menus' music is the game's own MP3 streams (`sound\stream\open.mp3`, `town.mp3`, `random.mp3`), played by its DirectSound code. |
| A game starts (skirmish, campaign mission, a loaded save) | The CD is closed and opened again (the same start-up sequence), then one `MCI_PLAY` with `MCI_NOTIFY`, `MCI_FROM` track 2 and `MCI_TO` track 50: tracks 2-49 in a row. |
| That play ends | `MM_MCINOTIFY` (`MCI_NOTIFY_SUCCESSFUL`) to the game's window; the game plays tracks 2-49 again. |
| Victory, defeat | `MCI_STOP` (the play's notification comes back `MCI_NOTIFY_ABORTED`), the CD closed and opened, and `sound\stream\won1.mp3` / `lost.mp3` from the game's MP3 player. |
| Losing activation | `MCI_PAUSE`, then `MCI_PLAY` with no flags to go on (to the disc's end, with no notification). The hosts keep the game active, so this does not happen in practice. |
| Saving | `MCI_STATUS` `MCI_STATUS_POSITION`; a loaded game can `MCI_SEEK` there. |

The game counts its plays with `MCI_NOTIFY` against the `MM_MCINOTIFY`s it
gets back, aborted and superseded ones included, and only moves on when
none is outstanding (`0x004ADFA0`). So every play with notify gets exactly
one notification: successful at its end, aborted by a stop, pause, seek or
close, superseded by another play with notify.

If the open fails, or reports fewer than 2 tracks, the game tries MIDI music
(`midi\music%d.mid` through the MCI sequencer) and then streamed files
(`stream\music%d.wav`/`.mp3`). A Steam install has neither, so no `Music`
folder (or `--music off`) means no in-game music and nothing else changes.
On Windows those other devices still go to WINMM.

## The sliders

Both sliders (`0`-`100`, with "Off" at the bottom) map linearly in the game
(`-100*v` + `10000`, `0x00542DD0` for sound effects and `0x004ACC00`-`0x004AE2F1`
for CD music) onto each mixer line's `0..0xFFFF` range, so the bottom step
arrives as `1%` (`655`), never `0%`.

- **Why a cubic taper.** A linear scale leaves `1%` at `-40 dB` (faint, not
  silent) and puts `50%` at `-6 dB`. `cdaudio.c` applies `(v / 65535)^3`,
  approximating a Win9x/AC'97 audio-taper mixer: `100%` is `0.0 dB`, `50%`
  is `-18.1 dB` (`1/8` amplitude), `10%` is `-60.0 dB`, and the `1%` bottom
  step is `-120.0 dB` — below 16-bit PCM's quantization floor and below
  DirectSound's `-10000` (`-100 dB`) silence floor, so both sliders produce
  exact digital silence at the bottom.
- **Sound-slider isolation on Windows.** `cdaudio.c` intercepts
  `MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT` as well as `SRC_COMPACTDISC` so
  neither reaches WINMM (where Vista and later map a program's wave-out line
  to the process's audio session volume, which turned down `waveOut` CD
  music too). Instead, `cdaudio_effects_volume` drives
  `recaudio_set_effects_gain` on `recaudio.c`'s always-on DirectSound proxy,
  scaling every secondary buffer's volume (adding `DSBCAPS_CTRLVOLUME` at
  creation and hiding the slider's dB offset from `GetVolume`), while
  leaving `waveOut` (CD music, films) and the Windows volume untouched.
- **Sound-slider isolation on Linux and macOS (`win32hle`).**
  `cdaudio_effects_volume` calls `hle_dsound_set_buffer_gain` (pcrecomp
  `e70a0ab`), which scales only DirectSound buffers in `dsound.c` and leaves
  `hle_audio_stream_*` (CD music, films) untouched; `hle_dsound_set_master(0)`
  still mutes everything in test runs.

Only the CD and wave-out lines are ours, so the system's own mixer is never
touched; with the real WINMM (`main` before this, and `--music native`) the
music player's mixer calls went to the system's mixer.

**Clone Campaigns.** `battlegrounds_x1.exe` imports these from Steam's
`WIN32.dll`, a winmm wrapper (ogg-winmm: it plays `Music\TrackNN.ogg`
through `libvorbisfile-3.dll`, found beside its own DLL, so in the recomp
too; its mixer calls go to the real WINMM). The host binds shims by name, so
Clone Campaigns gets `cdaudio.c` as well, on both hosts, with a slider that
works; `--music native` gives `WIN32.dll` back. On this machine Clone
Campaigns never opens the CD: its `Music Volume` is 9900 (the slider at
zero), in the user's registry and the test farm's profile, and the game
skips its music player then (`0x00427241`).

## How it works

- **The disc.** Track 1 is data (2 s of silence), tracks 2..N are the Ogg
  files, N the highest `TrackNN.ogg` there is; a missing one in between is
  an empty track. Each file's length comes from its last Ogg page
  (stb_vorbis), a few milliseconds a file: 50 tracks in 20-360 ms, once per
  folder.
  Positions are 44.1 kHz samples from the disc's start (588 to a CD frame);
  `MCI_STATUS` answers in milliseconds, MSF or TMSF (a track's length in
  MSF, as WINMM does). A track at another rate is resampled (the Steam
  tracks are all 44.1 kHz stereo).
- **The player.** A thread decodes (`stb_vorbis`, `third_party/stb_vorbis`)
  2048 frames at a time and feeds the output, half a second ahead at most.
  The commands only change what it should play and bump a generation
  number; nothing on the game's thread waits for the player. The position
  is what the output has played (`waveOut`'s finished buffers, or win32hle's
  stream's queue). At the play's end the player waits until it has been
  heard, then posts `MM_MCINOTIFY`: `PostMessageA` on Windows, the machine's
  queue on win32hle (`mach_enter`, `hle_post_message`).
- **No audio device.** The tracks keep time on the clock, so the game's
  notifications still come.
- **The shims.** `cdaudio_mciSendCommandA`, `cdaudio_mciGetErrorStringA` and
  the seven `mixer*` imports, in `host.c`'s table and `shims.c`'s.
  `mciSendStringA` is not imported by either game.

## Tests

- **Unit** (`unit/cdaudio`, `tests/unit/cdaudio_test.c`, both hosts): uses
  an injected output sink (`cdaudio_test_sink`, no real-time waits) to test
  that `tests/data/tone.ogg` (2 s, 441 Hz left, 882 Hz right, made with
  ffmpeg's libvorbis) decodes to its length, rate, tones and level; a seek
  then a decode matches the decode from the start at seven places; a disc
  of three copies answers MCI_STATUS's counts, lengths and positions in
  three formats; a play from track 2 to 3 puts exactly that track's samples
  on the output, then notifies; both the CD and wave-out mixer lines are
  ours while other lines go to WINMM; the cubic curve gives 1/8 amplitude at
  50% and silence at the 1% bottom step, while the sound slider drives
  `cdaudio_effects_volume` and leaves the music untouched; stop, pause and
  a second play abort or supersede the notification; bad tracks and devices
  are refused; no tracks or `--music off` fail the open.
- **Functional** (`music/base`, `tools/test.py`, both hosts): a skirmish
  with `--music DIR`, DIR a disc of 2-second tracks (copies of the
  fixture), so the game's whole play ends inside the check. It passes when
  the disc reads as tracks 1-50, the game gets 50 from `MCI_STATUS` and sets
  the volume, nothing plays before Start Game, tracks 2-49 play in order 2 s
  apart, the 48 take 96 s, the notification follows and the game plays
  2-49 again. Clone Campaigns is not checked (its music is off, above).
- **Functional sliders** (`audio/base`, `tools/test.py`, Windows host):
  drives Options in the main menu and a skirmish with `--music` on the tone
  fixture under `--record`. Verifies the menu's DirectSound MP3 is loud at
  the sound slider's top and silent at its bottom (1%, -120 dB), the music
  slider reaches 1% (-120 dB) and 100% (0 dB), and in-game CD music plays
  at the fixture's full level and 441 Hz pitch while the sound slider stays
  at the bottom.

Measured 2026-11-26: Windows, tracks 3-49 2.00 s apart, 96.0 s; Linux (WSL
Ubuntu, clang `-m32`, SDL's dummy audio), 1.90-2.10 s apart, 95.7 s. With
the real tracks, a test run's audio session is muted while its meter shows
the music (peaks 0.04-0.19 at the slider's 50%).
