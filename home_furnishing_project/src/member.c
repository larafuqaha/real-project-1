/*
 * member.c — code that runs inside every forked team-member process.
 *
 * Roles:
 *   - source (member 0): owns the furniture pile, picks pieces, sends them
 *     forward, listens for rejected pieces coming back, reads start-of-round
 *     tokens from parent.  supports up to TWO in-flight pieces: one
 *     moving forward (just-sent) and one moving backward (rejected and
 *     returning).  The source no longer has to wait for a rejected piece to
 *     come all the way home before launching the next one.
 *   - middle (1..n-2): forwards in both directions with a tired pause.
 *   - sink (n-1): receives from forward; if piece arrives in correct serial
 *     order it is "delivered" (reported to parent on status pipe), otherwise
 *     it is rejected backwards.
 *
 */

#include "member.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>

/* --- per-process signal flags ------------------------------------------- */
static volatile sig_atomic_t reset_flag = 0;
static volatile sig_atomic_t stop_flag  = 0;
static volatile sig_atomic_t alarm_flag = 0;   /* set by SIGALRM, used by tired_pause */

static void on_reset(int s) { (void)s; reset_flag = 1; }
static void on_stop (int s) { (void)s; stop_flag  = 1; }
static void on_alarm(int s) { (void)s; alarm_flag = 1; }

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);

    
    sa.sa_handler = on_reset; sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    sa.sa_handler = on_stop;  sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* SIGALRM wakes pause() when our setitimer fires. */
    sa.sa_handler = on_alarm; sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    /* SIGPIPE: ignore. We always check write() return values. */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/* --- random pause with fatigue, implemented with setitimer + sigsuspend - */
/*
 * IMPORTANT: we use sigsuspend(2), not the simpler pause(2)+flag idiom.
 * pause() has a classic race condition: a signal can fire between checking
 * the flag and calling pause(), causing pause() to block forever waiting
 * for a signal that's already been delivered.  sigsuspend atomically
 * (1) unblocks the signals listed in its mask and (2) suspends — so the
 * "check flag, then suspend" pair is race-free as long as we block the
 * relevant signals around the check.
 */
static void tired_pause(const cfg_t* cfg, int handled, unsigned int* seed) {
    /* upper bound grows with how many pieces this member has handled */
    double scale = 1.0;
    for (int i = 0; i < handled && scale < 100.0; i++) scale *= cfg->fatigue_factor;

    int hi = (int)(cfg->max_pause_ms * scale);
    if (hi > cfg->fatigue_cap_ms) hi = cfg->fatigue_cap_ms;
    if (hi <= cfg->min_pause_ms) hi = cfg->min_pause_ms + 1;

    int span = hi - cfg->min_pause_ms;
    if (span <= 0) return;
    int ms = cfg->min_pause_ms + (rand_r(seed) % span);
    if (ms <= 0) return;

    /* Block SIGALRM/SIGUSR1/SIGUSR2 while we set up; sigsuspend will
     * atomically unblock them while it sleeps. */
    sigset_t block, prev, empty;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &prev);

    sigemptyset(&empty);   /* mask used by sigsuspend: nothing blocked */

    alarm_flag = 0;

    /* Arm a one-shot itimer for `ms` milliseconds. */
    struct itimerval it;
    memset(&it, 0, sizeof it);
    it.it_value.tv_sec  = ms / 1000;
    it.it_value.tv_usec = (ms % 1000) * 1000L;
    if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
        sigprocmask(SIG_SETMASK, &prev, NULL);
        return;   /* setitimer failed; just skip the pause */
    }

    /* Loop until the alarm fires or a stop/reset arrives.  sigsuspend
     * atomically replaces the signal mask with `empty` and waits for any
     * signal — the handlers run, then control returns here.  Crucially,
     * because the signals are blocked outside this call, we cannot miss
     * one that arrives between the flag check and the sigsuspend call. */
    while (!alarm_flag && !stop_flag && !reset_flag) {
        sigsuspend(&empty);
    }

    /* Disarm in case we left early because of stop/reset. */
    struct itimerval off;
    memset(&off, 0, sizeof off);
    setitimer(ITIMER_REAL, &off, NULL);

    /* Restore the previous signal mask. */
    sigprocmask(SIG_SETMASK, &prev, NULL);
}

