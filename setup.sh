#!/bin/sh
# Setup for Linux (Debian, Ubuntu, WSL2; others with their own packages), the
# native host on pcrecomp's win32hle, no Wine: tools/setup.ps1's steps.
#
# It checks the tools and offers to install the missing ones, fetches the
# pcrecomp it builds against (tools/pcrecomp_pin.sh), copies your installed
# game into game/ (the Steam Saga release is unwrapped; a disc install is
# updated to v1.1 from BGUpdate1_1.exe), finds disc 2, finds every function,
# lifts Battlegrounds.exe to C, builds build-linux/swgbrecomp and writes
# play.sh to start it. A step whose output is there is skipped (--force
# redoes them), so a rerun after a failure resumes. The details go to
# setup.log.
#
#   ./setup.sh
#   ./setup.sh --yes --game-dir "/path/to/STAR WARS - Galactic Battlegrounds Saga/Game"
#
#   --game-dir DIR    the installed Game folder (Battlegrounds.exe, Data/GENIE.DAT)
#   --play-disc DIR   disc 2: its mount point or a copy, an .iso or a .cue (not for Steam)
#   --patch-exe FILE  LucasArts' BGUpdate1_1.exe, when the install is v1.0
#   --yes, -y         answer yes to every question (installs too)
#   --force           redo every step
#
# PCRECOMP picks the toolkit checkout (default ../pcrecomp); CC the compiler;
# SUDO_ASKPASS the password helper when there is no terminal.
# macOS: build-macos.sh by hand (docs/linux-macos.md).
set -u
cd "$(dirname "$0")" || exit 1
ROOT=$(pwd)
LOG=$ROOT/setup.log
YES=0 FORCE=0 GAME_DIR= PLAY_DISC= PATCH_EXE=
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--yes) YES=1 ;;
    --force) FORCE=1 ;;
    --game-dir|--game) GAME_DIR=${2:-}; shift ;;
    --play-disc) PLAY_DISC=${2:-}; shift ;;
    --patch-exe) PATCH_EXE=${2:-}; shift ;;
    -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see ./setup.sh --help)" >&2; exit 2 ;;
  esac
  shift
done
PCRECOMP=${PCRECOMP:-$(cd .. && pwd)/pcrecomp}
export PCRECOMP
VENV=$ROOT/work/venv
PY=$VENV/bin/python
. "$ROOT/tools/pcrecomp_pin.sh"

echo "==== setup $(date '+%Y-%m-%dT%H:%M:%S')" >>"$LOG"
if [ -t 1 ]; then C_STEP='\033[36m' C_BAD='\033[31m' C_WARN='\033[33m' C_OK='\033[32m' C_END='\033[0m'
else C_STEP= C_BAD= C_WARN= C_OK= C_END=; fi
say() { printf '%s\n' "$*"; printf '%s\n' "$*" >>"$LOG"; }
colour() { printf "$1%s$C_END\n" "$2"; printf '%s\n' "$2" >>"$LOG"; }
step() { echo; colour "$C_STEP" "[$1/7] $2"; }
fail() {
  say ""
  colour "$C_BAD" "Setup stopped: $*"
  colour "$C_WARN" "The details are in $LOG. Fix that and run ./setup.sh again; finished steps are skipped."
  exit 1
}
ask() {
  if [ "$YES" = 1 ]; then say "$1 yes"; return 0; fi
  printf '%s [Y/n] ' "$1"
  read -r a || a=n
  echo "$1 $a" >>"$LOG"
  case "$a" in [nN]*) return 1 ;; *) return 0 ;; esac
}
ask_path() {   # question -> the answer on stdout
  [ "$YES" = 1 ] && return 1
  printf '%s ' "$1" >&2
  read -r a || return 1
  a=${a#\"}; a=${a%\"}; a=${a#\'}; a=${a%\'}
  [ -n "$a" ] && printf '%s\n' "$a"
}
# Run a command with its output in the log; stop when it fails.
run() {
  what=$1; shift
  say "  $what..."
  echo "\$ $*" >>"$LOG"
  "$@" >>"$LOG" 2>&1 || fail "$what failed (exit code $?)."
}
have() { command -v "$1" >/dev/null 2>&1; }
# sudo for the installs; with no terminal (a launcher), SUDO_ASKPASS's helper asks.
if [ "$(id -u)" = 0 ]; then SUDO=
elif [ ! -t 0 ] && [ -n "${SUDO_ASKPASS:-}" ]; then SUDO="sudo -A"
else SUDO=sudo; fi

say "Star Wars: Galactic Battlegrounds: static recompilation setup (Linux, native)"
say "You need your own copy: the Steam Saga release, or the game installed from its US disc 1 with"
say "disc 2 (or an .iso/.cue of it) and LucasArts' BGUpdate1_1.exe if the install is v1.0; and about 8 GB free."

# ---------------------------------------------------------------- tools
step 1 "Checking the tools"
case "$(uname -m)" in x86_64|i?86) ;; *) fail "this needs an x86 PC (the game is 32-bit x86 code compiled for it); this one is $(uname -m)." ;; esac

