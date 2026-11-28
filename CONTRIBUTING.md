# Contributing

Issues and pull requests are welcome.

## The one rule

**Nothing from the game goes in the repository.** No game files, no lifted C
(`src/recomp/gen/`), no catalog dumps (`work/`), no disassembly listings,
nothing generated from `Battlegrounds.exe`. The tool ships; its output is made on each
person's machine from their own copy. `.gitignore` covers the usual paths;
check `git status` before you commit anyway. Screenshots of the game running
are fine.

## Where your code comes from

Write it yourself, from the game's own behavior, Microsoft's documentation and
pcrecomp. Don't bring in code from GPL or LGPL projects (Wine, ReactOS,
DOSBox, ScummVM) or from wrappers such as dgVoodoo or nGlide, and don't write
code with them open. Permissively licensed code is fine with its notice kept:
add a row to [NOTICE.md](NOTICE.md) and its license text under `LICENSES/`.

## Before a pull request

1. Build both hosts: `build.cmd` on Windows, `./build-linux.sh` on Linux (WSL
   is fine). After a lift change, lift first: `py -3 run_lift.py --all`
   (expected: `errors 0`).
2. Run the merge gate on your build, which must exit 0:
   ```
   py -3 tools\test.py --level functional --cc --retry 1
   ```
   About 20 minutes; `--build` and `--prefix` point it at a worktree's build
   ([docs/testing.md](docs/testing.md)). For a change that can't reach the
   game (docs, a tool's own logic), `--level unit` is enough.
3. A new behavior comes with a check that shows it: a unit in `tests/unit/`
   or a check in `tools/test.py`.
4. A wall you got past goes in the docs: what it looked like, what it was,
   what moved it.

## Habits

- **Resolving a merge conflict, rebuild both hosts.** Never fix one with a
  text substitution that can turn `\n` into a real newline: it broke the
  Linux host's build once.
- **No machine paths in code.** Defaults come from the checkout, or from
  `SWGB_STAR` for a shared test farm.
- **One home per helper.** Before adding a function to a tool or a host,
  search for its name and its job. Code with no Win32 in it goes in a file
  both hosts compile (the `winport.h` pattern: `dplay.c`, `keyscript.c`).
- **Register every flag and environment variable:** the usage text of both
  hosts, and [docs/building.md](docs/building.md#environment-variables) for
  a variable (a unit test checks that one).
- **No real-time waits in unit tests.** Inject the clock and drive it, as
  `present_test_clock_us` does. In scripted game runs, prefer `on:NAME` to
  a fixed delay.
- **Test seams are hooks, not modes:** a function pointer the test sets,
  not a flag production code branches on.
- **A check never writes its own reference** except under
  `--update-baselines`, and a check that passes only on a retry is a bug
  to look at, not a pass.
- **Status lives in one place.** The README's table is where things stand,
  [ROADMAP.md](ROADMAP.md) what is next, [CHANGELOG.md](CHANGELOG.md) what
  changed. A PR that changes a number (a function count, a line count, an
  optimization level) greps the docs for the old value.
- **Compile options per target** in CMake, never a second hand-kept list of
  sources.

## Where a fix belongs

- A bug in the lifter, the catalog or the `native32` runtime belongs in
  [pcrecomp](https://github.com/sp00nznet/pcrecomp), not here.
- A fix to the game itself is a patch in `run_lift.py` or a shim in
  `src/runtime/`, with a comment that says what it fixes and why.
