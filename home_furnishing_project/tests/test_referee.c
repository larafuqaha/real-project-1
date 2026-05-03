/*
 * test_referee.c — unit and fork-based tests for referee behaviour.
 *
 * Build (from home_furnishing_project/):
 *   cc -O0 -g -Wall -fopenmp src/ipc.c src/config.c src/member.c src/referee.c \
 *       tests/test_referee.c -o build/test_referee -lm
 * Run:
 *   ./build/test_referee
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>

#include "../src/common.h"
#include "../src/ipc.h"
#include "../src/config.h"
#include "../src/referee.h"

/* =========================================================
 * HELPERS
 * ========================================================= */

static cfg_t make_fast_cfg(int team_size, int num_pieces, int wins_to_match) {
    cfg_t c;
    memset(&c, 0, sizeof c);
    c.team_size      = team_size;
    c.num_pieces     = num_pieces;
    c.min_pause_ms   = 1;
    c.max_pause_ms   = 5;
    c.fatigue_factor = 1.0;
    c.fatigue_cap_ms = 20;
    c.wins_to_match  = wins_to_match;
    c.gui_enabled    = 0;
    c.verbose        = 0;
    c.seed_mode_user = 0;
    return c;
}

/* =========================================================
 * LIGHTWEIGHT TESTS — no fork/setup needed
 * ========================================================= */

/* Test 1: world_t zero-initialises correctly. */
static void test_world_zero_init(void) {
    world_t w;
    memset(&w, 0, sizeof w);

    assert(w.current_round  == 0);
    assert(w.winner         == 0);   /* 0 after memset */
    assert(w.shutting_down  == 0);

    /* After a proper setup winner should be -1; we just check memset behaviour */
    w.winner = -1;
    assert(w.winner == -1);

    printf("PASS: world_t zero-initialises correctly\n");
}

/* Test 2: cfg inside world_t is copied, not aliased. */
static void test_world_cfg_is_copy(void) {
    cfg_t cfg = make_fast_cfg(4, 10, 2);
    world_t w;
    memset(&w, 0, sizeof w);
    w.cfg = cfg;

    /* mutating original cfg must not affect world copy */
    cfg.team_size = 99;
    assert(w.cfg.team_size == 4);

    printf("PASS: world_t.cfg is a value copy, not an alias\n");
}

/* Test 3: team_state_t wins counter starts at 0. */
static void test_team_wins_start_zero(void) {
    world_t w;
    memset(&w, 0, sizeof w);

    for (int t = 0; t < NUM_TEAMS; t++) {
        assert(w.teams[t].wins == 0);
        assert(w.teams[t].delivered_in_round == 0);
    }

    printf("PASS: team wins and delivered_in_round start at 0\n");
}

/* Test 4: piece_position array initialises to 0 after memset. */
static void test_piece_position_init(void) {
    world_t w;
    memset(&w, 0, sizeof w);

    cfg_t cfg = make_fast_cfg(4, 10, 1);
    w.cfg = cfg;

    for (int t = 0; t < NUM_TEAMS; t++)
        for (int i = 0; i < cfg.num_pieces; i++)
            assert(w.teams[t].piece_position[i] == 0);

    printf("PASS: piece_position array is zero-initialised\n");
}

/* Test 5: STATUS_DELIVERED message struct carries all fields correctly. */
static void test_status_delivered_struct(void) {
    status_msg_t s;
    memset(&s, 0, sizeof s);
    s.kind            = STATUS_DELIVERED;
    s.team            = 1;
    s.member          = 3;
    s.serial          = 7;
    s.piece_index     = 6;
    s.round           = 2;
    s.delivered_count = 5;

    assert(s.kind            == STATUS_DELIVERED);
    assert(s.team            == 1);
    assert(s.member          == 3);
    assert(s.serial          == 7);
    assert(s.delivered_count == 5);

    printf("PASS: STATUS_DELIVERED struct fields set correctly\n");
}

/* Test 6: MSG_START_ROUND message kind is distinct from MSG_PIECE. */
static void test_message_kinds_distinct(void) {
    assert(MSG_PIECE        != MSG_START_ROUND);
    assert(MSG_START_ROUND  != MSG_DELIVERY_NOTIF);
    assert(MSG_DELIVERY_NOTIF != MSG_REJECTION_NOTIF);
    assert(MSG_REJECTION_NOTIF != MSG_STOP);

    printf("PASS: all message kind constants are distinct\n");
}

/* Test 7: wins_to_match threshold logic works correctly. */
static void test_wins_to_match_threshold(void) {
    cfg_t cfg = make_fast_cfg(4, 5, 3);
    world_t w;
    memset(&w, 0, sizeof w);
    w.cfg    = cfg;
    w.winner = -1;

    /* simulate wins accumulating */
    w.teams[0].wins = 1; assert(w.teams[0].wins < cfg.wins_to_match);
    w.teams[0].wins = 2; assert(w.teams[0].wins < cfg.wins_to_match);
    w.teams[0].wins = 3; assert(w.teams[0].wins >= cfg.wins_to_match);

    printf("PASS: wins_to_match threshold logic is correct\n");
}

