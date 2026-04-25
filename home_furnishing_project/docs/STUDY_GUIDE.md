# Study Guide — ENCS4330 Project #1
# Signals, Pipes & FIFOs under Linux
## Home Furnishing Competition

This guide takes you from zero to having every concept you need to implement the project. Read it top-to-bottom once, then keep it open as a reference while you code.

---

## Table of Contents

1. [Big Picture: What the Project Actually Asks](#1-big-picture)
2. [Linux Processes — The Foundation](#2-linux-processes)
3. [The `fork()` / `exec()` / `wait()` Family](#3-fork-exec-wait)
4. [Signals — Asynchronous Notifications](#4-signals)
5. [Pipes — Anonymous IPC](#5-pipes)
6. [FIFOs (Named Pipes)](#6-fifos)
7. [Choosing Between Pipes, FIFOs, and Signals](#7-choosing-ipc)
8. [Random Numbers in Multi-Process Programs](#8-random-numbers)
9. [OpenMP — Shared-Memory Parallelism](#9-openmp)
10. [OpenGL Basics for Visualization (GLUT)](#10-opengl-glut)
11. [Configuration File Parsing in C](#11-config-files)
12. [Debugging with GDB (Multi-Process)](#12-gdb-multiprocess)
13. [Build Systems — `Makefile` Essentials](#13-makefiles)
14. [Common Bugs and How to Spot Them](#14-common-bugs)
15. [Recommended Reading Order Before You Code](#15-reading-order)

---

## 1. Big Picture
### What the project asks

You're simulating two competing teams. Each team is a **chain of processes** (members), and each chain has a "source" (member 0, next to the furniture pile) and a "sink" (last member, inside the house). Furniture pieces have unique serial numbers. The source picks one at random and shoves it down the chain. If it arrives at the sink in the *correct* serial order, it stays. Otherwise it bounces back to the source, who must try a different piece next time. First team to deliver every piece in correct order wins a round. First to win N rounds wins the match.

The IPC requirements are explicit: **signals, pipes, and/or FIFOs**. You do *not* have to use all three — you have to *justify* the ones you pick. OpenMP and OpenGL are also asked for.

### The mental model that makes this easy

Think of the system as having three layers:

1. **Coordination layer (parent / referee process)** — spawns everything, watches for round completion, declares winners, restarts rounds, draws the GUI.
2. **Worker layer (team members)** — many small processes that just sit in a loop: read from previous neighbor, sleep, write to next neighbor.
3. **Communication channels** — pipes between adjacent members (the natural choice — it's literally a "conveyor belt"), and signals from the referee to broadcast events like "round over, reset."

Once you see it that way the design almost writes itself.

---

## 2. Linux Processes
### What you need to know

A **process** is an instance of a running program. Each has:
- A **PID** (process ID) — a unique integer.
- A **PPID** (parent PID) — who created it.
- Its own virtual address space — memory is *not* shared by default.
- File descriptors — small integers that index open files/pipes/sockets.

Key consequence: when you `fork()`, the child gets its own copy of memory. Variables you set in the parent are not magically visible to the child after the fork — they were copied at the moment of the fork and then drift independently. This is why we need IPC at all.

### Useful headers

```c
#include <sys/types.h>   // pid_t
#include <unistd.h>      // fork, pipe, read, write, close, sleep, getpid, getppid, execvp
#include <sys/wait.h>    // wait, waitpid
#include <signal.h>      // signal, sigaction, kill, raise
#include <stdlib.h>      // exit, EXIT_SUCCESS, srand, rand
#include <stdio.h>       // printf, perror
#include <string.h>      // strerror, memset
#include <errno.h>       // errno
#include <sys/stat.h>    // mkfifo
#include <fcntl.h>       // open, O_RDONLY, O_WRONLY, O_NONBLOCK
```

---

## 3. `fork()`, `exec()`, `wait()`
### `fork()`

```c
pid_t pid = fork();
if (pid < 0)      { perror("fork"); exit(1); }
else if (pid == 0) { /* child code */ }
else               { /* parent code, pid is child's PID */ }
```

Returns **twice**. Once in the parent (with the child's PID), once in the child (with 0). Memory is duplicated copy-on-write. Open file descriptors are inherited.

### `exec*()`

Replaces the current process image with a new program. Used after `fork()` if you want the child to run a *different* program. For this project you can keep all code in one binary with a role-dispatching `main()`, so you may not need `exec` at all — but you should know it exists.

### `wait()` / `waitpid()`

The parent should reap children to avoid zombies:

```c
int status;
pid_t finished = waitpid(-1, &status, WNOHANG); // -1 = any child, WNOHANG = don't block
```

`WNOHANG` is critical for this project — the referee can't block in `wait()` because it also has to redraw the GUI and listen for signals.

### Zombie vs orphan

- **Zombie**: child has exited but parent hasn't called `wait()`. Still has an entry in the process table.
- **Orphan**: parent died first; child is reparented to `init` (PID 1).

You don't want either. Always reap, and on shutdown, kill children explicitly.

---

## 4. Signals
### Concept

A signal is an asynchronous notification delivered by the kernel to a process. It interrupts whatever the process is doing and runs a **signal handler** (a function you registered) — or takes a default action (often: terminate).

### Signals you'll actually use

| Signal | Number | Default | What it's good for here |
|---|---|---|---|
| `SIGUSR1` | 10 | terminate | Custom event — e.g., "round won, reset" |
| `SIGUSR2` | 12 | terminate | Custom event — e.g., "competition over, exit" |
| `SIGTERM` | 15 | terminate | "Please exit cleanly" |
| `SIGINT` | 2 | terminate | Ctrl-C |
| `SIGCHLD` | 17 | ignore | Sent to parent when a child changes state |
| `SIGKILL` / `SIGSTOP` | 9 / 19 | — | Cannot be caught — last-resort cleanup |

### Registering a handler — use `sigaction`, not `signal`

`signal()` has historical portability quirks. `sigaction()` is the modern, well-defined API:

```c
void on_sigusr1(int sig) {
    /* MUST be async-signal-safe — see below */
}

struct sigaction sa = {0};
sa.sa_handler = on_sigusr1;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;   // auto-restart interrupted system calls
sigaction(SIGUSR1, &sa, NULL);
```

### The async-signal-safety trap

A signal handler interrupts your code at an arbitrary point. If the handler calls a function that wasn't designed to be re-entered (like `printf`, `malloc`, anything in stdio), you can deadlock or corrupt state. The safe pattern is:

> **In the handler, set a `volatile sig_atomic_t` flag. Do real work in the main loop.**

```c
volatile sig_atomic_t round_over = 0;
void on_round_over(int s) { round_over = 1; }

/* main loop: */
while (!stop) {
    if (round_over) { round_over = 0; reset_round(); }
    /* ... */
}
```

### Sending signals

```c
kill(child_pid, SIGUSR1);  // "kill" is named misleadingly; it just sends a signal
```

The parent always knows its children's PIDs (return value of `fork`), so this is trivial.

### Blocking signals during critical sections

```c
sigset_t set, oldset;
sigemptyset(&set);
sigaddset(&set, SIGUSR1);
sigprocmask(SIG_BLOCK, &set, &oldset);
/* critical section */
sigprocmask(SIG_SETMASK, &oldset, NULL);
```

---

## 5. Pipes
### Concept

A **pipe** is a kernel-managed in-memory FIFO buffer with two ends:
- A **read end** — file descriptor for reading.
- A **write end** — file descriptor for writing.

```c
int fd[2];
pipe(fd);   // fd[0] is read end, fd[1] is write end
```

After `fork()`, *both* parent and child have copies of both ends. **You must close the ends you don't use**, otherwise readers will never see EOF and writers will never get `SIGPIPE`/error.

### The canonical "chain of processes" pattern

For a chain `M0 → M1 → M2 → ... → Mn-1` you create `n-1` pipes — one between each adjacent pair — and a return path of another `n-1` pipes for the bounce-back.

```
forward[i]: written by M[i],   read by M[i+1]
backward[i]: written by M[i+1], read by M[i]
```

After fork, member `i` keeps:
- `forward[i-1]` read end (input from previous), if `i > 0`
- `forward[i]` write end (output to next), if `i < n-1`
- `backward[i-1]` write end (rejection back to previous), if `i > 0`
- `backward[i]` read end (rejection from next), if `i < n-1`

Everything else is `close()`d in that process.

### Atomicity

`write()` of `<= PIPE_BUF` (≥ 4096 bytes on Linux) is atomic — it won't be interleaved with another writer. We're sending small structs (one furniture piece at a time), so we're safely in that range.

### Reading: blocking is your friend here

By default `read()` on a pipe blocks until data arrives. That's exactly the "wait for the previous member to hand me a piece" semantic we want. No busy-loop, no polling, just `read()`.

### EOF

When all writers of a pipe close their write end, the read end returns `0` from `read()`. Use this for clean shutdown.

### `SIGPIPE`

If you write to a pipe whose read end is closed, you get `SIGPIPE`, which by default kills your process. Either ignore it (`signal(SIGPIPE, SIG_IGN)`) and check `write()`'s return value, or carefully manage shutdown order.

---

## 6. FIFOs (Named Pipes)
### Concept

A FIFO is like a pipe but it has a name in the filesystem. Unrelated processes can open it. Created with `mkfifo()`:

```c
mkfifo("/tmp/myfifo", 0666);
int fd = open("/tmp/myfifo", O_WRONLY);   // blocks until reader opens
```

### When to use FIFO vs pipe

- **Pipe**: parent and child (or siblings via shared parent). Not visible in the filesystem. **This matches our team-member chain perfectly.**
- **FIFO**: when processes are unrelated, or you need a name to coordinate. Useful here if you want a standalone GUI process talking to the simulator without having forked it directly.

For this project the **pipe is the better default for the member chain**. If you split the GUI off as its own program (clean separation), a FIFO between simulator and GUI becomes attractive.

---

## 7. Choosing IPC
### Decision matrix for this project

| Need | Best tool | Why |
|---|---|---|
| Pass furniture between adjacent members | **Pipe** | Natural conveyor-belt semantics, related processes, blocking read = automatic synchronization. |
| Bounce a rejected piece backwards | **Pipe (separate)** | Same reasoning, opposite direction. Two unidirectional pipes are clearer than one with framing. |
| Tell all members "round over, drop everything and reset" | **Signal (SIGUSR1)** | Asynchronous broadcast event — exactly what signals are for. Doesn't need data. |
| Tell everyone "competition over, exit" | **Signal (SIGUSR2 or SIGTERM)** | Same. |
| Decouple GUI from simulator (optional) | **FIFO** | Lets you `kill -USR1` the simulator from a script, tail a FIFO for visualization, etc. |
| Share win counters / stats with referee | Pipe back to referee | Sink writes "delivered" reports up to the referee on a dedicated pipe. |

So: **pipes for data flow, signals for control events**. FIFOs only if you want a clean GUI/simulator split. Be ready to defend that choice — the rubric specifically wants you to.

---

## 8. Random Numbers in Multi-Process Programs
### The classic bug

Every process inherits the parent's RNG state. If you fork 10 children and they all call `rand()` without re-seeding, they produce **identical** sequences. Always re-seed in each child:

```c
srand(time(NULL) ^ (getpid() << 16));
```

Mixing PID into the seed prevents two children spawned in the same second from getting identical seeds.

### `rand()` vs `rand_r()` vs `random()`

- `rand()` is fine in a single-threaded process.
- With OpenMP, threads share the RNG state and contend for it. Use a per-thread seed and `rand_r(&seed)` (note: `rand_r` is technically deprecated in POSIX 2008 but still universally available; alternatives include `drand48_r` or just maintaining your own LCG).

---

## 9. OpenMP
### The 30-second introduction

OpenMP is a pragma-based shared-memory parallelism library. You annotate loops, the compiler spawns threads. **It does *not* parallelize across processes** — only across threads within one process.

Compile with `-fopenmp`. Include `<omp.h>` if you call functions like `omp_get_thread_num()`.

### Where it actually helps in this project

Honestly? Not in many places. Most of the work is `read()` → sleep → `write()`, which is I/O-bound and cannot be parallelized within a single member.

But there *are* spots:

- **Initial furniture array setup** (assigning unique serial numbers, shuffling) — embarrassingly parallel.
- **Round reset** (resetting per-piece state for both teams) — parallel.
- **GUI rendering** of many furniture sprites — can be parallel for transforms but OpenGL calls themselves must be on the GL thread, so OMP won't help much here.

Example of the shuffle:

```c
#pragma omp parallel for
for (int i = 0; i < N; i++) furniture[i].serial = i;

/* shuffle on a single thread (Fisher-Yates is sequential) */
for (int i = N-1; i > 0; i--) {
    int j = rand() % (i+1);
    swap(&furniture[i], &furniture[j]);
}
```

### Critical gotcha

Forking a process that has already started OpenMP threads is **undefined behavior**. Always `fork()` first, then use OpenMP inside whichever process needs it. The natural fit here is to use OpenMP only inside the parent/referee, after all `fork()`s are done.

---

## 10. OpenGL + GLUT
### Why GLUT specifically

The project says "simple and elegant elements are enough." `freeglut` (or `glut`) gives you a window, an event loop, and primitive drawing in ~30 lines. No shader programs, no buffers, no GLSL. Perfect for "draw two house outlines and animated furniture rectangles."

### The minimum you need to know

```c
#include <GL/freeglut.h>

void display(void) {
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1.0, 0.0, 0.0);          // red
    glBegin(GL_QUADS);
        glVertex2f(-0.5, -0.5);
        glVertex2f( 0.5, -0.5);
        glVertex2f( 0.5,  0.5);
        glVertex2f(-0.5,  0.5);
    glEnd();
    glutSwapBuffers();
}

void timer(int v) {
    /* read latest state, update positions */
    glutPostRedisplay();
    glutTimerFunc(33, timer, 0);   // ~30 FPS
}

int main(int argc, char** argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(1000, 600);
    glutCreateWindow("Home Furnishing Competition");
    glClearColor(0.95, 0.95, 0.95, 1.0);
    glutDisplayFunc(display);
    glutTimerFunc(0, timer, 0);
    glutMainLoop();   // never returns
}
```

### Coordinate system

By default OpenGL's clip space is `[-1, +1]` in both X and Y. Either work in that space (simple) or set up an orthographic projection that uses pixel coordinates:

```c
glMatrixMode(GL_PROJECTION);
glLoadIdentity();
gluOrtho2D(0, win_w, 0, win_h);   // (0,0) bottom-left, (w,h) top-right
glMatrixMode(GL_MODELVIEW);
```

### Drawing text

```c
glRasterPos2f(x, y);
glutBitmapString(GLUT_BITMAP_HELVETICA_18, (unsigned char*)"Team A: 3 wins");
```

### Architecture: GL is in the parent only

`glutMainLoop()` never returns and runs on the calling thread. So:
- Either the parent process does *all* GL work (good — clean) and spawns all members, communicating via pipes.
- Or you split GUI into its own process and feed it state through a FIFO.

For simplicity and the project's "be wise in your choices" hint, do the first. The parent reads status updates from members on a status pipe, updates an in-memory model of where every piece is, and the GLUT timer redraws from that.

### Linking

```
-lGL -lGLU -lglut
```

On Ubuntu install: `sudo apt install freeglut3-dev`.

---

## 11. Configuration Files
### Format choice

A simple `key = value` text format with `#` comments is enough. No need for JSON/YAML libraries.

```
# config.txt
team_size      = 5
num_pieces     = 100
min_pause_ms   = 100
max_pause_ms   = 500
fatigue_factor = 1.05
wins_to_match  = 3
```

### Parser sketch

```c
FILE* f = fopen(path, "r");
char line[256], key[64], val[128];
while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, " %63[^= \t] = %127s", key, val) == 2) {
        if      (!strcmp(key,"team_size"))    cfg.team_size  = atoi(val);
        else if (!strcmp(key,"num_pieces"))   cfg.num_pieces = atoi(val);
        /* ... */
    }
}
```

### Pass the path as `argv[1]`

```c
const char* path = (argc >= 2) ? argv[1] : "config/default.cfg";
```

Default to a known file so it runs out of the box.

---

## 12. GDB Multi-Process
### Compile with debug info

```
gcc -g -O0 -Wall -Wextra ...
```

### Following children

By default `gdb` follows the parent on `fork`. To follow the child:

```
(gdb) set follow-fork-mode child
(gdb) set detach-on-fork off    # debug both
(gdb) info inferiors
(gdb) inferior 2                # switch to second one
```

### Attaching to a running process

```
gdb -p <pid>
```

Useful when one of your member processes hangs.

### Useful commands

| Command | Effect |
|---|---|
| `bt` | backtrace |
| `frame N` | jump to frame N |
| `print var` | inspect |
| `info threads` | list threads (relevant when OMP active) |
| `handle SIGUSR1 nostop pass` | don't stop on this signal, just deliver it |

That last one matters: by default GDB stops on every signal, which makes signal-driven code hellish to debug.

---

## 13. Makefiles
### Minimum viable Makefile

```make
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -fopenmp
LDFLAGS = -lGL -lGLU -lglut -lm -fopenmp

SRC = src/main.c src/config.c src/member.c src/referee.c src/gui.c
OBJ = $(SRC:.c=.o)
BIN = build/furnishing

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p build
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
```

Run with `make`, run binary with `./build/furnishing config/default.cfg`.

---

## 14. Common Bugs
### A non-exhaustive list ranked by how often I'd bet you'll hit them

1. **Forgot to close unused pipe ends** → reads block forever, writes never get `EPIPE` on shutdown.
2. **All children produce the same random numbers** → forgot to re-seed after `fork()`.
3. **Used `printf` inside a signal handler** → occasional weird hangs / corruption.
4. **`SIGCHLD` not reaped** → zombies pile up; eventually `fork()` fails.
5. **Race on round reset** — referee starts new round while a stale piece is still in flight in a pipe; sink reads it next round and gets confused. Fix: drain pipes on reset, OR include a round number in every message and discard stale messages.
6. **Writing more than one struct in a write but readers expect one struct per read** → `read()` may return fewer bytes than asked; loop until you get the full struct.
7. **GUI process forked from a process that already initialized GL** → undefined. Init GL only after all forks.
8. **OMP + fork combined wrong** → only use OMP in parent, after forks.
9. **`SIGPIPE` kills your simulator on shutdown** → ignore it, check write return.
10. **Mixed up `fd[0]` (read) and `fd[1]` (write)** — universally the most common pipe bug. Wrap them in named structs to make this impossible.

### `read` doesn't always return what you asked for

On pipes, small writes (< PIPE_BUF) are atomic, so a single `write(fd, &msg, sizeof msg)` followed by a single `read(fd, &msg, sizeof msg)` *usually* delivers the whole struct. But to be robust write a helper:

```c
ssize_t read_full(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r == 0) return got;          // EOF
        if (r < 0) {
            if (errno == EINTR) continue; // signal interrupted us, retry
            return -1;
        }
        got += r;
    }
    return got;
}
```

---

## 15. Reading Order Before You Code

If you have, say, six hours to prep, spend it like this:

1. **Hour 1** — `fork`, `wait`, basic process model. Write a 30-line program: parent forks 3 children, each prints its PID, parent reaps them all.
2. **Hour 2** — Pipes. Write the conveyor belt: a parent and 4 children connected by pipes, one integer travelling from child 0 to child 3 and back.
3. **Hour 3** — Signals with `sigaction`. Write a program that catches `SIGUSR1` and toggles a flag printed by the main loop.
4. **Hour 4** — GLUT hello world. A window, a moving rectangle on a timer.
5. **Hour 5** — Read the project spec again, sketch the design (next document), pick which IPC primitive does what.
6. **Hour 6** — Configuration file parsing + Makefile + skeleton of `main()` that dispatches to "referee" or "member" based on a role argument.

After this, you implement.

---

## Quick Cheat-Sheet

```c
/* fork */
pid_t p = fork();

/* pipe */
int fd[2]; pipe(fd);  /* fd[0]=read, fd[1]=write */

/* signal — modern way */
struct sigaction sa = { .sa_handler = handler, .sa_flags = SA_RESTART };
sigemptyset(&sa.sa_mask);
sigaction(SIGUSR1, &sa, NULL);

/* send signal */
kill(pid, SIGUSR1);

/* fifo */
mkfifo("/tmp/x", 0666);
int fd = open("/tmp/x", O_WRONLY);

/* reap children non-blockingly */
while (waitpid(-1, NULL, WNOHANG) > 0) ;

/* re-seed RNG in child */
srand(time(NULL) ^ (getpid() << 16));

/* OpenMP */
#pragma omp parallel for
for (int i = 0; i < N; i++) ...

/* GLUT main */
glutInit(&argc, argv);
glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
glutCreateWindow("...");
glutDisplayFunc(display);
glutTimerFunc(0, tick, 0);
glutMainLoop();
```

That's everything. Now turn to the design document.
