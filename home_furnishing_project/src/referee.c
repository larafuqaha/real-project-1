/*
 * referee.c — parent-side orchestration.
 *
 *   - sets up all pipes, forks all members, fills world_t
 *   - exposes referee_tick() called from GLUT timer (or main loop in headless)
 *   - sends SIGUSR1 (round reset) and SIGUSR2 (shutdown)
 */
#include "referee.h"
#include "ipc.h"
#include "member.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* Per-team pipe layout, allocated dynamically because team_size is runtime. */
typedef struct {
    int (*fwd)[2];          /* fwd[i][0]=read, fwd[i][1]=write, between member i and i+1 */
    int (*bwd)[2];          /* bwd[i][0]=read, bwd[i][1]=write, between i+1 (writer) and i (reader) */
    int   start[2];         /* parent writes to start[1], source reads from start[0] */
    int   notif[2];         /* parent writes delivery notifications to notif[1], source reads notif[0] */
    int   status[2];        /* status[1] is written by all members, status[0] is read by parent */
} team_pipes_t;

static team_pipes_t g_pipes[NUM_TEAMS];

/* === setup helpers === */

static int alloc_team_pipes(team_pipes_t* p, int team_size) {
    int N = team_size - 1;     /* number of forward (and backward) links */
    p->fwd = calloc(N, sizeof(*p->fwd));
    p->bwd = calloc(N, sizeof(*p->bwd));
    if (!p->fwd || !p->bwd) return -1;

    for (int i = 0; i < N; i++) {
        if (pipe(p->fwd[i]) < 0) return -1;
        if (pipe(p->bwd[i]) < 0) return -1;
    }
    if (pipe(p->start)  < 0) return -1;
    if (pipe(p->notif)  < 0) return -1;
    if (pipe(p->status) < 0) return -1;
    return 0;
}

static void close_in_parent(team_pipes_t* p, int team_size) {
    int N = team_size - 1;
    /* parent does not use any forward/backward pipe ends — close them all */
    for (int i = 0; i < N; i++) {
        close(p->fwd[i][0]); close(p->fwd[i][1]);
        close(p->bwd[i][0]); close(p->bwd[i][1]);
    }
    /* parent keeps: start[1] (write), notif[1] (write), status[0] (read) */
    close(p->start[0]);
    close(p->notif[0]);
    close(p->status[1]);
}

/* Run inside the child after fork: close every fd not needed for this member. */
static void close_in_child_member(team_pipes_t* p, int team_size, int member_id) {
    int N = team_size - 1;

    for (int i = 0; i < N; i++) {
        /* forward pipe i: writer is member i, reader is member i+1 */
        if (member_id != i)     close(p->fwd[i][1]);   /* not the writer */
        if (member_id != i + 1) close(p->fwd[i][0]);   /* not the reader */
        /* backward pipe i: writer is member i+1, reader is member i */
        if (member_id != i + 1) close(p->bwd[i][1]);
        if (member_id != i)     close(p->bwd[i][0]);
    }

    /* status: members write, parent reads */
    close(p->status[0]);   /* close read end */

    /* start: only source reads */
    close(p->start[1]);                          /* nobody else writes */
    if (member_id != 0) close(p->start[0]);

    /* notif: only source reads */
    close(p->notif[1]);                          /* nobody but parent writes */
    if (member_id != 0) close(p->notif[0]);
}

static void build_member_ctx(member_ctx_t* mc,
                             team_pipes_t* p, int team_id, int member_id, int team_size) {
    int N = team_size - 1;
    memset(mc, 0, sizeof *mc);
    mc->team_id    = team_id;
    mc->member_id  = member_id;
    mc->team_size  = team_size;

    /* defaults */
    mc->fd_in_forward = mc->fd_out_forward = -1;
    mc->fd_in_backward = mc->fd_out_backward = -1;
    mc->fd_start_in = -1;

    /* forward read: member i reads forward pipe (i-1) */
    if (member_id > 0) mc->fd_in_forward = p->fwd[member_id - 1][0];
    /* forward write: member i writes forward pipe i */
    if (member_id < N) mc->fd_out_forward = p->fwd[member_id][1];
    /* backward read: member i reads backward pipe i */
    if (member_id < N) mc->fd_in_backward = p->bwd[member_id][0];
    /* backward write: member i writes backward pipe (i-1) */
    if (member_id > 0) mc->fd_out_backward = p->bwd[member_id - 1][1];

    mc->fd_status_out = p->status[1];
    mc->fd_start_in = -1;
    mc->fd_notif_in = -1;
    if (member_id == 0) {
        mc->fd_start_in = p->start[0];
        mc->fd_notif_in = p->notif[0];
    }
}

/* === public API === */

