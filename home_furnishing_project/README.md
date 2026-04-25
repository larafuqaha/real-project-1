# Home Furnishing Competition
## ENCS4330 Real-Time Applications & Embedded Systems — Project #1

A multi-process Linux simulation of two teams competing to furnish their houses.
Uses **signals**, **pipes**, **OpenMP** and **OpenGL/GLUT**.

## Quick start

### 1. Install dependencies (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install build-essential freeglut3-dev libomp-dev
```

### 2. Build

```bash
make            # full build with OpenGL GUI
# OR
make headless   # no OpenGL link (servers / CI)
# OR
make debug      # -O0 -g3 for gdb sessions
```

### 3. Run

```bash
./build/furnishing config/default.cfg
```

Press **Q** or **Esc** to exit early. Configuration values can be edited in
`config/default.cfg` without recompiling.

## What you'll see

Two horizontal lanes, one per team. Each lane shows:
- A "pile" rectangle on the far left (where the source member stands).
- A row of circles for the team members (source = blue, sink = orange, middle = green).
- Animated red squares appearing near each member as a piece passes through.
- A house outline on the right with a green fill bar showing round progress.
- A scoreboard in the middle: round number, wins for each team.

When a team reaches the configured `wins_to_match`, the winner is announced
and the program exits a few seconds later.

## Project layout

```
home_furnishing_project/
├── Makefile                  # build with/without GL, debug variants
├── README.md                 # this file
├── config/
│   └── default.cfg           # all runtime parameters
├── docs/
│   ├── STUDY_GUIDE.md        # everything you need to learn for this project
│   └── DESIGN.md             # architecture and decision log
└── src/
    ├── common.h              # shared types (msg_t, cfg_t, etc.)
    ├── config.[ch]           # config-file parser
    ├── ipc.[ch]              # safe read/write/drain helpers
    ├── member.[ch]           # team-member process logic
    ├── referee.[ch]          # parent process: spawns, manages rounds
    ├── gui.[ch]              # OpenGL/GLUT visualization
    └── main.c                # entry point
```

Read `docs/STUDY_GUIDE.md` first if you want the background. Read
`docs/DESIGN.md` to understand *why* every choice was made.

## Debugging

```bash
make debug
gdb --args ./build/furnishing config/default.cfg
(gdb) handle SIGUSR1 nostop pass
(gdb) handle SIGUSR2 nostop pass
(gdb) handle SIGCHLD nostop pass
(gdb) set follow-fork-mode child   # to step into a member
(gdb) run
```

For diagnosing pipe leaks, with the binary running:
```bash
ls -l /proc/$(pgrep -f furnishing | head -1)/fd
```

## Notes for the grader

- **IPC choices**: pipes for the conveyor belt (data), signals (`SIGUSR1`/`SIGUSR2`)
  for control events (round reset / shutdown). Justification in `docs/DESIGN.md`.
- **OpenMP**: applied to round-reset loops in the parent. Honest measurement
  shows the simulation is dominated by deliberate `nanosleep`s, so wall-clock
  speedup is minimal — documented as such in the design doc.
- **OpenGL**: simple `freeglut` immediate-mode rendering. Two lanes, two
  houses, animated squares for in-flight pieces, scoreboard. No shaders.
- **Configurable**: every value the spec lists as user-defined lives in
  `config/default.cfg`. Pass any other path as `argv[1]`.
- **Bug-resistance**: `sigaction` (not `signal`); async-signal-safe handlers
  set flags only; `read_full`/`write_full` retry on `EINTR`; pipe ends closed
  in unused processes; round-number stamping discards stale messages on reset.