/* --- send a status message to parent  ------ */
static void send_status(int fd, status_kind_t k, int team, int member,
                        const msg_t* m, int delivered_count)
{
    if (fd < 0) return;
    status_msg_t s;
    memset(&s, 0, sizeof s);
    s.kind      = (int)k;
    s.team      = team;
    s.member    = member;
    s.serial    = m ? m->serial      : 0;
    s.piece_index = m ? m->piece_index : -1;
    s.round     = m ? m->round       : 0;
    s.direction = m ? m->direction   : 0;
    s.delivered_count = delivered_count;
    write_full(fd, &s, sizeof s);   /* ignore return: parent may have closed */
}

/* ============================ SOURCE =================================== */

/* The source can have up to MAX_INFLIGHT pieces "out there" simultaneously:
 * one going forward (just sent) and one coming back (rejected).*/
/*
 * pick_next_piece — choose a piece index to send forward.
 *
 * Prefer not-yet-rejected pieces. Fall back to any undelivered piece if
 * everything has been rejected at least once (so the round can progress).
 */
static int pick_next_piece(const cfg_t* cfg,
                           const int* delivered, const int* rejected,
                           const int* serials, unsigned int* seed, int* out_idx)
{
    int candidates[MAX_PIECES];
    int nc = 0;

    for (int i = 0; i < cfg->num_pieces; i++) {
        if (!delivered[i] && !rejected[i]) candidates[nc++] = i;
    }
    if (nc == 0) {
        /* fallback: every undelivered piece has been rejected at least once —
         * ignore rejection marks so the round does not stall. */
        for (int i = 0; i < cfg->num_pieces; i++)
            if (!delivered[i]) candidates[nc++] = i;
        if (nc == 0) return 0;
    }
    *out_idx = candidates[rand_r(seed) % nc];
    (void)serials;
    return 1;
}