int referee_setup(world_t* world, const cfg_t* cfg) {
    memset(world, 0, sizeof *world);
    world->cfg = *cfg;
    world->current_round = 0;
    world->winner = -1;

    for (int t = 0; t < NUM_TEAMS; t++) {
        if (alloc_team_pipes(&g_pipes[t], cfg->team_size) < 0) {
            perror("alloc_team_pipes");
            return -1;
        }
    }

    /*  fork BEFORE any OpenMP threads are created. */
    for (int t = 0; t < NUM_TEAMS; t++) {
        for (int m = 0; m < cfg->team_size; m++) {
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); return -1; }

            if (pid == 0) {
                /* CHILD */
                close_in_child_member(&g_pipes[t], cfg->team_size, m);
                /* close fds belonging to the OTHER team entirely */
                int other = 1 - t;
                int Nother = cfg->team_size - 1;
                for (int i = 0; i < Nother; i++) {
                    close(g_pipes[other].fwd[i][0]); close(g_pipes[other].fwd[i][1]);
                    close(g_pipes[other].bwd[i][0]); close(g_pipes[other].bwd[i][1]);
                }
                close(g_pipes[other].start[0]); close(g_pipes[other].start[1]);
                close(g_pipes[other].notif[0]); close(g_pipes[other].notif[1]);
                close(g_pipes[other].status[0]); close(g_pipes[other].status[1]);

                member_ctx_t mc;
                build_member_ctx(&mc, &g_pipes[t], t, m, cfg->team_size);
                member_run(&mc, cfg);
                _exit(0);
            }
            /* PARENT */
            world->teams[t].member_pids[m] = pid;
        }
    }

    /* PARENT: close member-only fds */
    for (int t = 0; t < NUM_TEAMS; t++) close_in_parent(&g_pipes[t], cfg->team_size);

    /* keep handy fds in world */
    for (int t = 0; t < NUM_TEAMS; t++) {
        world->teams[t].fd_status_in = g_pipes[t].status[0];
        world->teams[t].fd_start_out = g_pipes[t].start[1];
        world->teams[t].fd_notif_out = g_pipes[t].notif[1];
        for (int i = 0; i < cfg->num_pieces; i++) {
            world->teams[t].piece_position[i] = -1;
            world->teams[t].piece_serial[i]   = 0;
            world->teams[t].delivered[i] = 0;
        }
        world->teams[t].wins = 0;
        world->teams[t].delivered_in_round = 0;

        /* status pipe will be polled non-blockingly */
        set_nonblock(world->teams[t].fd_status_in);
    }

    /* Start round 1 */
    referee_start_round(world);
    return 0;
}

void referee_start_round(world_t* world) {
    world->current_round++;

    /* parallel reset of per-piece state */
    #pragma omp parallel for collapse(2)
    for (int t = 0; t < NUM_TEAMS; t++) {
        for (int i = 0; i < world->cfg.num_pieces; i++) {
            world->teams[t].piece_position[i] = 0; /* at source */
            world->teams[t].piece_serial[i]   = 0;
            world->teams[t].delivered[i] = 0;
        }
    }
    for (int t = 0; t < NUM_TEAMS; t++) world->teams[t].delivered_in_round = 0;

    msg_t start;
    memset(&start, 0, sizeof start);
    start.kind = MSG_START_ROUND;
    start.round = world->current_round;

    for (int t = 0; t < NUM_TEAMS; t++) {
        if (write_full(world->teams[t].fd_start_out, &start, sizeof start) < 0) {
            if (errno != EPIPE) perror("write start");
        }
    }

    if (world->cfg.verbose)
        printf("[referee] === ROUND %d ===\n", world->current_round);
}

static void broadcast_signal(world_t* world, int sig) {
    for (int t = 0; t < NUM_TEAMS; t++) {
        for (int m = 0; m < world->cfg.team_size; m++) {
            pid_t p = world->teams[t].member_pids[m];
            if (p > 0) kill(p, sig);
        }
    }
}