# What is missing, as words for the user and package names for apt.
MISSING= APT=
need() { MISSING="$MISSING${MISSING:+, }$1"; APT="$APT $2"; }
check_tools() {
  MISSING= APT=
  if ! have python3 || ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
    need "Python 3.10 or newer" "python3 python3-venv"
  elif ! python3 -c 'import ensurepip, venv' 2>/dev/null && [ ! -x "$PY" ]; then
    need "Python's venv" python3-venv
  fi
  have git || need git git
  have cmake || need cmake cmake
  have ninja || need ninja ninja-build
  have pkg-config || need pkg-config pkg-config
  have fc-match || need fontconfig fontconfig
  if ! fc-list 2>/dev/null | grep -qi 'Liberation Sans'; then need "the Liberation fonts" fonts-liberation; fi
  have clang || need clang clang
  # -m32: 32-bit libc and libgcc (gcc-multilib), for clang as well as gcc.
  t=$(mktemp -d)
  printf 'int main(void){return 0;}\n' >"$t/m.c"
  cc=${CC:-$(have clang && echo clang || echo gcc)}
  "$cc" -m32 "$t/m.c" -o "$t/m" >/dev/null 2>&1 || need "32-bit C libraries (-m32)" gcc-multilib
  # SDL2 and SDL2_ttf for i386: a program that links them with -m32 (the
  # 64-bit ones answer pkg-config just as well, so only linking tells).
  printf '#include <SDL.h>\n#include <SDL_ttf.h>\nint main(int c, char **v){(void)c;(void)v;SDL_Init(0);return TTF_Init();}\n' >"$t/s.c"
  flags=$(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/lib/pkgconfig \
          pkg-config --cflags --libs sdl2 SDL2_ttf 2>/dev/null)
  # shellcheck disable=SC2086  # a list of flags
  "$cc" -m32 "$t/s.c" $flags -o "$t/s" >/dev/null 2>&1 || need "SDL2 and SDL2_ttf for i386" "libsdl2-dev:i386 libsdl2-ttf-dev:i386"
  rm -rf "$t"
}
check_tools
if [ -n "$MISSING" ]; then
  say "  Missing: $MISSING."
  if have apt-get; then
    pk=$(echo $APT | tr ' ' '\n' | sort -u | tr '\n' ' ')
    case "$pk" in *:i386*) arch="sudo dpkg --add-architecture i386 && " ;; *) arch= ;; esac
    say "  The packages: $pk"
    ask "  Install them now (${arch}sudo apt-get install, about 600 MB)?" || fail "these are needed: $MISSING."
    case "$pk" in *:i386*)
      dpkg --print-foreign-architectures | grep -qx i386 || run "Adding the i386 architecture" $SUDO dpkg --add-architecture i386 ;;
    esac
    run "Updating the package lists" $SUDO apt-get update
    # shellcheck disable=SC2086  # a list of packages
    run "Installing $pk" $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $pk
    hash -r 2>/dev/null
    check_tools
    [ -z "$MISSING" ] || fail "still missing after the install: $MISSING."
  else
    say "  Install them with your package manager, then run ./setup.sh again:"
    say "    Fedora: sudo dnf install python3 git cmake ninja-build pkgconf fontconfig liberation-fonts clang glibc-devel.i686 libgcc.i686 SDL2-devel.i686 SDL2_ttf-devel.i686"
    say "    Arch: sudo pacman -S python git cmake ninja pkgconf fontconfig ttf-liberation clang lib32-glibc lib32-gcc-libs, and lib32-sdl2-compat lib32-sdl2_ttf from the AUR"
    fail "these are needed: $MISSING."
  fi
fi
say "  C compiler: $(${CC:-$(have clang && echo clang || echo gcc)} --version | head -1)"
if ! have ffmpeg; then
  say "  ffmpeg is not installed: the films (and --record) need it. The game runs without."
  if have apt-get && ask "  Install ffmpeg now (sudo apt-get install, about 100 MB)?"; then
    run "Installing ffmpeg" $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ffmpeg
  fi
fi