static void run_source(const member_ctx_t* ctx, const cfg_t* cfg) {
    unsigned int seed = (unsigned)(time(NULL) ^ (getpid() << 16));
    int* delivered = calloc(cfg->num_pieces, sizeof(int));
    int* rejected  = calloc(cfg->num_pieces, sizeof(int));
    int  serials[MAX_PIECES];
    int  current_round = 0;
    int  handled = 0;
    int  delivered_in_round = 0;

    int fwd_in_flight = 0;
    int bwd_in_flight = 0;

    int trace = getenv("FURN_TRACE") != NULL;

    if (!delivered || !rejected) die("calloc");

    while (!stop_flag) {
        /* ---- 1. Wait for START_ROUND from parent ----------------------- */
        if (trace) fprintf(stderr, "[m %d:0 src] waiting for START_ROUND\n",
                           ctx->team_id);
        msg_t start;
        ssize_t r = read_full(ctx->fd_start_in, &start, sizeof start);
        if (r == 0) break;
        if (r < 0) {
            if (stop_flag) break;
            if (reset_flag) { reset_flag = 0; continue; }
            continue;
        }
        if (start.kind != MSG_START_ROUND) continue;

        if (trace) fprintf(stderr, "[m %d:0 src] got START_ROUND round=%d\n",
                           ctx->team_id, start.round);

        current_round = start.round;
        delivered_in_round = 0;
        memset(delivered, 0, cfg->num_pieces * sizeof(int));
        memset(rejected,  0, cfg->num_pieces * sizeof(int));
          /*#pragma omp parallel for
        for (int i = 0; i < cfg->num_pieces; i++) {
            delivered[i] = 0;
            rejected[i]  = 0;
        }*/
        reset_flag    = 0;
        fwd_in_flight = 0;
        bwd_in_flight = 0;

        #pragma omp parallel for
        for (int i = 0; i < cfg->num_pieces; i++) serials[i] = i + 1;

        
        for (int i = cfg->num_pieces - 1; i > 0; i--) {
            int j = rand_r(&seed) % (i + 1);
            int t = serials[i]; serials[i] = serials[j]; serials[j] = t;
        }

        drain_pipe_nonblock(ctx->fd_notif_in);
        drain_pipe_nonblock(ctx->fd_in_backward);

        /* ---- 2. Drive the round --------------------------------------- */
        while (!stop_flag && !reset_flag && delivered_in_round < cfg->num_pieces) {

            if (!fwd_in_flight) {
                int idx;
                if (pick_next_piece(cfg, delivered, rejected,
                                    serials, &seed, &idx)) {
                    msg_t m;
                    memset(&m, 0, sizeof m);
                    m.kind        = MSG_PIECE;
                    m.serial      = serials[idx];
                    m.piece_index = idx;
                    m.round       = current_round;
                    m.direction   = +1;
                    m.from_member = 0;

                    if (write_full(ctx->fd_out_forward, &m, sizeof m) >= 0) {
                        if (trace) {
                            static int counter = 0;
                            if (++counter % 10 == 0)
                                fprintf(stderr,
                                    "[m %d:0 src] sent piece #%d "
                                    "(idx=%d serial=%d round=%d "
                                    "fwd=1 bwd=%d)\n",
                                    ctx->team_id, counter, idx,
                                    serials[idx], current_round,
                                    bwd_in_flight);
                        }
                        send_status(ctx->fd_status_out, STATUS_TRACE,
                                    ctx->team_id, 0, &m, delivered_in_round);
                        fwd_in_flight = 1;
                    } else {
                        if (stop_flag || reset_flag) break;
                    }
                }
            }

            /* Both pipes idle and nothing left to send → round finished */
            if (!fwd_in_flight && !bwd_in_flight) break;

            /* ---- (b) wait for an event --------------------------------- */
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ctx->fd_in_backward, &rfds);
            FD_SET(ctx->fd_notif_in,    &rfds);
            int maxfd = ctx->fd_in_backward;
            if (ctx->fd_notif_in > maxfd) maxfd = ctx->fd_notif_in;

            int sel = select(maxfd + 1, &rfds, NULL, NULL, NULL);
            if (sel < 0) {
                if (errno == EINTR) {
                    if (stop_flag || reset_flag) break;
                    continue;
                }
                break;
            }

            /* ---- backward pipe: a rejected piece has returned to us --- */
            if (FD_ISSET(ctx->fd_in_backward, &rfds)) {
                msg_t back;
                ssize_t br = read_full(ctx->fd_in_backward, &back, sizeof back);
                if (br <= 0) {
                    if (stop_flag || reset_flag) break;
                    continue;
                }
                if (back.round != current_round) continue;

                back.from_member = 0;
                send_status(ctx->fd_status_out, STATUS_TRACE,
                            ctx->team_id, 0, &back, delivered_in_round);

                rejected[back.piece_index] = 1;
                bwd_in_flight = 0;   /* backward pipe now free */

                /* If we had NOT already sent a new piece forward while this
                 * one was coming back (fwd_in_flight==0), we set
                 * fwd_in_flight=0 here and will send on the next iteration.
                 * If fwd_in_flight==1 we already pipelined — do nothing. */
                if (!fwd_in_flight) {
                    /* We were in state (0,1) waiting for the reject; now
                     * both flags are 0 so the top of the loop will send. */
                }

                handled++;
                tired_pause(cfg, handled, &seed);
            }

            /* ---- notif pipe: parent says a piece was delivered --------- */
            if (FD_ISSET(ctx->fd_notif_in, &rfds)) {
                msg_t notif;
                ssize_t nr = read_full(ctx->fd_notif_in, &notif, sizeof notif);
                if (nr <= 0) {
                    if (stop_flag || reset_flag) break;
                    continue;
                }
                if (notif.round != current_round) continue;

                if (notif.kind == MSG_DELIVERY_NOTIF) {
                    if (notif.piece_index >= 0 && notif.piece_index < cfg->num_pieces)
                        delivered[notif.piece_index] = 1;
                    delivered_in_round++;
                    fwd_in_flight = 0;   /* forward pipe now free */
                    memset(rejected, 0, cfg->num_pieces * sizeof(int));
                    handled++;
                    tired_pause(cfg, handled, &seed);
                } else if (notif.kind == MSG_REJECTION_NOTIF) {
                    /* Sink just rejected a piece — it is now in the backward
                     * pipe travelling home.  Forward pipe is free right now. */
                    fwd_in_flight = 0;
                    bwd_in_flight = 1;
                    /* No pause — the source didn't handle anything yet;
                     * it will pause when the piece physically arrives. */
                }
            }
        }

        if (reset_flag) {
            drain_pipe_nonblock(ctx->fd_in_backward);
            drain_pipe_nonblock(ctx->fd_notif_in);
        }
    }

    free(delivered);
    free(rejected);
}

