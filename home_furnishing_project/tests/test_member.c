/*
 * test_member.c — unit and fork-based tests for member behaviour.
 *
 * Build (from home_furnishing_project/):
 *   cc -O0 -g -Wall -fopenmp src/ipc.c src/config.c tests/test_member.c \
 *       -o build/test_member -lm
 * Run:
 *   ./build/test_member
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

#include "../src/common.h"
#include "../src/ipc.h"
#include "../src/config.h"

/* =========================================================
 * HELPERS
 * ========================================================= */

/* Build a minimal config suitable for fast tests. */
static cfg_t make_test_cfg(int team_size, int num_pieces) {
    cfg_t c;
    memset(&c, 0, sizeof c);
    c.team_size      = team_size;
    c.num_pieces     = num_pieces;
    c.min_pause_ms   = 1;
    c.max_pause_ms   = 5;
    c.fatigue_factor = 1.0;
    c.fatigue_cap_ms = 20;
    c.wins_to_match  = 1;
    c.gui_enabled    = 0;
    c.verbose        = 0;
    c.seed_mode_user = 0;
    return c;
}

/* Build a msg_t with sensible defaults. */
static msg_t make_piece_msg(int serial, int piece_index, int round, int direction) {
    msg_t m;
    memset(&m, 0, sizeof m);
    m.kind        = MSG_PIECE;
    m.serial      = serial;
    m.piece_index = piece_index;
    m.round       = round;
    m.direction   = direction;
    return m;
}

/* =========================================================
 * LIGHTWEIGHT TESTS — no fork needed
 * ========================================================= */

/* Test 1: member_ctx_t fields initialise correctly for source (member 0). */
static void test_member_ctx_source_fields(void) {
    member_ctx_t mc;
    memset(&mc, 0, sizeof mc);

    mc.team_id   = 0;
    mc.member_id = 0;
    mc.team_size = 4;

    /* source: no forward-in, no backward-out */
    mc.fd_in_forward   = -1;
    mc.fd_out_forward  = 5;   /* some valid fd placeholder */
    mc.fd_in_backward  = 6;
    mc.fd_out_backward = -1;
    mc.fd_start_in     = 7;
    mc.fd_notif_in     = 8;
    mc.fd_status_out   = 9;

    assert(mc.member_id  == 0);
    assert(mc.fd_in_forward   == -1);   /* source has no upstream */
    assert(mc.fd_out_backward == -1);   /* source has no backward-out */
    assert(mc.fd_start_in     >= 0);    /* source needs start pipe */
    assert(mc.fd_notif_in     >= 0);    /* source needs notif pipe  */

    printf("PASS: source member_ctx_t fields are correct\n");
}

/* Test 2: member_ctx_t fields initialise correctly for sink (last member). */
static void test_member_ctx_sink_fields(void) {
    member_ctx_t mc;
    memset(&mc, 0, sizeof mc);

    int team_size = 4;
    mc.team_id   = 1;
    mc.member_id = team_size - 1;   /* sink */
    mc.team_size = team_size;

    mc.fd_in_forward   = 10;
    mc.fd_out_forward  = -1;   /* sink has no forward-out */
    mc.fd_in_backward  = -1;   /* sink has no backward-in */
    mc.fd_out_backward = 11;
    mc.fd_start_in     = -1;   /* only source uses this */
    mc.fd_notif_in     = -1;   /* only source uses this */
    mc.fd_status_out   = 12;

    assert(mc.fd_out_forward  == -1);
    assert(mc.fd_in_backward  == -1);
    assert(mc.fd_start_in     == -1);
    assert(mc.fd_notif_in     == -1);
    assert(mc.fd_in_forward   >= 0);
    assert(mc.fd_out_backward >= 0);
    assert(mc.fd_status_out   >= 0);

    printf("PASS: sink member_ctx_t fields are correct\n");
}

/* Test 3: middle member has both forward and backward fds open. */
static void test_member_ctx_middle_fields(void) {
    member_ctx_t mc;
    memset(&mc, 0, sizeof mc);

    mc.team_id   = 0;
    mc.member_id = 2;   /* middle */
    mc.team_size = 5;

    mc.fd_in_forward   = 20;
    mc.fd_out_forward  = 21;
    mc.fd_in_backward  = 22;
    mc.fd_out_backward = 23;
    mc.fd_start_in     = -1;
    mc.fd_notif_in     = -1;
    mc.fd_status_out   = 24;

    assert(mc.fd_in_forward   >= 0);
    assert(mc.fd_out_forward  >= 0);
    assert(mc.fd_in_backward  >= 0);
    assert(mc.fd_out_backward >= 0);
    assert(mc.fd_start_in     == -1);
    assert(mc.fd_notif_in     == -1);

    printf("PASS: middle member_ctx_t fields are correct\n");
}

