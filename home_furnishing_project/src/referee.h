#ifndef REFEREE_H
#define REFEREE_H

#include "common.h"

/* Live state of one team — used by both referee and gui modules. */
typedef struct {
    pid_t   member_pids[MAX_TEAM_SIZE];
    int     fd_status_in;     /* parent reads from here (sink writes) */
    int     fd_start_out;     /* parent writes START_ROUND to source */
    int     fd_notif_out;     /* parent writes delivery notifications to source */

    /* live, for visualization: which member currently has piece p? */
    int     piece_position[MAX_PIECES]; /* member id; -1 if not in flight */
    int     delivered[MAX_PIECES];      /* 0/1 */
    int     wins;
    int     delivered_in_round;
} team_state_t;

typedef struct {
    cfg_t        cfg;
    team_state_t teams[NUM_TEAMS];
    int          current_round;
    int          winner;        /* -1 none, 0 or 1 if match decided */
    int          shutting_down;
} world_t;

/* Entire setup: forks all members, sets up pipes, fills `world`. */
int  referee_setup(world_t* world, const cfg_t* cfg);

/* Read any pending status messages (non-blocking). Updates world state.
 * If a team won this round, it broadcasts SIGUSR1 and starts a new round
 * (or, if wins_to_match reached, sends SIGUSR2 and sets winner). */
void referee_tick(world_t* world);

/* Send SIGUSR2 to all members and waitpid them. */
void referee_shutdown(world_t* world);

/* Begin a new round (fresh seeds, parent writes START_ROUND token). */
void referee_start_round(world_t* world);

#endif
