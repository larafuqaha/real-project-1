#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/config.h"

static void test_load_small_config(void) {
    cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);

    int r = config_load(&cfg, "config/test_small.txt");
    assert(r == 0);
    assert(cfg.team_size  == 4);
    assert(cfg.num_pieces == 25);
    assert(cfg.gui_enabled == 0);

    printf("PASS: test_small.txt loaded correctly\n");
}

static void test_load_nonexistent_file(void) {
    cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);

    /* loading a missing file should either fail or keep safe defaults */
    config_load(&cfg, "config/does_not_exist.txt");

    /* either way, the resulting config must have sane values */
    assert(cfg.team_size  >= 0);
    assert(cfg.num_pieces >= 0);

    printf("PASS: loading nonexistent file does not crash\n");
}

static void test_load_sets_sane_defaults(void) {
    cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);

    int r = config_load(&cfg, "config/test_small.txt");
    assert(r == 0);

    /* basic sanity on every important field */
    assert(cfg.team_size     >= 2);
    assert(cfg.num_pieces    >= 1);
    assert(cfg.min_pause_ms  >= 0);
    assert(cfg.max_pause_ms  >  cfg.min_pause_ms);
    assert(cfg.wins_to_match >= 1);
    assert(cfg.fatigue_factor >= 1.0);

    printf("PASS: loaded config has sane field values\n");
}

static void test_load_default_config(void) {
    cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);

    int r = config_load(&cfg, "config/default.txt");
    assert(r == 0);
    assert(cfg.team_size     >= 2);
    assert(cfg.num_pieces    >= 1);
    assert(cfg.wins_to_match >= 1);

    printf("PASS: default.txt loaded correctly\n");
}

static void test_two_loads_give_same_result(void) {
    cfg_t a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);

    config_load(&a, "config/test_small.txt");
    config_load(&b, "config/test_small.txt");

    assert(a.team_size     == b.team_size);
    assert(a.num_pieces    == b.num_pieces);
    assert(a.min_pause_ms  == b.min_pause_ms);
    assert(a.max_pause_ms  == b.max_pause_ms);
    assert(a.wins_to_match == b.wins_to_match);

    printf("PASS: loading same file twice gives identical config\n");
}

int main(void) {
    printf("--- Config Tests ---\n");
    test_load_small_config();
    test_load_nonexistent_file();
    test_load_sets_sane_defaults();
    test_load_default_config();
    test_two_loads_give_same_result();
    printf("All config tests passed.\n\n");
    return 0;
}
