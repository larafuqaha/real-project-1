# Home Furnishing Competition
### ENCS4330 - Real-Time Applications and Embedded Systems
### Birzeit University - Faculty of Engineering and Technology
### Electrical and Computer Engineering Department
### Second Semester 2025/2026

---

## Authors
  Tawba Abdallah 1221002
  Taymaa Nasser 1222640
  Lara Fuqaha 1220071
  Lara Abu Asfour 1221484

---

## Project Overview

This project implements a multi-process simulation of a home furnishing competition between two teams under Linux. The simulation uses signals, pipes, and FIFOs for inter-process communication, OpenMP for parallelism, and OpenGL/GLUT for visualization.

Two teams race to furnish their respective houses by moving furniture pieces from a source member through a chain of team members to a sink member. The team that completes the most rounds wins the match.

---

## Simulation Rules

### Teams and Members

Each team has a user-defined number of members numbered from 0 to N-1. Member 0 is the source, stationed next to the furniture pile. Member N-1 is the sink, stationed inside the house. All members in between form a relay chain, passing furniture pieces one by one from source to sink.

### Furniture

The furniture consists of a user-defined number of pieces, each assigned a unique serial number either randomly by the system or provided by the user. Each piece comes in a pair, one for each team. Team members are unaware of the serial numbers assigned to the pieces, so they cannot pre-sort them.

### Movement and Order Checking

The source randomly selects a piece and passes it forward through the chain. When a piece reaches the sink, the sink checks whether it arrives in the correct serial order. If it is the next expected piece in sequence, it is accepted into the house. If it is out of order, it is rejected and sent back through the chain in reverse until it returns to the source. The source must not re-select a rejected piece until a different piece has been successfully delivered.

### Fatigue

Each time a team member handles a piece, their pause time increases multiplicatively by a configurable fatigue factor, simulating tiredness. The pause time is capped at a user-defined maximum to prevent the simulation from stalling.

### Rounds and Match

A round ends when one team delivers all pieces in the correct order. That team wins the round. All furniture is then reset with new serial numbers and a new round begins. The match ends when any team accumulates a user-defined number of round wins.

---

## Architecture

### Processes

Every team member runs as an independent forked process. There are 2 * team_size child processes in total. The parent process acts as the referee, orchestrating rounds and tracking state.

### Inter-Process Communication

Communication between processes is done entirely through anonymous pipes. There are five categories of pipes per team:

- Forward pipes carry furniture pieces from source toward the sink.
- Backward pipes carry rejected pieces from the sink back toward the source.
- A start pipe carries round-start tokens from the referee to the source.
- A notification pipe carries delivery and rejection notifications from the referee to the source, enabling the 2-in-flight pipelining optimization.
- A status pipe carries delivery reports, win announcements, and trace messages from members back to the referee.

Signals are used for round resets and shutdown. SIGUSR1 signals a round reset to all members. SIGUSR2 and SIGTERM signal a graceful shutdown. SIGALRM is used internally by each member to implement the fatigue-aware pause using sigsuspend, which avoids the race condition present in a naive pause-based approach.

### 2-in-Flight Pipelining

The source supports having two pieces in transit simultaneously: one moving forward through the chain and one returning backward after rejection. When the sink rejects a piece, it immediately notifies the referee via the status pipe. The referee relays a rejection notification to the source via the notification pipe, freeing the source to send the next piece without waiting for the rejected one to physically return. This significantly improves throughput.

### OpenMP

OpenMP is used in two places. In referee_start_round(), the per-piece state arrays are reset in parallel using a collapse(2) pragma across teams and pieces. In the source member, the serial number array initialization is parallelized with a pragma omp parallel for. The impact of OpenMP on performance is measurable when the number of pieces is large.

---

## Modules

### common.h

Defines all shared constants, message structs, status structs, and the configuration struct used across every module. Acts as the single source of truth for data types.

### config.c / config.h

Loads and validates the runtime configuration from a plain text key-value file. Applies safe defaults when a file is missing or a key is unrecognized. Validates all fields and returns an error if any value is out of range.

