#include "config.h"

#include <ctype.h>
#include <string.h>
#include <errno.h>

static void set_defaults(cfg_t* c) {
    c->team_size       = 5;
    c->num_pieces      = 1000;
    c->min_pause_ms    = 50;
    c->max_pause_ms    = 250;
    c->fatigue_factor  = 1.0005;
    c->fatigue_cap_ms  = 2000;
    c->wins_to_match   = 3;
    c->seed_mode_user  = 0;
    c->user_seed       = 0;
    c->gui_enabled     = 1;
    c->verbose         = 0;
}

/* trim leading/trailing whitespace, in-place */
static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

int config_load(cfg_t* cfg, const char* path) {
    set_defaults(cfg);

    if (!path) {
        fprintf(stderr, "[config] no config path given, using defaults\n");
        return 0;
    }

    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[config] cannot open '%s' (%s) — using defaults\n",
                path, strerror(errno));
        return 0;
    }

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;

        /* strip comments (#... to EOL) */
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char* trimmed = trim(line);
        if (!*trimmed) continue;

        char* eq = strchr(trimmed, '=');
        if (!eq) {
            fprintf(stderr, "[config] line %d: missing '=': %s\n", lineno, trimmed);
            continue;
        }
        *eq = '\0';
        char* key = trim(trimmed);
        char* val = trim(eq + 1);
        if (!*key || !*val) continue;

        if      (!strcmp(key, "team_size"))       cfg->team_size      = atoi(val);
        else if (!strcmp(key, "num_pieces"))      cfg->num_pieces     = atoi(val);
        else if (!strcmp(key, "min_pause_ms"))    cfg->min_pause_ms   = atoi(val);
        else if (!strcmp(key, "max_pause_ms"))    cfg->max_pause_ms   = atoi(val);
        else if (!strcmp(key, "fatigue_factor"))  cfg->fatigue_factor = atof(val);
        else if (!strcmp(key, "fatigue_cap_ms"))  cfg->fatigue_cap_ms = atoi(val);
        else if (!strcmp(key, "wins_to_match"))   cfg->wins_to_match  = atoi(val);
        else if (!strcmp(key, "gui_enabled"))     cfg->gui_enabled    = atoi(val);
        else if (!strcmp(key, "verbose"))         cfg->verbose        = atoi(val);
        else if (!strcmp(key, "seed_mode")) {
            cfg->seed_mode_user = (!strcmp(val, "user"));
        }
        else if (!strcmp(key, "user_seed"))       cfg->user_seed = (unsigned)atol(val);
        else fprintf(stderr, "[config] line %d: unknown key '%s'\n", lineno, key);
    }
    fclose(f);

    /* validation */
    if (cfg->team_size < 2 || cfg->team_size > MAX_TEAM_SIZE) {
        fprintf(stderr, "[config] team_size must be 2..%d\n", MAX_TEAM_SIZE);
        return 1;
    }
    if (cfg->num_pieces < 1 || cfg->num_pieces > MAX_PIECES) {
        fprintf(stderr, "[config] num_pieces must be 1..%d\n", MAX_PIECES);
        return 1;
    }
    if (cfg->min_pause_ms < 0 || cfg->max_pause_ms <= cfg->min_pause_ms) {
        fprintf(stderr, "[config] need 0 <= min_pause_ms < max_pause_ms\n");
        return 1;
    }
    if (cfg->wins_to_match < 1) {
        fprintf(stderr, "[config] wins_to_match must be >= 1\n");
        return 1;
    }
    if (cfg->fatigue_factor < 1.0) {
        fprintf(stderr, "[config] fatigue_factor must be >= 1.0\n");
        return 1;
    }
    return 0;
}

void config_print(const cfg_t* c, FILE* f) {
    fprintf(f, "=== Configuration ===\n");
    fprintf(f, "  team_size       = %d\n",  c->team_size);
    fprintf(f, "  num_pieces      = %d\n",  c->num_pieces);
    fprintf(f, "  min_pause_ms    = %d\n",  c->min_pause_ms);
    fprintf(f, "  max_pause_ms    = %d\n",  c->max_pause_ms);
    fprintf(f, "  fatigue_factor  = %.6f\n", c->fatigue_factor);
    fprintf(f, "  fatigue_cap_ms  = %d\n",  c->fatigue_cap_ms);
    fprintf(f, "  wins_to_match   = %d\n",  c->wins_to_match);
    fprintf(f, "  seed_mode       = %s\n",  c->seed_mode_user ? "user" : "random");
    if (c->seed_mode_user) fprintf(f, "  user_seed       = %u\n", c->user_seed);
    fprintf(f, "  gui_enabled     = %d\n",  c->gui_enabled);
    fprintf(f, "  verbose         = %d\n",  c->verbose);
    fprintf(f, "=====================\n");
}