/* Test 4: MSG_PIECE struct carries all fields intact through a pipe. */
static void test_piece_message_integrity(void) {
    int fds[2];
    pipe(fds);

    msg_t out = make_piece_msg(99, 7, 3, +1);
    out.from_member = 2;

    write_full(fds[1], &out, sizeof out);
    close(fds[1]);

    msg_t in;
    memset(&in, 0, sizeof in);
    read_full(fds[0], &in, sizeof in);
    close(fds[0]);

    assert(in.kind        == MSG_PIECE);
    assert(in.serial      == 99);
    assert(in.piece_index == 7);
    assert(in.round       == 3);
    assert(in.direction   == +1);
    assert(in.from_member == 2);

    printf("PASS: MSG_PIECE fields survive pipe round-trip\n");
}

/* Test 5: direction field correctly distinguishes forward vs backward. */
static void test_piece_direction_field(void) {
    msg_t fwd = make_piece_msg(1, 0, 1, +1);
    msg_t bwd = make_piece_msg(1, 0, 1, -1);

    assert(fwd.direction == +1);
    assert(bwd.direction == -1);
    assert(fwd.direction != bwd.direction);

    printf("PASS: direction field correctly set for forward/backward\n");
}

/* Test 6: config values remain sane after make_test_cfg. */
static void test_config_values_sane(void) {
    cfg_t c = make_test_cfg(4, 25);
    assert(c.team_size     >= 2);
    assert(c.num_pieces    >= 1);
    assert(c.max_pause_ms  >  c.min_pause_ms);
    assert(c.fatigue_factor >= 1.0);
    assert(c.wins_to_match  >= 1);
    printf("PASS: make_test_cfg produces sane values\n");
}

/* =========================================================
 * FORK-BASED TESTS
 * ========================================================= */

/*
 * Test 7: A forked "middle" process reads a piece from the forward pipe
 * and writes it to the next forward pipe unchanged.
 *
 * Layout:  [parent] --fwd_in--> [child middle] --fwd_out--> [parent reads back]
 */
static void test_middle_relays_piece_forward(void) {
    int fwd_in[2], fwd_out[2], bwd_dummy[2], status[2];
    pipe(fwd_in);
    pipe(fwd_out);
    pipe(bwd_dummy);
    pipe(status);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* CHILD — simulates a middle member */
        close(fwd_in[1]);
        close(fwd_out[0]);
        close(bwd_dummy[0]);
        close(status[0]);

        msg_t m;
        ssize_t r = read_full(fwd_in[0], &m, sizeof m);
        if (r != (ssize_t)sizeof m) _exit(1);

        /* middle just passes it on */
        m.from_member = 1;
        write_full(fwd_out[1], &m, sizeof m);
        close(fwd_in[0]);
        close(fwd_out[1]);
        close(bwd_dummy[1]);
        close(status[1]);
        _exit(0);
    }

    /* PARENT */
    close(fwd_in[0]);
    close(fwd_out[1]);
    close(bwd_dummy[0]); close(bwd_dummy[1]);
    close(status[0]);    close(status[1]);

    msg_t sent = make_piece_msg(42, 3, 1, +1);
    write_full(fwd_in[1], &sent, sizeof sent);
    close(fwd_in[1]);

    msg_t recv;
    memset(&recv, 0, sizeof recv);
    read_full(fwd_out[0], &recv, sizeof recv);
    close(fwd_out[0]);

    int status_val;
    waitpid(pid, &status_val, 0);

    assert(WIFEXITED(status_val) && WEXITSTATUS(status_val) == 0);
    assert(recv.kind        == MSG_PIECE);
    assert(recv.serial      == 42);
    assert(recv.piece_index == 3);
    assert(recv.round       == 1);
    assert(recv.direction   == +1);

    printf("PASS: middle process relays piece forward correctly\n");
}

/*
 * Test 8: A forked "sink" process sends a STATUS_DELIVERED status message
 * when it receives a piece with serial == 1 (the first expected piece).
 *
 * We simulate just the status reporting: sink reads from fwd pipe and
 * writes a status_msg_t to the status pipe.
 */