### ipc.c / ipc.h

Provides low-level pipe utilities: read_full and write_full guarantee atomic complete reads and writes, drain_pipe_nonblock flushes stale data from a pipe during round resets, and set_nonblock / set_block toggle blocking mode on file descriptors.

### member.c / member.h

Contains the logic for all three member roles. The source waits for a start token, manages piece selection and rejection tracking, and drives the round. Middle members relay pieces in both directions with a fatigue pause. The sink performs the serial-order check and either accepts or rejects each incoming piece.

### referee.c / referee.h

The parent-side orchestrator. Sets up all pipes and forks all member processes. Exposes referee_tick(), which is called periodically to drain status messages, update visualization state, send notifications to the source, detect round wins, and manage match progression. Also handles graceful shutdown and process reaping.

### gui.c / gui.h

Implements the OpenGL/GLUT visualization. Draws a real-time view of both team lanes including the furniture pile, house fill bar, member circles, in-flight pieces colored by serial number, a top scoreboard with round and timer information, and a winner banner. Falls back to headless mode automatically if OpenGL is unavailable at compile time.

### main.c

Entry point. Loads the config, installs parent signal handlers, calls referee_setup(), and hands off to either gui_run() or headless_run() depending on the configuration.

---

## Configuration

All simulation parameters are loaded from a plain text file passed as a command-line argument. No values are hardcoded. The following keys are supported:

| Key | Description | Default |
|---|---|---|
| team_size | Number of members per team (2 to 32) | 4 |
| num_pieces | Number of furniture pieces per round | 1000 |
| min_pause_ms | Minimum member pause in milliseconds | 50 |
| max_pause_ms | Maximum member pause before fatigue | 250 |
| fatigue_factor | Multiplicative fatigue growth per piece handled | 1.0005 |
| fatigue_cap_ms | Maximum pause ceiling in milliseconds | 2000 |
| wins_to_match | Number of round wins needed to end the match | 2 |
| seed_mode | random or user | random |
| user_seed | Seed value used when seed_mode is user | 42 |
| gui_enabled | 1 to enable OpenGL window, 0 for headless | 1 |
| verbose | 1 to enable referee log output | 0 |

---

## Testing

The project includes a four-layer test suite.

### Unit Tests

`test_ipc.c` tests the pipe utility functions in isolation: write/read roundtrip correctness, EOF handling, pipe draining, and message ordering.

`test_config.c` tests the configuration loader: correct parsing of known files, safe handling of missing files, field validation, and deterministic repeated loads.

`test_member.c` tests member behavior in two tiers. Lightweight tests verify context field initialization for source, sink, and middle roles, message struct integrity, and direction field correctness. Fork-based tests verify that a middle process correctly relays a piece forward, that the sink reports STATUS_DELIVERED for an in-order piece, that the sink rejects and returns an out-of-order piece, and that a member exits cleanly on EOF.

`test_referee.c` tests the referee in two tiers. Lightweight tests verify world_t initialization, config copying, wins counter initialization, piece position initialization, status message struct correctness, message kind uniqueness, and wins threshold logic. Fork-based tests verify that referee_setup forks the correct number of children, that status pipes are non-blocking after setup, that the referee runs multiple ticks without crashing, that referee_tick is safe after shutdown, and that both teams have independent pipe file descriptors.

### Scenario Tests

`test_scenarios.sh` runs the compiled binary against several configuration files and checks output for expected strings. Scenarios cover small configs, minimum team size, large teams, single piece per round, multi-round matches, 2-in-flight pipeline observation, and absence of core dumps.

### Stress Tests

`test_stress.sh` runs the binary 20 times back-to-back and reports the number of successful completions, timeouts (indicating possible deadlocks), crashes, and core dumps.

---

## Dependencies

- GCC with OpenMP support (fopenmp)
- freeglut3-dev for OpenGL/GLUT visualization (optional, falls back to headless)
- GDB for debugging (optional, binary compiled with -g by default)
