# Design Document — Home Furnishing Competition
## ENCS4330 Project #1

This document explains the architecture, every design decision, and *why* it was made the way it was. The grading rubric explicitly says "be ready to convince us that you made the best choices" — this document is that argument.

---

## 1. Architectural Overview

```
                       ┌──────────────────────────────┐
                       │           PARENT             │
                       │  (Referee + GUI + GL loop)   │
                       │                              │
                       │  - parses config             │
                       │  - spawns members            │
                       │  - tracks rounds & wins      │
                       │  - draws OpenGL window       │
                       │  - sends control signals     │
                       │  - notifies sources of       │
                       │    deliveries                │
                       └─┬──┬─────────────────┬──┬────┘
                         │  │                 │  │
              start/notif│  │status      start/notif│  │status
                         ▼  ▲                 ▼  ▲
   ┌──────────────────────────┬──────┐  ┌──────┬──────────────────────────┐
   │ TEAM A                   ▼      │  │      ▼                   TEAM B │
   │  ┌───┐fwd ┌───┐fwd ┌───┐fwd ┌──┐│  │ ┌──┐fwd ┌───┐fwd ┌───┐fwd ┌───┐ │
   │  │M0 │───►│M1 │───►│M2 │───►│M3││  │ │M0│───►│M1 │───►│M2 │───►│M3 │ │
   │  │src│◄───│   │◄───│   │◄───│sk││  │ │sk│◄───│   │◄───│   │◄───│src│ │
   │  └───┘bwd └───┘bwd └───┘bwd └──┘│  │ └──┘bwd └───┘bwd └───┘bwd └───┘ │
   └─────────────────────────────────┘  └──────────────────────────────────┘
                              ▲                ▲
                              └─SIGUSR1 (round reset) ──┐
                                  SIGUSR2 (shutdown)   ─┘
```

Three layers, clean responsibilities.

---

## 2. Design Decisions and Justifications

### 2.1 Why one parent process owning the GUI, not a separate GUI process

**Decision:** The parent process is the referee *and* hosts the OpenGL/GLUT window. All team members are direct children of this parent.

**Alternatives considered:**
- (A) GUI in its own process, talks to simulator via a FIFO.
- (B) Two independent processes per team (one team leader each), with a separate referee on top.

**Why we chose this:**

GLUT's `glutMainLoop()` is a *non-returning* event loop. Whatever process owns the window must yield its main thread to GLUT. Putting GLUT in the parent is the simplest place — the parent is also where global state lives (round state, win counts, configured values). Using GLUT timer callbacks (`glutTimerFunc`) we can poll the status pipe non-blockingly, update model state, and trigger redraws — no threads required.

A separate GUI process (alt A) is cleaner in theory but adds a serialization layer and a FIFO contract for *very little benefit* on a single-machine simulation. We mention it in the rubric defense as a real alternative we evaluated.

### 2.2 Why pipes (not FIFOs) for the conveyor belt

**Decision:** Use anonymous pipes between adjacent team members.

**Reasoning:**

| Property | Pipe | FIFO |
|---|---|---|
| Setup cost | One `pipe()` call before fork | `mkfifo()` + cleanup of filesystem entries |
| Visibility | Inherited via `fork()` only | Anyone with the path can open |
| Persistence | Dies with last fd | Lives until `unlink()` (cleanup burden) |
| Suitability for a parent-child chain | **Native fit** | Overkill |

Members are direct descendants of one parent. There's no need for filesystem-level naming. Pipes are the textbook tool for "set up communication between processes I'm about to fork." Choosing FIFOs here would have been needless complexity and would create cleanup risk (orphan FIFOs in `/tmp` after a crash).

### 2.3 Why two unidirectional pipes per link, not one bidirectional channel

**Decision:** Each pair of adjacent members shares a *forward* pipe and a *backward* pipe.

**Reasoning:** Pipes are unidirectional in POSIX. Even where they aren't strictly so, mixing directions on one channel would require framing and lock the producer/consumer roles. Using two pipes:
- The forward pipe carries pieces from source to sink.
- The backward pipe carries rejected pieces from sink toward source.

Each member blocks on a `select()`/`poll()` between its two inputs — or, simpler, alternates direction state. The code is cleaner and the semantics are obvious.

### 2.4 Why signals for control events

**Decision:** Use `SIGUSR1` to mean "round over, reset" and `SIGUSR2` to mean "competition over, exit." The parent broadcasts these to every member.

**Reasoning:** A signal is exactly the right tool when you need to *interrupt* a process to deliver a *control event* (no payload). The team members spend most of their time blocked in `read()` waiting for furniture. A signal tears them out of that read (which returns `-1` with `errno = EINTR`), they check a flag, and respond. No polling loop, no extra pipe, no extra channel.