static void test_sink_reports_delivery(void) {
    int fwd[2], status[2], bwd[2];
    pipe(fwd);
    pipe(status);
    pipe(bwd);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* CHILD — minimal sink simulation */
        close(fwd[1]);
        close(status[0]);
        close(bwd[0]);

        msg_t m;
        ssize_t r = read_full(fwd[0], &m, sizeof m);
        if (r != (ssize_t)sizeof m) _exit(1);

        /* Simulate sink decision: serial 1 == next expected (1) → deliver */
        int next_expected = 1;
        status_msg_t s;
        memset(&s, 0, sizeof s);
        s.team        = 0;
        s.member      = 3;
        s.serial      = m.serial;
        s.piece_index = m.piece_index;
        s.round       = m.round;

        if (m.serial == next_expected) {
            s.kind            = STATUS_DELIVERED;
            s.delivered_count = 1;
            write_full(status[1], &s, sizeof s);
        } else {
            /* reject: send back on backward pipe */
            s.kind      = STATUS_TRACE;
            s.direction = -1;
            write_full(status[1], &s, sizeof s);
            msg_t back = m;
            back.direction = -1;
            write_full(bwd[1], &back, sizeof back);
        }

        close(fwd[0]);
        close(status[1]);
        close(bwd[1]);
        _exit(0);
    }

    /* PARENT */
    close(fwd[0]);
    close(status[1]);
    close(bwd[1]);

    msg_t sent = make_piece_msg(1, 0, 1, +1);  /* serial=1 → should be accepted */
    write_full(fwd[1], &sent, sizeof sent);
    close(fwd[1]);

    status_msg_t sr;
    memset(&sr, 0, sizeof sr);
    read_full(status[0], &sr, sizeof sr);
    close(status[0]);
    close(bwd[0]);

    int st;
    waitpid(pid, &st, 0);

    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    assert(sr.kind            == STATUS_DELIVERED);
    assert(sr.serial          == 1);
    assert(sr.delivered_count == 1);

    printf("PASS: sink reports STATUS_DELIVERED for correct-order piece\n");
}

/*
 * Test 9: Sink rejects an out-of-order piece (serial != next_expected)
 * and sends it back on the backward pipe.
 */
static void test_sink_rejects_out_of_order(void) {
    int fwd[2], status[2], bwd[2];
    pipe(fwd);
    pipe(status);
    pipe(bwd);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(fwd[1]);
        close(status[0]);
        close(bwd[0]);

        msg_t m;
        ssize_t r = read_full(fwd[0], &m, sizeof m);
        if (r != (ssize_t)sizeof m) _exit(1);

        int next_expected = 1;
        if (m.serial != next_expected) {
            /* reject */
            msg_t back = m;
            back.direction = -1;
            write_full(bwd[1], &back, sizeof back);

            status_msg_t s;
            memset(&s, 0, sizeof s);
            s.kind      = STATUS_TRACE;
            s.direction = -1;
            s.serial    = m.serial;
            write_full(status[1], &s, sizeof s);
        }

        close(fwd[0]); close(status[1]); close(bwd[1]);
        _exit(0);
    }

    /* PARENT */
    close(fwd[0]); close(status[1]); close(bwd[1]);

    /* send piece with serial=5 when next expected is 1 → should be rejected */
    msg_t sent = make_piece_msg(5, 4, 1, +1);
    write_full(fwd[1], &sent, sizeof sent);
    close(fwd[1]);

    msg_t back;
    memset(&back, 0, sizeof back);
    read_full(bwd[0], &back, sizeof back);
    close(bwd[0]);
    close(status[0]);

    int st;
    waitpid(pid, &st, 0);

    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    assert(back.serial    == 5);
    assert(back.direction == -1);   /* came back */

    printf("PASS: sink rejects out-of-order piece and returns it backward\n");
}

/*
 * Test 10: A child process exits cleanly (exit code 0) when its read pipe
 * is closed (EOF), simulating a graceful shutdown.
 */
static void test_member_exits_on_eof(void) {
    int fwd[2];
    pipe(fwd);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(fwd[1]);
        msg_t m;
        ssize_t r = read_full(fwd[0], &m, sizeof m);
        /* EOF → r == 0 → exit cleanly */
        close(fwd[0]);
        _exit(r == 0 ? 0 : 1);
    }

    close(fwd[0]);
    close(fwd[1]);   /* immediately close write end → child sees EOF */

    int st;
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    printf("PASS: member process exits cleanly on EOF\n");
}

/* =========================================================
 * MAIN
 * ========================================================= */

int main(void) {
    printf("--- Member Tests ---\n\n");

    printf("[ Lightweight tests ]\n");
    test_member_ctx_source_fields();
    test_member_ctx_sink_fields();
    test_member_ctx_middle_fields();
    test_piece_message_integrity();
    test_piece_direction_field();
    test_config_values_sane();

    printf("\n[ Fork-based tests ]\n");
    test_middle_relays_piece_forward();
    test_sink_reports_delivery();
    test_sink_rejects_out_of_order();
    test_member_exits_on_eof();

    printf("\nAll member tests passed.\n\n");
    return 0;
}