# Python packages in a virtual environment of this checkout (work/venv), so
# the system's Python is left alone (Debian's refuses pip --user anyway).
if [ ! -x "$PY" ] || ! "$PY" -c 'import capstone, pefile, cryptography' 2>/dev/null; then
  say "  The Python packages capstone, pefile and cryptography go in work/venv (about 30 MB)."
  ask "  Install them now (pip, into work/venv only)?" || fail "capstone, pefile and cryptography are required."
  mkdir -p work
  [ -x "$PY" ] || run "Making work/venv" python3 -m venv --system-site-packages "$VENV"
  run "Installing capstone, pefile and cryptography" "$PY" -m pip install --disable-pip-version-check capstone pefile cryptography
fi
say "  Python: $PY ($("$PY" --version 2>&1))"

# ---------------------------------------------------------------- pcrecomp
step 2 "The pcrecomp toolkit"
if [ ! -d "$PCRECOMP" ]; then
  say "  pcrecomp is not beside this folder ($PCRECOMP)."
  ask "  Download it now (git, about 20 MB: $PCRECOMP_PIN_URL, branch $PCRECOMP_PIN_BRANCH)?" || fail "pcrecomp is required at $PCRECOMP."
  run "Cloning pcrecomp" pcrecomp_pin_clone "$PCRECOMP"
fi
why=$(pcrecomp_pin_why "$PCRECOMP")
if [ -n "$why" ]; then
  say "  $why: the Linux host needs it."
  if [ -d "$PCRECOMP/.git" ] || git -C "$PCRECOMP" rev-parse --git-dir >/dev/null 2>&1; then
    ask "  Fetch it and check it out in $PCRECOMP (git fetch, then a detached checkout)?" ||
      fail "point PCRECOMP at a pcrecomp with that commit (sh tools/pcrecomp_pin.sh fetch DIR makes one)."
    run "Updating pcrecomp" pcrecomp_pin_update "$PCRECOMP"
  fi
  why=$(pcrecomp_pin_why "$PCRECOMP")
  [ -z "$why" ] || fail "$why."
fi
say "  pcrecomp: $PCRECOMP ($(git -C "$PCRECOMP" rev-parse --short HEAD))"

# ---------------------------------------------------------------- the game
step 3 "Copying your copy of Galactic Battlegrounds into game/"
# DIR/REL, each name matched without regard to case; the path on stdout.
ci() {
  p=$1 rest=$2
  while [ -n "$rest" ]; do
    n=${rest%%/*}
    case "$rest" in */*) rest=${rest#*/} ;; *) rest= ;; esac
    m=$(find "$p" -mindepth 1 -maxdepth 1 -iname "$n" 2>/dev/null | head -n 1)
    [ -n "$m" ] || return 1
    p=$m
  done
  printf '%s\n' "$p"
}
is_game() { [ -n "$1" ] && ci "$1" Battlegrounds.exe >/dev/null && ci "$1" Data/GENIE.DAT >/dev/null; }
is_disc2() { [ -n "$1" ] && ci "$1" Game/Avi >/dev/null && ! ci "$1" GameData >/dev/null; }
SAGA="STAR WARS - Galactic Battlegrounds Saga"
# Steam libraries: the default ones and every one in libraryfolders.vdf.
steam_libs() {
  for s in "$HOME/.steam/steam" "$HOME/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
           /mnt/?/"Program Files (x86)"/Steam /mnt/?/Steam /mnt/?/SteamLibrary /mnt/?/Games/SteamLibrary; do
    [ -d "$s/steamapps" ] || continue
    echo "$s"
    vdf=$s/steamapps/libraryfolders.vdf
    [ -f "$vdf" ] || continue
    sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | sed 's/\\\\/\\/g' | while IFS= read -r l; do
      case "$l" in
        [A-Za-z]:\\*) d=$(printf '%s' "$l" | cut -c1 | tr 'A-Z' 'a-z'); l="/mnt/$d/$(printf '%s' "$l" | cut -c4- | tr '\\' /)" ;;
      esac
      echo "$l"
    done
  done
}
find_game() {   # every candidate, the likeliest first
  steam_libs | while IFS= read -r s; do
    g="$s/steamapps/common/$SAGA/Game"
    if is_game "$g"; then echo "$g"; fi
  done
  for p in "$HOME/.wine/drive_c" /mnt/c; do
    for d in "$p/Program Files (x86)/LucasArts/Star Wars Galactic Battlegrounds/Game" \
             "$p/Program Files/LucasArts/Star Wars Galactic Battlegrounds/Game"; do
      if is_game "$d"; then echo "$d"; fi
    done
  done
}