/* ============================ MIDDLE =================================== */

static void run_middle(const member_ctx_t* ctx, const cfg_t* cfg) {
    unsigned int seed = (unsigned)(time(NULL) ^ (getpid() << 16));
    int handled = 0;
    int trace = getenv("FURN_TRACE") != NULL;
    int fwd_count = 0, bwd_count = 0;

    while (!stop_flag) {
        if (reset_flag) {
            reset_flag = 0;
            if (ctx->fd_in_forward  >= 0) drain_pipe_nonblock(ctx->fd_in_forward);
            if (ctx->fd_in_backward >= 0) drain_pipe_nonblock(ctx->fd_in_backward);
            handled = 0;
            fwd_count = bwd_count = 0;
            if (trace) fprintf(stderr, "[m %d:%d mid] RESET (top)\n",
                               ctx->team_id, ctx->member_id);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (ctx->fd_in_forward  >= 0) { FD_SET(ctx->fd_in_forward,  &rfds); if (ctx->fd_in_forward  > maxfd) maxfd = ctx->fd_in_forward; }
        if (ctx->fd_in_backward >= 0) { FD_SET(ctx->fd_in_backward, &rfds); if (ctx->fd_in_backward > maxfd) maxfd = ctx->fd_in_backward; }
        if (maxfd < 0) break;

        int sel = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (sel < 0) {
            if (errno == EINTR) {
                if (stop_flag) break;
                if (reset_flag) {
                    reset_flag = 0;
                    if (ctx->fd_in_forward  >= 0) drain_pipe_nonblock(ctx->fd_in_forward);
                    if (ctx->fd_in_backward >= 0) drain_pipe_nonblock(ctx->fd_in_backward);
                    handled = 0;
                    continue;
                }
                continue;
            }
            break;
        }

        /* forward direction */
        if (ctx->fd_in_forward >= 0 && FD_ISSET(ctx->fd_in_forward, &rfds)) {
            msg_t m;
            ssize_t r = read_full(ctx->fd_in_forward, &m, sizeof m);
            if (r > 0 && m.kind == MSG_PIECE) {
                m.from_member = ctx->member_id;
                send_status(ctx->fd_status_out, STATUS_TRACE, ctx->team_id, ctx->member_id, &m, 0);
                tired_pause(cfg, handled++, &seed);
                if (reset_flag || stop_flag) continue;
                if (write_full(ctx->fd_out_forward, &m, sizeof m) < 0) continue;
                fwd_count++;
                if (trace && fwd_count % 20 == 0)
                    fprintf(stderr, "[m %d:%d mid] forwarded %d pieces (last serial=%d round=%d)\n",
                            ctx->team_id, ctx->member_id, fwd_count, m.serial, m.round);
            }
        }

        /* backward direction */
        if (ctx->fd_in_backward >= 0 && FD_ISSET(ctx->fd_in_backward, &rfds)) {
            msg_t m;
            ssize_t r = read_full(ctx->fd_in_backward, &m, sizeof m);
            if (r > 0 && m.kind == MSG_PIECE) {
                m.from_member = ctx->member_id;
                send_status(ctx->fd_status_out, STATUS_TRACE, ctx->team_id, ctx->member_id, &m, 0);
                tired_pause(cfg, handled++, &seed);
                if (reset_flag || stop_flag) continue;
                if (write_full(ctx->fd_out_backward, &m, sizeof m) < 0) continue;
                bwd_count++;
            }
        }
    }
}

/* ============================ SINK ===================================== */

static void run_sink(const member_ctx_t* ctx, const cfg_t* cfg) {
    unsigned int seed = (unsigned)(time(NULL) ^ (getpid() << 16));
    int handled = 0;
    int next_expected_serial = 1;
    int delivered_this_round = 0;
    int last_round = -1;
    int trace = getenv("FURN_TRACE") != NULL;

    while (!stop_flag) {
        msg_t m;
        ssize_t r = read_full(ctx->fd_in_forward, &m, sizeof m);
        if (r == 0) break;
        if (r < 0) {
            if (stop_flag) break;
            if (reset_flag) {
                if (trace) fprintf(stderr, "[m %d:%d sink] RESET, drained\n",
                                   ctx->team_id, ctx->member_id);
                reset_flag = 0;
                drain_pipe_nonblock(ctx->fd_in_forward);
                next_expected_serial = 1;
                delivered_this_round = 0;
                handled = 0;
                continue;
            }
            continue;
        }
        if (m.kind != MSG_PIECE) continue;

        if (m.round != last_round) {
            if (trace) fprintf(stderr, "[m %d:%d sink] new round %d (was %d), expecting serial 1\n",
                               ctx->team_id, ctx->member_id, m.round, last_round);
            last_round = m.round;
            next_expected_serial = 1;
            delivered_this_round = 0;
        }

        m.from_member = ctx->member_id;
        send_status(ctx->fd_status_out, STATUS_TRACE, ctx->team_id,
                    ctx->member_id, &m, delivered_this_round);

        tired_pause(cfg, handled++, &seed);
        if (reset_flag || stop_flag) continue;

        if (m.serial == next_expected_serial) {
            delivered_this_round++;
            next_expected_serial++;
            if (trace && (delivered_this_round % 5 == 0 || delivered_this_round == cfg->num_pieces))
                fprintf(stderr, "[m %d:%d sink] DELIVERED %d/%d (serial=%d)\n",
                        ctx->team_id, ctx->member_id, delivered_this_round,
                        cfg->num_pieces, m.serial);
            send_status(ctx->fd_status_out, STATUS_DELIVERED, ctx->team_id,
                        ctx->member_id, &m, delivered_this_round);

            if (delivered_this_round == cfg->num_pieces) {
                send_status(ctx->fd_status_out, STATUS_WIN, ctx->team_id,
                            ctx->member_id, &m, delivered_this_round);
            }
        } else {
            m.direction = -1;
            m.from_member = ctx->member_id;
            if (write_full(ctx->fd_out_backward, &m, sizeof m) < 0) {
                if (stop_flag || reset_flag) continue;
            }
            send_status(ctx->fd_status_out, STATUS_TRACE, ctx->team_id,
                        ctx->member_id, &m, delivered_this_round);
        }
    }
}

/* ============================ DISPATCH ================================= */

void member_run(const member_ctx_t* ctx, const cfg_t* cfg) {
    install_signals();

    if (cfg->seed_mode_user)
        srand(cfg->user_seed ^ ((ctx->team_id << 8) | ctx->member_id));
    else
        srand((unsigned)(time(NULL) ^ (getpid() << 16)));

    int trace = getenv("FURN_TRACE") != NULL;
    if (trace) fprintf(stderr, "[m %d:%d pid=%d] start (role=%s)\n",
                       ctx->team_id, ctx->member_id, getpid(),
                       ctx->member_id == 0 ? "source" :
                       ctx->member_id == ctx->team_size - 1 ? "sink" : "middle");

    if (ctx->member_id == 0) {
        run_source(ctx, cfg);
    } else if (ctx->member_id == ctx->team_size - 1) {
        run_sink(ctx, cfg);
    } else {
        run_middle(ctx, cfg);
    }

    if (trace) fprintf(stderr, "[m %d:%d pid=%d] exit\n",
                       ctx->team_id, ctx->member_id, getpid());

    _exit(0);
}
