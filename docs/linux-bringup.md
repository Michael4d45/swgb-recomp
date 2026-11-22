# Continuing the Linux port on a Linux machine

A checklist for picking up Track 7 on a native Ubuntu install. It covers
where things stand, how to get the code, the game and the lift onto Linux,
how to build, what to expect on the first runs, and the work in order.
[linux-macos.md](linux-macos.md) has the design and the reasons; this page
is the steps.

## Where it stands (2026-11-22)

- Branch `track/linux-macos` on the fork, PR
  [Michael4d45/swgb-recomp#1](https://github.com/Michael4d45/swgb-recomp/pull/1).
- **Linked and run on Linux (Omarchy/Arch, `-m32`):**
  - SteamStub unwrapped with Steamless under mono; catalog + lift
    (`errors 0`); `./build-linux.sh` produces `build-linux/swgbrecomp`.
  - Dry run: `0 imports not implemented yet`.
  - Headless `--shot` reaches the main menu with GDI labels (needs
    pcrecomp `swgb-linux-palette` or newer: 8bpp TextOut + surface
    palette). DirectPlay Create + VERSION 4.06.03.0516 required or boot
    MessageBox-exits. Host shows the SDL cursor for playtesting.
  - `tools/linux_menu_shots.sh` captures boot + Single Player hover PNGs.
  - `src/linux/eh.c` supplies `recomp_eh_push`/`pop` (win32hle has no
    guest EH yet).
- pcrecomp: branch `swgb-linux-palette` (tip **`4d69543`**: 8bpp TextOut +
  live DD palette nearest-match, softdd's `sync_dib_palette`). Build with
  `PCRECOMP=~/star/pcrecomp`.
- Arch notes: no `lib32-sdl2` in repos — use AUR `lib32-sdl2-compat` and
  `lib32-sdl2_ttf`. Ubuntu packages in §1 still apply on Debian/Ubuntu.

## 1. Packages

Ubuntu 24.04 (Debian is the same):

```sh
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install git gcc-multilib cmake ninja-build pkg-config python3-pip ffmpeg \
                 fonts-liberation libsdl2-dev:i386 libsdl2-ttf-dev:i386
python3 -m pip install --user capstone pefile   # only to lift on Linux (step 3b)
```

`fonts-liberation`: win32hle draws Arial and MS Sans Serif with Liberation.
`ffmpeg` (the program) is for `--record`. For movies later, add
`libavformat-dev:i386 libavcodec-dev:i386 libswresample-dev:i386
libavutil-dev:i386`.

Check that `-m32` works:

```sh
echo 'int main(void){return 0;}' > /tmp/m.c && gcc -m32 /tmp/m.c -o /tmp/m && echo ok
```

## 2. The code

The build expects pcrecomp beside the repository, as on Windows:

```sh
mkdir -p ~/star && cd ~/star
git clone -b track/linux-macos https://github.com/Michael4d45/swgb-recomp.git
git clone https://github.com/sp00nznet/pcrecomp.git       # ../pcrecomp
```

## 3. The game and the lift

Nothing from the game is in git. Copy it from the Windows drive, which is
`A:` on Windows. Mount it on Linux first; the path below assumes
`/mnt/a`. Copy onto the Linux disk rather than running from the NTFS
mount: it is faster, and saved games are written there.

```sh
W=/mnt/a/Games/star/swgb-recomp
cd ~/star/swgb-recomp
cp -r "$W/game" game                       # 2.4 GB; its Battlegrounds.exe is already unwrapped (v1.1, 0x3C03FD69)
mkdir -p work && cp "$W/work/Battlegrounds.exe" "$W/work/functions.json" work/
```

The Steam `Battlegrounds.exe` is SteamStub-wrapped. Use the unwrapped
one in `game/` or `work/`, never a fresh copy from the Steam folder.

Then get the lifted C, one of two ways:

- **a. Copy it** (238 MB). This is only safe if pcrecomp is at
  `a67767b`, since the C must match the runtime headers:
  `mkdir -p src/recomp && cp -r "$W/src/recomp/gen" src/recomp/gen`
- **b. Lift it on Linux** (a few minutes; `functions.json` saves the
  10-minute catalog): `python3 run_lift.py --all`. The expected end is
  `errors 0`. Do this after updating pcrecomp.

## 4. Build

```sh
./build-linux.sh
```

This configures `build-linux/` with `cmake/linux-i386.cmake` and builds
`build-linux/swgbrecomp`. The lift is about 15 CPU-minutes with gcc, so a
few minutes with every core.

- `CMAKE_ARGS="-DGEN_OPT=-O0"` builds faster for debugging.
- `PCRECOMP=~/star/pcrecomp-linux` points at a pcrecomp worktree (step 6).
- To reconfigure, delete `build-linux/`.

Likely first build errors, and where they are:

- **Link errors for undefined `sub_XXXXXXXX` or `recomp_*` symbols:** the
  lift does not match the pcrecomp version (step 3).
- **A duplicate symbol between `src/linux` and win32hle:** rename ours.
- **`cmake/native.cmake` mistakes:** it has never run past the SDL2 check.

## 5. The first runs

Run them in this order:

```sh
B=build-linux/swgbrecomp
$B --game game                                    # dry run: maps and binds, no game
$B --run --game game --headless --watchdog 60 --files --shot 30 /tmp/menu.bmp
$B --run --game game --headless --watchdog 60 --trace 2>trace.log   # every import call
$B --run --game game                              # a window, with sound; F11 fullscreen, F12 scaling
```

- **The dry run** should print `0 imports not implemented yet`. The census
  found 237 of the 258 imports in win32hle and `shims.c` answers the rest.
  Any import it names goes into `src/linux/shims.c`.
- **Headless:** the goal is `/tmp/menu.bmp` showing the main menu
  (`ffmpeg -i /tmp/menu.bmp /tmp/menu.png` to view it). The Windows
  reference is `docs/screenshots/main-menu.png`.
- **When a run stops:**
  - `[not-lifted] sub_X`: a lift gap (the same on Windows?).
  - `=== fault ===`: the last import shown is usually the culprit.
  - `unimplemented: NAME`: a stub was called. Add the import to `shims.c`.
  - `RaiseException ... not handled`: see gap 6 in linux-macos.md.
- **Missing files:** `--files` lists every open. Registry reads that fail
  show in `--trace`. Seed them in `main.c` (`hle_reg_set_string`, as
  `cd_pick` does for `CDPath`).
- **Disc 2:** `--cd /media/$USER/<label>` (a mounted disc or ISO:
  `sudo mount -o loop disc2.iso /mnt/cd`). Without it, the game warns and
  goes on.

## 6. The work, in order

The pcrecomp changes are in pcrecomp's code. Make them in a worktree of
your own, so the shared checkout stays as it is:
`git -C ~/star/pcrecomp worktree add ~/star/pcrecomp-linux -b swgb-linux`,
then build with `PCRECOMP=~/star/pcrecomp-linux`. Where they go (fork,
pull request upstream) is your call.

1. **Reach the main menu headless** (step 5's loop) — done.
2. **Text in 8-bit surfaces** (pcrecomp) — done on `swgb-linux-palette`.
3. **Text fields** — done: EDIT `WM_PRINT` paint in pcrecomp;
   `src/linux/fields.c` composes them into the primary. New Player name
   shows with `--keys` (`docs/testing.md`).
4. **Window behaviour:** check whether the game pauses when the SDL window
   loses focus. On Windows, `host.c`'s `host_wndproc` forces the window
   active. If it pauses, filter `WM_ACTIVATEAPP` there.
5. **A skirmish with the mouse and keys, and sound** (the scripted
   skirmish in `docs/testing.md` works headless too).
6. **CD music:** MCI `cdaudio` over `game/Music/TrackNN.ogg` with
   stb_vorbis. It replaces `s_mciSendCommandA` in `shims.c`, and Windows
   can use the same core. Gap 5 in linux-macos.md lists the MCI calls.
7. **Movies:** once Track 6's player is on `main`, its decoder core with
   frames into `hle_dd_frame_hook` and sound into `hle_audio_stream_*`
   (gap 4).
8. **Housekeeping:**
   - `setup.sh` builds natively instead of stopping (Red Alert 2's
     `setup.sh` is the model);
   - `tools/conformance.py` runs the Linux build;
   - `docs/testing.md` gets a Linux section;
   - update the README status row.

## 7. Before pushing

- Rebase on `origin/main` (other tracks merge first, in roadmap order).
- If `src/runtime/` changed, check that Windows still builds. On Windows:
  `powershell -File tools\worktree_env.ps1 -Name linux-macos -Build`,
  then the printed command with `--headless --multi`.
- Update Track 7 in `ROADMAP.md` and "What is done" in `linux-macos.md`.
- Open PRs on the fork with
  `gh pr create --repo Michael4d45/swgb-recomp --base main`. T3's PR tool
  opened one on upstream once.