stamp() {   # the PE time stamp of $1, in hex
  "$PY" -c 'import struct,sys; d=open(sys.argv[1],"rb").read(4096); pe=struct.unpack_from("<I",d,0x3C)[0]; print("%08X" % struct.unpack_from("<I",d,pe+8)[0])' "$1"
}
steam_wrapped() {   # SteamStub's .bind section
  "$PY" -c 'import pefile,sys; pe=pefile.PE(sys.argv[1],fast_load=True); sys.exit(0 if any(s.Name.rstrip(b"\0")==b".bind" for s in pe.sections) else 1)' "$1"
}
STEAM=0
if is_game game && [ -f work/Battlegrounds.exe ] && [ "$FORCE" = 0 ]; then
  say "  game/ is already there (skipping)."
  [ -f work/steam ] && STEAM=1
else
  G=$GAME_DIR
  if [ -z "$G" ]; then
    G=$(find_game | head -n 1)
    [ -n "$G" ] && say "  Found the installed game in $G"
  fi
  if [ -z "$G" ] && [ "$YES" = 1 ]; then fail "the installed game wasn't found. Give its Game folder with --game-dir."; fi
  while [ -z "$G" ]; do
    G=$(ask_path "  Where is the installed game? Its Game folder (with Battlegrounds.exe):") || fail "no game folder given."
  done
  is_game "$G" || fail "$G has no Battlegrounds.exe with Data/GENIE.DAT beside it. Point setup at the Game folder of the Steam release or of an install from disc 1."
  mkdir -p game work
  say "  Copying the installed game (about $(du -sh "$G" 2>/dev/null | cut -f1))..."
  cp -R "$G/." game/ >>"$LOG" 2>&1 || fail "copying $G into game/ failed."
  rm -f work/steam
  if steam_wrapped "$(ci game Battlegrounds.exe)"; then
    STEAM=1
    : >work/steam
    run "Unwrapping Steam's Battlegrounds.exe (SteamStub)" "$PY" tools/steamstub_swgb.py "$(ci game Battlegrounds.exe)" work/Battlegrounds.exe
    cp work/Battlegrounds.exe "$(ci game Battlegrounds.exe)"
  fi
fi
EXE=$(ci game Battlegrounds.exe)
if [ "$(stamp "$EXE")" != 3C03FD69 ]; then
  [ "$STEAM" = 0 ] || fail "game/Battlegrounds.exe is not v1.1 after the unwrap (time stamp $(stamp "$EXE"))."
  P=$PATCH_EXE
  if [ -z "$P" ]; then
    for d in "$ROOT" "$(dirname "$ROOT")" "$HOME/Downloads"; do
      f=$(ci "$d" BGUpdate1_1.exe) && { P=$f; say "  Found $P"; break; }
    done
  fi
  if [ -z "$P" ] && [ "$YES" = 1 ]; then fail "game/Battlegrounds.exe is not v1.1. Give LucasArts' BGUpdate1_1.exe with --patch-exe."; fi
  while [ -z "$P" ]; do
    P=$(ask_path "  The game is v1.0. Where is BGUpdate1_1.exe (the v1.1 update)?") || fail "the v1.1 update is required."
  done
  if ! have cabextract; then
    have apt-get && ask "  The update is a cabinet: install cabextract (sudo apt-get install)?" &&
      run "Installing cabextract" $SUDO apt-get install -y cabextract
    have cabextract || fail "cabextract is needed to unpack the update."
  fi
  run "Unpacking the v1.1 update" "$PY" tools/bgupdate.py "$P" work/patch
  cp work/patch/Eng/Game/Battlegrounds.exe "$EXE"
  cp work/patch/Common/Game/Data/GENIE.DAT "$(ci game Data/GENIE.DAT)"
  [ "$(stamp "$EXE")" = 3C03FD69 ] || fail "the update did not give the v1.1 Battlegrounds.exe. Is it the US English BGUpdate1_1.exe?"
fi
[ "$STEAM" = 1 ] || cp "$EXE" work/Battlegrounds.exe

# Disc 2, the play disc: the game looks for it each time it starts. The Steam
# release has none and needs none; the host takes a folder with Game/ in it as
# drive D: (src/linux/main.c, cd_pick), so it gets an empty one.
CD=
if [ "$STEAM" = 1 ]; then
  mkdir -p work/disc/Game/Avi
  CD=$ROOT/work/disc
  say "  The Steam release: no disc 2 needed (work/disc stands in for it)."
elif [ -f work/cd ] && [ -z "$PLAY_DISC" ] && [ "$FORCE" = 0 ] && is_disc2 "$(cat work/cd)"; then
  CD=$(cat work/cd)
  say "  Disc 2: $CD"
