#!/bin/sh
# The pcrecomp the native (Linux, macOS) host builds against.
#
# The native host needs win32hle changes that are not in pcrecomp's upstream
# main yet: 8-bit GDI text, the EDIT paint, hle_lookup_manual_hook,
# DestroyWindow's focus, GetClipper's error, the EDIT's keys. They are
# on the branch below of the fork, and the build is pinned to one commit of
# it. A checkout whose HEAD contains that commit is fine (a newer one too).
# When the changes are upstream, the pin moves to upstream main.
#
# Sourced by setup.sh, build-linux.sh and build-macos.sh. By hand:
#   sh tools/pcrecomp_pin.sh check [DIR]     exit 0 when DIR has the pinned commit
#   sh tools/pcrecomp_pin.sh fetch [DIR]     clone DIR, or fetch into it and check out the pin
#   sh tools/pcrecomp_pin.sh show            print the URL, branch and commit
# DIR defaults to $PCRECOMP, else ../pcrecomp beside this repository.

PCRECOMP_PIN_URL=https://github.com/Michael4d45/pcrecomp.git
PCRECOMP_PIN_BRANCH=swgb-linux-palette
PCRECOMP_PIN=eb608a2defc492813d2e120a60faa7089215bb7f

# 0 when $1 is a git checkout whose HEAD has the pinned commit in its history.
pcrecomp_pin_ok() {
  [ -d "$1" ] || return 1
  git -C "$1" cat-file -e "$PCRECOMP_PIN^{commit}" 2>/dev/null || return 1
  git -C "$1" merge-base --is-ancestor "$PCRECOMP_PIN" HEAD 2>/dev/null
}

# Why $1 is not usable, in one line (empty when it is).
pcrecomp_pin_why() {
  if [ ! -d "$1" ]; then echo "$1 does not exist"
  elif [ ! -d "$1/runtime/win32hle" ]; then echo "$1 is not a pcrecomp checkout"
  elif ! git -C "$1" rev-parse --git-dir >/dev/null 2>&1; then echo "$1 is not a git checkout, so its version is unknown"
  elif ! pcrecomp_pin_ok "$1"; then echo "$1 does not have commit ${PCRECOMP_PIN%"${PCRECOMP_PIN#???????}"} of $PCRECOMP_PIN_BRANCH ($PCRECOMP_PIN_URL)"
  fi
}

# Clone the pinned commit into $1 (which must not exist).
pcrecomp_pin_clone() {
  git clone --branch "$PCRECOMP_PIN_BRANCH" "$PCRECOMP_PIN_URL" "$1" || return 1
  git -C "$1" checkout -q --detach "$PCRECOMP_PIN"
}

# Bring an existing checkout $1 to the pin: fetch the branch (a shallow clone
# too), then check the commit out detached. Refuses a checkout with changes.
pcrecomp_pin_update() {
  if [ -n "$(git -C "$1" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    echo "$1 has changes of its own; commit or stash them first." >&2
    return 1
  fi
  if git -C "$1" rev-parse --is-shallow-repository 2>/dev/null | grep -q true; then
    git -C "$1" fetch --depth 50 "$PCRECOMP_PIN_URL" "$PCRECOMP_PIN_BRANCH" || return 1
  else
    git -C "$1" fetch "$PCRECOMP_PIN_URL" "$PCRECOMP_PIN_BRANCH" || return 1
  fi
  # The branch may have moved past the pin; GitHub serves a commit by its hash.
  git -C "$1" cat-file -e "$PCRECOMP_PIN^{commit}" 2>/dev/null ||
    git -C "$1" fetch "$PCRECOMP_PIN_URL" "$PCRECOMP_PIN" || return 1
  git -C "$1" checkout -q --detach "$PCRECOMP_PIN"
}

pcrecomp_pin_default_dir() {
  if [ -n "${PCRECOMP:-}" ]; then echo "$PCRECOMP"
  else echo "$(cd "$(dirname "$0")/.." && cd .. && pwd)/pcrecomp"
  fi
}

# Run as a program (not sourced): the subcommands above.
case "$(basename -- "$0")" in
  pcrecomp_pin.sh)
    set -e
    dir=${2:-$(PCRECOMP=${PCRECOMP:-} pcrecomp_pin_default_dir)}
    case "${1:-show}" in
      show) echo "$PCRECOMP_PIN_URL $PCRECOMP_PIN_BRANCH $PCRECOMP_PIN" ;;
      check)
        why=$(pcrecomp_pin_why "$dir")
        if [ -n "$why" ]; then echo "$why"; exit 1; fi
        echo "$dir: ok ($(git -C "$dir" rev-parse --short HEAD))" ;;
      fetch)
        if [ -d "$dir" ]; then pcrecomp_pin_update "$dir"; else pcrecomp_pin_clone "$dir"; fi
        echo "$dir: $(git -C "$dir" rev-parse --short HEAD)" ;;
      *) echo "usage: sh tools/pcrecomp_pin.sh check|fetch|show [DIR]" >&2; exit 2 ;;
    esac
    ;;
esac
