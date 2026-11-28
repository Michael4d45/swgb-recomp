# The films

The game's films (the LucasArts logo, the intro, the campaigns' films) play
in the recompiled game, headless and in `--record` too. You need **ffmpeg
on `PATH`** (it decodes them) and the films themselves (disc 2). Without
either, the game goes on without its films, as it did before.

## What the game does

| Where | What |
|---|---|
| `0x005E104B` | At start, `ICInfo('vidc', 'iv50')`: is Indeo 5 installed? If not, the game plays no film at all. Windows 10 and 11 have no Indeo 5. |
| `0x005E43F0` | Plays a film by name: `avi\NAME.avi` in the game folder, then `CDPath` + `game\avi\NAME.avi` (disc 2). Missing: no film. |
| `0x005ECDF0` | Sets the film's mode: 640x480x16 (800x600x16, then 800x600x8 if that fails), or 640x480x8 first with the game's `8BITVIDEO` switch. A 320x240 film is shown twice its size. |
| `0x005E46C9` | `MCIWndCreateA(game window, 0x50001F0A, path)`: a child window (no menu, no play bar). The game subclasses it (`0x005E48F0`: a click stops the film), asks `MCIWNDM_GETPALETTE` and installs that palette, centres it with `MoveWindow`, and sends `MCI_PLAY`. |
| `0x005EB8AF` | Once a second, `MCIWNDM_GETMODEA`: `"stopped"` (or nothing) ends the film; `"paused"` gets `MCI_RESUME`. |
| `0x005ECC20` | Enter and Space end the film (`0x005E4950(1)`), Esc too (`0x005E4950(0)`). |
| `0x005E4950` | Ends it: `MCI_STOP` unless stopped, `WM_CLOSE`, the game's palette back. |

The names: `logo1` and `intro` at start (the Saga's Clone Campaigns
films are `xlogo1.avi` and `xintro.avi`), and in the
campaigns each scenario's pregame, victory and loss film from its header
(`INTRO` and `celebration` in the base game's; `<None>` is none).

## How the host plays them (`src/runtime/movie.c`, `movie_core.c`)

`movie_core.c` holds the platform-neutral pieces shared with the native
Linux host (`src/linux/movie.c`): the AVI header probe, `--films` name
lookup, the film palette and ordered dither, and the A/V clock
(`tests/unit/films_test.c` checks them on both hosts).

- **`ICInfo`** answers yes for Indeo 5 when ffmpeg is on `PATH` (FFmpeg's
  `indeo5` decoder), and asks Windows for anything else.
- **`MCIWndCreateA`** makes the host's own child window (class
  `swgb.movie`, sized from the AVI header), not MSVFW32's, and answers the
  messages above. A film that is missing or not an AVI gives `NULL`, as a
  failed `MCIWndCreateA` did, and the game goes on.
- **Decoding:** `MCI_PLAY` starts two `ffmpeg` processes on pipes (no build
  dependency, as `--record` does the other way): the video as raw BGRA
  frames at the film's constant rate, the sound as 44.1 kHz 16-bit stereo
  PCM. Only the pipe is inherited by them.
- **Sound and sync:** the sound goes to `waveOut`. Its playing position is
  the clock: each frame is shown when it is due by that clock, and a frame
  late by a whole frame is dropped (one is still shown every 100 ms however
  slow the machine). With no audio device, or a film without sound, the
  wall clock times the film; if the sound ends first, the wall clock carries
  on from where it stopped. Pausing pauses both. Test runs are muted (the
  process's audio session), which leaves the clock running.
- **The picture:** softdd shows a child window only by printing it into
  the frame it presents (`compose()`, `WM_PRINT`), and so the film is drawn
  there. In a 16-bit mode (the default) the frame is stretched in by GDI.
  In an 8-bit mode (`8BITVIDEO`) the palette the window gives the game is a
  6x6x6 colour cube with 40 greys, and the film is ordered-dithered into it
  (4x4 Bayer, per channel, so flat black stays black), as DrawDib did on an
  8-bit screen. Each new frame asks softdd to present now
  (`softdd_refresh`), between the game's own presents.
- **Skipping** is the game's own (above). The window gives the keyboard
  focus back to the game window, which handles the keys.
- `--record` scales a mode other than the first one (the films' 640x480)
  to the recording's size, so the mp4 has the films in it, and their sound:
  every chunk queued on waveOut goes to the recording too, with when it will
  be heard (recaudio's stream tap, [recording.md](recording.md)). A muted
  test run records it all the same.

The log says what played and how the timing held:

```
[movie] playing ...\xintro.avi (320x240, 29.97 fps, with sound)
[movie] xintro.avi: 2120 frames shown, 0 dropped; the last due at 70.70 s, shown at 70.72 s (clock: the sound)
```

## Where the films are

- **Disc 2**, `Game\Avi`, found as for play (`--cd E`, `SWGB_CD`, `CD_DRIVE` alias, or the
  first CD-ROM drive with `Game\Avi` on it; see [building.md](building.md)).
- **`avi\` in the game folder:** copy disc 2's `Game\Avi` there to play
  without the disc in.
- **`--films DIR`:** a folder looked in for a film the game cannot find,
  by its name (`NAME.avi`), then by the Saga's Clone Campaigns name
  (`xNAME.avi`). The Steam install has no disc 2, only Clone Campaigns'
  `xlogo1.avi` and `xintro.avi` in `Game\`: `--films` pointed there plays
  them in place of the base game's logo and intro, which is how this was
  tested.

## Checking it

```
swgbrecomp.exe --run --multi --headless --watchdog 100 --films "<Steam>\Game" --record films.mp4
swgbrecomp.exe --run --multi --headless --watchdog 40 --films "<Steam>\Game" --args 8BITVIDEO --keys "on:logo1,4:shot=logo8.bmp"
swgbrecomp.exe --run --multi --headless --watchdog 30 --films "<Steam>\Game" --keys "on:logo1,3:click:400:300,on:intro,4:esc"
```

The first records both films and the menu after them; the second shows the
8-bit path; the third skips the logo with a click and the intro with Esc.
Without `--films` (and no disc 2) the log shows the four opens missing and
the menu as before.

## Not done yet

- Disc 2's own films: untested here (no disc 2). The path, the names and
  the format (Indeo 5 AVI) are the same as the Clone Campaigns films'.
- Linux and macOS (track 7): the player uses `CreateProcess` pipes,
  `waveOut` and GDI; the decoding side would carry over as it is.