void referee_tick(world_t* world) {
    if (world->shutting_down) return;

    /* Drain status pipes non-blockingly. */
    for (int t = 0; t < NUM_TEAMS; t++) {
        int fd = world->teams[t].fd_status_in;
        for (;;) {
            status_msg_t s;
            ssize_t r = read(fd, &s, sizeof s);
            if (r == (ssize_t)sizeof s) {
                /* update visualization model */
                if (s.kind == STATUS_TRACE) {
                    if (s.piece_index >= 0 && s.piece_index < world->cfg.num_pieces) {
                        world->teams[t].piece_position[s.piece_index] = s.member;
                        world->teams[t].piece_serial[s.piece_index]   = s.serial;
                    }
                    /* If the SINK emitted this trace with direction=-1 it means
                     * it just rejected the piece and put it on the backward pipe.
                     * The forward pipe is now free — tell the source immediately
                     * so it can send the next piece without waiting for the
                     * physical return journey (2-in-flight pipelining). */
                    if (s.direction == -1 && s.member == world->cfg.team_size - 1) {
                        msg_t rn;
                        memset(&rn, 0, sizeof rn);
                        rn.kind        = MSG_REJECTION_NOTIF;
                        rn.serial      = s.serial;
                        rn.piece_index = s.piece_index;
                        rn.round       = s.round;
                        rn.direction   = -1;
                        if (write_full(world->teams[t].fd_notif_out, &rn, sizeof rn) < 0) {
                            if (errno != EPIPE && world->cfg.verbose)
                                fprintf(stderr, "[referee] rejection notif write failed: %s\n",
                                        strerror(errno));
                        }
                    }
                } else if (s.kind == STATUS_DELIVERED) {
                    if (s.piece_index >= 0 && s.piece_index < world->cfg.num_pieces) {
                        world->teams[t].delivered[s.piece_index] = 1;
                        world->teams[t].piece_position[s.piece_index] = world->cfg.team_size - 1;
                    }
                    world->teams[t].delivered_in_round = s.delivered_count;

                    /* Notify the source that a piece was delivered, so it can
                     * clear rejection state and pick the next piece. */
                    msg_t notif;
                    memset(&notif, 0, sizeof notif);
                    notif.kind        = MSG_DELIVERY_NOTIF;
                    notif.serial      = s.serial;
                    notif.piece_index = s.piece_index;
                    notif.round       = s.round;
                    notif.direction   = +1;
                    /*  source may have died or pipe may be full;
                     * we don't block here. */
                    if (write_full(world->teams[t].fd_notif_out, &notif, sizeof notif) < 0) {
                        if (errno != EPIPE && world->cfg.verbose)
                            fprintf(stderr, "[referee] notif write failed: %s\n", strerror(errno));
                    }
                } else if (s.kind == STATUS_WIN) {
                    /* Round won! */
                    world->teams[t].wins++;
                    if (world->cfg.verbose)
                        printf("[referee] team %d won round %d (total wins: %d)\n",
                               t, world->current_round, world->teams[t].wins);

                    if (world->teams[t].wins >= world->cfg.wins_to_match) {
                        world->winner = t;
                        printf("\n*** TEAM %c WINS THE COMPETITION (%d wins) ***\n\n",
                               (char)('A' + t), world->teams[t].wins);
                        world->shutting_down = 1;
                        broadcast_signal(world, SIGUSR2);
                        return;
                    }

                    /* Otherwise, broadcast reset and start next round. */
                    broadcast_signal(world, SIGUSR1);
                    /* Tiny pause so members handle the signal before we send START. */
                    struct timespec ts = {0, 50 * 1000000L};   /* 50 ms */
                    nanosleep(&ts, NULL);
                    referee_start_round(world);
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else if (r < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }
}

void referee_shutdown(world_t* world) {
    world->shutting_down = 1;
    broadcast_signal(world, SIGUSR2);

    /* Give children a moment to exit cleanly. */
    struct timespec ts = {0, 200 * 1000000L};
    nanosleep(&ts, NULL);

    /* Reap whatever's done; SIGKILL anything stuck. */
    for (int round = 0; round < 5; round++) {
        int any_left = 0;
        for (int t = 0; t < NUM_TEAMS; t++) {
            for (int m = 0; m < world->cfg.team_size; m++) {
                pid_t p = world->teams[t].member_pids[m];
                if (p <= 0) continue;
                int status;
                pid_t r = waitpid(p, &status, WNOHANG);
                if (r == p) world->teams[t].member_pids[m] = 0;
                else if (r == 0) any_left = 1;
            }
        }
        if (!any_left) break;
        nanosleep(&ts, NULL);
    }
    for (int t = 0; t < NUM_TEAMS; t++) {
        for (int m = 0; m < world->cfg.team_size; m++) {
            pid_t p = world->teams[t].member_pids[m];
            if (p > 0) {
                kill(p, SIGKILL);
                waitpid(p, NULL, 0);
                world->teams[t].member_pids[m] = 0;
            }
        }
    }

    /* Close pipes in parent. */
    for (int t = 0; t < NUM_TEAMS; t++) {
        if (world->teams[t].fd_status_in > 0) close(world->teams[t].fd_status_in);
        if (world->teams[t].fd_start_out > 0) close(world->teams[t].fd_start_out);
        if (world->teams[t].fd_notif_out > 0) close(world->teams[t].fd_notif_out);
    }
    for (int t = 0; t < NUM_TEAMS; t++) {
        free(g_pipes[t].fwd);
        free(g_pipes[t].bwd);
    }
}