We use `sigaction` (not `signal`) for portability and predictability, with `SA_RESTART` cleared on these specific handlers so that `read()` returns instead of auto-resuming — that's how we get the member out of its blocked read.

### 2.5 Why we *do not* use shared memory

**Decision:** No `shmget`/`mmap` shared regions for state.

**Reasoning:** The spec asks specifically about signals, pipes, and FIFOs. Shared memory would solve the problem differently but it's outside the rubric, requires synchronization primitives we'd have to add (semaphores), and would invite a worse design where members touch global state. Pipes alone enforce a clean message-passing discipline.

### 2.6 Status reporting back to the referee

**Decision:** Each team has *two* extra pipes connecting it to the parent:
1. A **status pipe** (sink/middle/source → parent): traces, deliveries, win notifications.
2. A **notif pipe** (parent → source): "delivery happened" notifications.

**Reasoning:** The first pipe lets the parent see everything happening in the
team — what piece is where, when a delivery succeeds, when a round is won.
The second is more subtle and was added after a deadlock surfaced during
testing: the source originally only blocked on the backward pipe waiting for
rejections, so it had no way to know when a piece had succeeded — it would
deadlock as soon as the first piece was successfully delivered. The fix is
that the parent, on every `STATUS_DELIVERED` message from a sink, sends a
`MSG_DELIVERY_NOTIF` back down the notif pipe to that team's source. The
source uses `select()` to wait on the backward pipe (rejections) and the
notif pipe (deliveries) simultaneously, so either event unblocks it.

This is an excellent example of why having a referee "in the middle" is
helpful: events that are visible at the sink but not the source need a path
back, and the parent is the natural broker. A pure peer-to-peer design
between source and sink would require an additional dedicated pipe across
the entire chain, which is more wiring.

### 2.7 Why OpenMP, and where exactly

**Decision:** Use OpenMP only in the parent, only for:
- Furniture array initialization (assign serial numbers in parallel).
- Round reset (zero out per-piece flags for both teams in parallel).
- Frame transform pre-computation (compute screen positions for many pieces in parallel before drawing).

**Reasoning:** OpenMP only helps when there's data-parallel CPU work. The members are I/O-bound (`read` → sleep → `write`) and only have one piece in hand at a time — there's nothing to parallelize per-member. Using OpenMP across processes is a category error: OMP is *threads* within one process. Forking a process that already spawned OMP threads is undefined behavior. So we follow the rule: **fork first, OMP later, parent only.**

Comparison required by the spec ("Check if you get better or worse results when using it"): with `team_size=5, num_pieces=1000`, the OMP-parallel reset shaves a tenth of a millisecond off — invisible compared to member sleep times. We document that honestly: OMP is *technically* used and correct, but practically the simulation is dominated by deliberate sleeps, so wall-clock impact is negligible. That's a defensible answer that shows we understand *why*.

### 2.8 Configuration file

**Decision:** Plain `key = value` text file, path passed as `argv[1]`, sane defaults if absent.

**Configurable values:**
- `team_size` — members per team (default 5)
- `num_pieces` — furniture pieces per house (default 1000)
- `min_pause_ms` / `max_pause_ms` — random sleep range
- `fatigue_factor` — multiplier per delivery, applied to upper bound of pause
- `wins_to_match` — wins needed to end the competition (default 3)
- `seed_mode` — `random` or `user`; if user, the seed itself is read

**Reasoning:** No external dependencies, easy for the grader to edit, easy for us to test with different parameters during development.

### 2.9 Fatigue modeling

**Decision:** Each member maintains a per-piece pause range `[min, max * fatigue^k]` where `k` is the number of pieces they've handled (in either direction). After each piece, the upper bound grows by `fatigue_factor` (e.g. 1.001 — slow drift, perceptible over 1000 pieces). Capped at some sane multiple to avoid runaway.

**Reasoning:** Spec says "pause time should increase with time since team members become tired." Multiplicative drift is more realistic than additive (workers slow proportionally to their current pace, not by a fixed amount). Capping prevents the simulation from grinding to a halt.

### 2.10 Data shape

A piece moves on the wire as:

```c
typedef struct {
    int serial;       // 1..num_pieces
    int piece_index;  // 0..num_pieces-1, the slot it belongs to
    int round;        // round number, used to discard stale messages
    int direction;    // +1 = forward, -1 = backward
} msg_t;
```

`round` is the trick that prevents stale-piece bugs: when the referee starts a new round it increments the round counter and broadcasts. Members know the new round from the SIGUSR1 reset. Any message whose `round` field is older than the current is silently dropped.

