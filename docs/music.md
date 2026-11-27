# Music

Galactic Battlegrounds plays its in-game music from disc 2's audio tracks,
through WINMM's MCI `cdaudio` device. A Steam install has no audio CD: it has
`Game\Music\Track02.ogg` ... `Track50.ogg` (CD track 1 is the data track).
`src/runtime/cdaudio.c` is that CD, on both hosts: the MCI commands the
game sends, answered over the Ogg files, and the mixer's CD line, whose
volume is the music's.

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
| Start-up | `MCI_OPEN` (`MCI_OPEN_TYPE_ID`, `MCI_DEVTYPE_CD_AUDIO`, shareable, wait), `MCI_SET` `MCI_FORMAT_TMSF`, `MCI_STATUS` `MCI_STATUS_NUMBER_OF_TRACKS` (50). Then the mixer: the first device with a `MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC` line, that line's volume control (`MIXERCONTROL_CONTROLTYPE_VOLUME`), and its value, which it restores when it closes the CD. Then the music slider's value: `volume 50%` from the registry's `Music Volume` 5000. |
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

**The music slider.** The game's slider (0-100) becomes `-100*v`, then the CD
line's volume control value, linearly over the control's range: half the
slider is half the amplitude (as Steam's `WIN32.dll` scales its samples).
The game's other mixer user, the sound effects' slider (`0x00542E00`, the
`MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT` line), still goes to WINMM on Windows,
where Vista and later give a program's wave line its own session volume.
Only the CD line is ours, so the system's own CD line, where a driver still
has one, is never touched; with the real WINMM (`main` before this, and
`--music native`) the music player's mixer calls went to the system's mixer.

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

- **Unit** (`unit/cdaudio`, `tests/unit/cdaudio_test.c`, both hosts): the
  fixture `tests/data/tone.ogg` (2 s, 441 Hz left, 882 Hz right, made with
  ffmpeg's libvorbis) decodes to its length, rate, tones and level; a seek
  then a decode matches the decode from the start at seven places; a disc
  of three copies answers MCI_STATUS's counts, lengths and positions in
  three formats; a play from track 2 to 3 puts exactly that track's samples
  on the output, then notifies; half the CD line's volume halves them; stop,
  pause and a second play abort or supersede the notification; bad tracks
  and devices are refused; no tracks or `--music off` fail the open.
- **Functional** (`music/base`, `tools/test.py`, both hosts): a skirmish
  with `--music DIR`, DIR a disc of 2-second tracks (copies of the
  fixture), so the game's whole play ends inside the check. It passes when
  the disc reads as tracks 1-50, the game gets 50 from `MCI_STATUS` and sets
  the volume, nothing plays before Start Game, tracks 2-49 play in order 2 s
  apart, the 48 take 96 s, the notification follows and the game plays
  2-49 again. Clone Campaigns is not checked (its music is off, above).

Measured 2026-11-26: Windows, tracks 3-49 2.00 s apart, 96.0 s; Linux (WSL
Ubuntu, clang `-m32`, SDL's dummy audio), 1.90-2.10 s apart, 95.7 s. With
the real tracks, a test run's audio session is muted while its meter shows
the music (peaks 0.04-0.19 at the slider's 50%).