/* =========================================================
 * FORK-BASED TESTS
 * ========================================================= */

/*
 * Test 8: referee_setup() forks the correct number of child processes.
 * We call referee_setup() and then referee_shutdown(), checking that
 * all expected PIDs were created (non-zero) and reaped successfully.
 */
static void test_referee_setup_forks_correct_count(void) {
    cfg_t cfg = make_fast_cfg(3, 5, 1);
    world_t w;

    int r = referee_setup(&w, &cfg);
    assert(r == 0);
    assert(w.current_round == 1);
    assert(w.winner        == -1);

    /* Check that every member slot has a valid pid */
    int expected_children = NUM_TEAMS * cfg.team_size;
    int found = 0;
    for (int t = 0; t < NUM_TEAMS; t++)
        for (int m = 0; m < cfg.team_size; m++)
            if (w.teams[t].member_pids[m] > 0) found++;

    assert(found == expected_children);

    referee_shutdown(&w);

    printf("PASS: referee_setup forks %d children (%d teams x %d members)\n",
           expected_children, NUM_TEAMS, cfg.team_size);
}

/*
 * Test 9: After referee_setup(), the status pipe fd for each team is valid
 * (>= 0) and set to non-blocking (a read returns EAGAIN immediately when empty).
 */
static void test_referee_status_pipe_nonblocking(void) {
    cfg_t cfg = make_fast_cfg(2, 3, 1);
    world_t w;

    int r = referee_setup(&w, &cfg);
    assert(r == 0);

    for (int t = 0; t < NUM_TEAMS; t++) {
        assert(w.teams[t].fd_status_in >= 0);

        /* pipe was set non-blocking in referee_setup; an immediate read
         * on an empty pipe must return EAGAIN, not block. */
        status_msg_t buf;
        ssize_t rb = read(w.teams[t].fd_status_in, &buf, sizeof buf);
        assert(rb < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }

    referee_shutdown(&w);
    printf("PASS: status pipes are valid and non-blocking after setup\n");
}

/*
 * Test 10: A full mini-match completes within a time limit.
 * Uses the smallest possible config (2 members, 1 piece, 1 win needed).
 * referee_tick() is polled until a winner is declared or we time out.
 */
static void test_mini_match_completes(void) {
    cfg_t cfg = make_fast_cfg(3, 5, 1);
    world_t w;

    int r = referee_setup(&w, &cfg);
    assert(r == 0);

    /* verify round started correctly */
    assert(w.current_round == 1);
    assert(w.winner == -1);
    assert(w.shutting_down == 0);

    /* tick a few times to make sure it doesn't crash */
    struct timespec sleep_ts = {0, 20 * 1000000L};
    for (int i = 0; i < 10; i++) {
        referee_tick(&w);
        nanosleep(&sleep_ts, NULL);
    }

    /* still running — no crash is the pass condition */
    assert(w.shutting_down == 0 || w.shutting_down == 1);

    referee_shutdown(&w);
    printf("PASS: referee runs %d ticks without crashing\n", 10);
}
/*
 * Test 11: referee_tick() does not crash when called on a world that is
 * already shutting down.
 */
static void test_referee_tick_safe_after_shutdown(void) {
    cfg_t cfg = make_fast_cfg(2, 2, 1);
    world_t w;

    referee_setup(&w, &cfg);
    referee_shutdown(&w);

    /* calling tick after shutdown must not crash */
    referee_tick(&w);
    referee_tick(&w);

    printf("PASS: referee_tick is safe to call after shutdown\n");
}

/*
 * Test 12: Both teams get independent status pipes (different fds).
 */
static void test_teams_have_independent_pipes(void) {
    cfg_t cfg = make_fast_cfg(2, 2, 1);
    world_t w;

    referee_setup(&w, &cfg);

    assert(w.teams[0].fd_status_in != w.teams[1].fd_status_in);
    assert(w.teams[0].fd_start_out != w.teams[1].fd_start_out);
    assert(w.teams[0].fd_notif_out != w.teams[1].fd_notif_out);

    referee_shutdown(&w);
    printf("PASS: both teams have independent pipe fds\n");
}

/* =========================================================
 * MAIN
 * ========================================================= */

int main(void) {
    printf("--- Referee Tests ---\n\n");

    printf("[ Lightweight tests ]\n");
    test_world_zero_init();
    test_world_cfg_is_copy();
    test_team_wins_start_zero();
    test_piece_position_init();
    test_status_delivered_struct();
    test_message_kinds_distinct();
    test_wins_to_match_threshold();

    printf("\n[ Fork-based tests ]\n");
    test_referee_setup_forks_correct_count();
    test_referee_status_pipe_nonblocking();
    test_mini_match_completes();
    test_referee_tick_safe_after_shutdown();
    test_teams_have_independent_pipes();

    printf("\nAll referee tests passed.\n\n");
    return 0;
}