else
  CD=$PLAY_DISC
  if [ -z "$CD" ]; then
    for d in /media/"$USER"/* /run/media/"$USER"/* /media/* /mnt/cdrom /cdrom; do
      is_disc2 "$d" && { CD=$d; say "  Disc 2 is mounted at $CD."; break; }
    done
  fi
  if [ -z "$CD" ] && [ "$YES" = 1 ]; then fail "disc 2 (the play disc) wasn't found. Mount it, or give its folder, .iso or .cue with --play-disc."; fi
  while [ -z "$CD" ]; do
    CD=$(ask_path "  Where is disc 2, the play disc? Its mount point, an .iso or a .cue:") || fail "disc 2 is required."
  done
  # An image is unpacked (its data track) into work/disc2: no mount, no root.
  case "$CD" in
    *.cue|*.CUE)
      [ -f work/disc2.iso ] && [ "$FORCE" = 0 ] || run "Converting $(basename "$CD") to work/disc2.iso" "$PY" tools/cue2iso.py "$CD" work/disc2.iso
      CD=$ROOT/work/disc2.iso ;;
  esac
  case "$CD" in
    *.iso|*.ISO)
      if ! have bsdtar && ! have 7z; then
        have apt-get && ask "  Unpacking the image needs bsdtar: install libarchive-tools (sudo apt-get install)?" &&
          run "Installing libarchive-tools" $SUDO apt-get install -y libarchive-tools
      fi
      mkdir -p work/disc2
      if have bsdtar; then run "Unpacking $(basename "$CD") into work/disc2" bsdtar -xf "$CD" -C work/disc2
      elif have 7z; then run "Unpacking $(basename "$CD") into work/disc2" 7z x -y -owork/disc2 "$CD"
      else fail "bsdtar or 7z is needed to unpack $CD (or mount it and give the mount point)."; fi
      CD=$ROOT/work/disc2 ;;
  esac
  is_disc2 "$CD" || fail "$CD is not disc 2 (it has no Game/Avi)."
  CD=$(cd "$CD" && pwd)
  printf '%s\n' "$CD" >work/cd
fi

# ---------------------------------------------------------------- catalog
step 4 "Finding every function (about ten minutes)"
if [ -f work/functions.json ] && [ "$FORCE" = 0 ]; then say "  Already done (skipping)."
else run "Disassembling Battlegrounds.exe" "$PY" "$PCRECOMP/tools/disasm/disasm32.py" work/Battlegrounds.exe -o work/functions.json
fi

# ---------------------------------------------------------------- lift
step 5 "Recompiling Battlegrounds.exe to C (two to five minutes)"
if [ -f src/recomp/gen/recomp_dispatch.c ] && [ "$FORCE" = 0 ]; then say "  Already done (skipping)."
else run "Lifting" "$PY" run_lift.py --all
fi

# ---------------------------------------------------------------- build
step 6 "Building build-linux/swgbrecomp (4M lines of C: minutes on many cores, longer on few)"
[ "$FORCE" = 1 ] && rm -rf build-linux
run "Compiling" env PCRECOMP="$PCRECOMP" ./build-linux.sh
[ -x build-linux/swgbrecomp ] || fail "the build made no build-linux/swgbrecomp."
run "Checking the host (a dry run: maps the game and binds its imports)" build-linux/swgbrecomp --game game

# ---------------------------------------------------------------- launcher
step 7 "Making play.sh"
cat >play.sh <<EOF
#!/bin/sh
# Made by setup.sh: starts the game in a window (F11 fullscreen, F12 scaling).
# Anything after it goes to the host: ./play.sh --fullscreen, --res 1920x1080, --films DIR
cd "\$(dirname "\$0")" && exec build-linux/swgbrecomp --run --game game --cd "$CD" "\$@"
EOF
chmod +x play.sh
say "  $ROOT/play.sh"
APPS=${XDG_DATA_HOME:-$HOME/.local/share}/applications
if [ -d "$APPS" ] || have xdg-open; then
  if ask "  Add Galactic Battlegrounds to your applications menu ($APPS)?"; then
    mkdir -p "$APPS"
    cat >"$APPS/swgb-recomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Galactic Battlegrounds (recompiled)
Comment=Star Wars: Galactic Battlegrounds, statically recompiled
Exec="$ROOT/play.sh"
Path=$ROOT
Terminal=false
Categories=Game;StrategyGame;
EOF
    say "  $APPS/swgb-recomp.desktop"
  fi
fi

echo
colour "$C_OK" "Done. Run ./play.sh to start it."
exit 0
