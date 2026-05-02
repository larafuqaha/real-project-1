/*
 * common.h — shared types and constants for the home furnishing simulation.
 *.
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

/* ---- compile-time limits ------------------------------------------------ */
#define MAX_TEAM_SIZE     32
#define MAX_PIECES        100000
#define NUM_TEAMS         2

/* ---- message kinds sent on the conveyor pipes --------------------------- */
typedef enum {
    MSG_PIECE           = 1, /* a furniture piece on the move */
    MSG_START_ROUND     = 2, /* parent -> source: begin new round */
    MSG_STOP            = 3, /* graceful stop token */
    MSG_DELIVERY_NOTIF  = 4, /* parent -> source: piece delivered; clear rejections */
    MSG_REJECTION_NOTIF = 5  /* parent -> source: sink rejected a piece; it is now in
                                 the backward pipe -- forward pipe is free, send next */
} msg_kind_t;

/* ---- message struct moving on every pipe -------------------------------- */
/* sizeof(msg_t) is well under PIPE_BUF, so writes are atomic on Linux.    */
typedef struct {
    int kind;          /* msg_kind_t */
    int serial;        /* furniture serial number, 1..num_pieces */
    int piece_index;   /* 0..num_pieces-1, the slot it belongs to */
    int round;         /* round number at the time of send */
    int direction;     /* +1 forward, -1 backward */
    int from_member;   /* who emitted this (debug/animation) */
} msg_t;

/* ---- status pipe: sink → parent ----------------------------------------- */
typedef enum {
    STATUS_DELIVERED = 1,  /* a piece just arrived at the sink in correct order */
    STATUS_WIN       = 2,  /* this team has delivered all pieces */
    STATUS_TRACE     = 3   /* visualization trace from any member */
} status_kind_t;

typedef struct {
    int kind;          /* status_kind_t */
    int team;          /* 0 or 1 */
    int member;        /* who reported it */
    int serial;        /* piece serial */
    int piece_index;
    int round;
    int direction;     /* for traces */
    int delivered_count; /* for STATUS_DELIVERED — how many pieces in this round so far */
} status_msg_t;

/* ---- per-team setup passed to a forked child ---------------------------- */
typedef struct {
    int team_id;
    int member_id;
    int team_size;

    /* fds to keep open inside the member, all others are closed by member.c. */
    int fd_in_forward;   /* -1 if source */
    int fd_out_forward;  /* -1 if sink */
    int fd_in_backward;  /* -1 if sink */
    int fd_out_backward; /* -1 if source */

    int fd_status_out;   /* every member writes traces here; sink also writes wins */
    int fd_start_in;     /* only meaningful for source: -1 otherwise. Source reads
                            START_ROUND tokens from this pipe (parent writes). */
    int fd_notif_in;     /* only meaningful for source: parent writes "delivered"
                            notifications here, source reads to know to clear
                            rejection state and pick next piece. */
} member_ctx_t;

/* ---- runtime configuration --------------------------------------------- */
typedef struct {
    int team_size;
    int num_pieces;
    int min_pause_ms;
    int max_pause_ms;
    double fatigue_factor;   /* multiplier on max_pause per delivery */
    int   fatigue_cap_ms;    /* upper ceiling on pause */
    int wins_to_match;
    int seed_mode_user;      /* 0 = random, 1 = use user_seed */
    unsigned int user_seed;
    int gui_enabled;         /* 0 = headless (no OpenGL), 1 = GLUT window */
    int verbose;             /* extra logging from referee */
} cfg_t;

/* ---- helpers ----------------------------------------------------------- */
static inline void die(const char* msg) {
    perror(msg);
    _exit(1);
}

#endif /* COMMON_H */