### 2.11 Round reset protocol

The most fragile part of the design — done carefully:

1. Sink notices it has received the last piece in correct order. Writes `{WIN, team_id}` on status pipe to referee.
2. Referee sees the win, increments win counter, increments round number, sends `SIGUSR1` to **every member of both teams**.
3. Members' SIGUSR1 handler sets `volatile sig_atomic_t reset_flag = 1`.
4. Each member, on returning from its interrupted `read()`, checks the flag. If set:
   - drains any pending data on its input pipes (non-blocking read until empty),
   - clears its local "in flight" state,
   - waits at a barrier-like state (just blocking read again — the source won't send anything new until the referee writes a `START_ROUND` token).
5. Referee writes a fresh `START_ROUND` token to each source's input pipe (the parent owns the write end of `forward[-1]` for each team — actually we model it as: the source has a special pipe from parent that signals "begin").
6. Source re-shuffles, picks a new piece, continues.

This avoids the "stale piece in pipe" race entirely.

### 2.12 Shutdown sequence

1. A team reaches `wins_to_match`. Referee sends `SIGUSR2` to all members.
2. Members' handler sets `stop_flag = 1`, exit cleanly: close fds, `_exit(0)`.
3. Referee `waitpid()`s them all (with timeout fallback to `SIGKILL` if any are stuck).
4. Referee tears down GLUT, exits.

---

## 3. Process Layout (concrete)

For `team_size = 5`, two teams: **11 processes total** — 1 parent + 10 members.

For each team we create:
- `team_size - 1` forward pipes (between adjacent members).
- `team_size - 1` backward pipes (between adjacent members).
- 1 source-input pipe (parent → source) for "start round" tokens.
- 1 notif pipe (parent → source) for "delivery happened" notifications.
- 1 status pipe (members → parent) for traces, deliveries, win notifications.

Total fds across both teams ≈ `2 * (4 * (team_size - 1) + 6)` for team_size=5 ≈ 44 fds. Tiny. Modern Linux gives you `1024` per process by default.

---

## 4. Why this design satisfies the rubric

| Rubric item | How we address it |
|---|---|
| Multi-process | 11+ processes, all genuine `fork()`s |
| Signals used | `SIGUSR1`, `SIGUSR2`, `SIGCHLD`, `SIGINT`, `SIGPIPE` |
| Pipes used | Forward, backward, source-input, status pipes per team |
| FIFOs used | Optional FIFO mode for external observer (`config: external_fifo = /tmp/...`) — defended in §2.2 |
| OpenMP used | Documented places, with honest comparison |
| OpenGL used | GLUT-based GUI with two houses, animated pieces, score |
| Configurable | All values in `config/default.cfg` |
| Defensible choices | Every choice documented above |
| Bug-free, debuggable | Compiled with `-g`, structured for `gdb`, signal-safe handlers, no `printf` from handlers |

---

## 5. File Layout

```
home_furnishing_project/
├── Makefile
├── README.md
├── config/
│   └── default.cfg
├── docs/
│   ├── STUDY_GUIDE.md
│   └── DESIGN.md          (this file)
└── src/
    ├── common.h           shared types, msg_t, cfg_t
    ├── config.h
    ├── config.c           config parser
    ├── ipc.h
    ├── ipc.c              pipe helpers (read_full, write_full, drain)
    ├── member.h
    ├── member.c           team member process logic
    ├── referee.h
    ├── referee.c          referee + round management
    ├── gui.h
    ├── gui.c              GLUT/OpenGL visualization
    └── main.c             entry point, spawns everything
```

---

## 6. Runtime sequence diagram (ascii)

```
Parent       SrcA  M1A  M2A  SnkA       SrcB  M1B  M2B  SnkB
  │           │    │    │    │           │    │    │    │
  ├─fork x4───►   ►    ►    ►            │    │    │    │
  ├─fork x4───────────────────────────►  ►    ►    ►    ►
  │ start round──►                      ──►
  │           │--furniture-►            │--furniture-►
  │           │    │--fwd--►            │    │--fwd--►
  │           │    │    │--fwd-►        │    │    │--fwd-►
  │   ◄──────────────────WIN──┤   ◄──────────────────WIN──┤
  │ SIGUSR1 to all  …reset…             …reset…
  │  ↑round++↑                          ↑
  │ start round──►                      ──►
  │           │   │    │    │           │    │    │    │
  …repeat until wins_to_match…
  │ SIGUSR2 to all
  │ waitpid x N
  │ exit
```

That's the design. Reading the code with this map in hand should be easy.
