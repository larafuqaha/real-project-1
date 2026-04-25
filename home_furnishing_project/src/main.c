/*
 * main.c — entry point for the home furnishing competition.
 *
 * Reads config (path from argv[1] if given, else config/default.cfg).
 * Forks all member processes via referee_setup().
 * Hands off to GLUT (gui_run) or headless mode based on config.
 */

#include "common.h"
#include "config.h"
#include "referee.h"
#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static world_t g_world;

/* Reap zombies so dying members don't accumulate. */
static void on_sigchld(int s) {
    (void)s;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) ;
    errno = saved_errno;
}

/* Ctrl-C handler: ask referee to shut down. We can't call referee_shutdown
 * directly from a signal handler (not async-safe), so just flip the flag and
 * the GLUT timer / headless loop will notice. */
static void on_sigint(int s) { (void)s; g_world.shutting_down = 1; }

int main(int argc, char** argv) {
    const char* cfg_path = (argc >= 2) ? argv[1] : "config/default.cfg";

    cfg_t cfg;
    if (config_load(&cfg, cfg_path) != 0) {
        fprintf(stderr, "Aborting due to invalid config.\n");
        return 1;
    }
    config_print(&cfg, stdout);

    /* parent signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = on_sigint;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    if (referee_setup(&g_world, &cfg) != 0) {
        fprintf(stderr, "referee_setup failed\n");
        return 1;
    }

    if (cfg.gui_enabled) {
        gui_run(&argc, argv, &g_world);
    } else {
        headless_run(&g_world);
    }

    /* gui_run/headless_run already called referee_shutdown */
    printf("[main] done.\n");
    return 0;
}
