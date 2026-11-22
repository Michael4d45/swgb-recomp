# Working in parallel

Several agents (or people) work on separate roadmap tracks at once, each
in its own git worktree on its own branch. This page is what they share,
and the rules that keep them from breaking each other's work.

## Setup, once per worktree

```powershell
powershell -File tools\worktree_env.ps1 -Name <track> -Build
```

It builds against the shared, prebuilt lifted code (seconds, not 30+
minutes), makes the worktree a game folder of its own, and prints the
command line to run. Rebuild after a change with the same command.

## What is shared, and how

| Resource | Shared how | Rule |
|---|---|---|
| Lifted C, `src\recomp\gen` | read by every worktree (`GEN_DIR`) | Never re-lift in the main checkout while tracks are open. A track that re-lifts (Clone Campaigns) uses its own gen directory and `-OwnGen`. |
| `swgb_gen.lib` | `A:\Games\star\shared\swgb_gen.lib`, linked by every worktree (`SWGB_GEN_LIB`) | Frozen: replaced only by the integrator, between merges. |
| `work\Battlegrounds.exe` | the main checkout's, passed with `--exe` | Read-only. |
| Game data | `A:\Games\star\games\<track>`: hard links to the main checkout's `game\` | Hard links share the bytes: never edit, patch or delete a game file there. What the game writes (player profiles, `SaveGame`, `Scenario`) is a real copy. `-FreshGame` resets it. |
| The game's options (Screen Size, speeds, volumes) | HKCU, keys under `LucasArts Entertainment Company LLC` | With `--multi`, the host gives each run a private copy per `--game` folder (seeded from the real keys on first use), so a run never changes the user's options or another worktree's. |
| Build output | `A:\Games\star\builds\<track>` | Your own. |
| The game's one-copy check | a named mutex | Always run with `--multi`, or your run ends another agent's (or the user's) game. |
| The screen, mouse and keyboard | the user's | `--headless` only. Never run a window, or the original game fullscreen (it takes over the main monitor and mouse), without asking the user first. |
| CPU | everyone's | A full build of the lifted C is 30+ minutes on all cores: only a re-lifting track does one, and says so. |
| `pcrecomp` | `A:\Games\star\pcrecomp`, used by every build | Read-only. A track that needs pcrecomp changes (Linux, macOS) works in a pcrecomp worktree of its own and points `PCRECOMP` at it. |
| `ffmpeg` | on `PATH` | For `--record` and frame checks. |

## Git

- One branch per track, from `origin/main`: `track/<name>`.
- Small commits; rebase on `origin/main` before asking for a merge.
- Merges go to the fork's `main` in roadmap order (most feasible first);
  later tracks rebase over earlier ones.
- **Hot files:** `src/runtime/softdd.c` (tracks 1, 2, 3, 5, 6, 7),
  `src/runtime/host.c` (2, 3, 6, 7), `CMakeLists.txt` (4, 7). Put new
  subsystems in new files (a presenter, a movie player) and keep edits to
  the hot files small and local: no reformatting, no moving code around.
- `ROADMAP.md`: update your own track's section only.

## Checking a change

- Headless runs with `--keys` (scripted mouse and keys), `--shot`
  (screenshots) and `--watchdog` (a time limit). `docs/testing.md` has the
  scripted skirmish.
- Compare against the original only through screenshots the user has
  already taken, or ask the user to run the comparison: it needs the
  screen.
