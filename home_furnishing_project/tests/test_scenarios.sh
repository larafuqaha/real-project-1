#!/bin/bash

BIN=./build/furnishing
PASS=0
FAIL=0

run_test() {
    local name="$1"
    local cfg="$2"
    local expect="$3"
    local timeout_sec="$4"

    output=$(timeout "$timeout_sec" "$BIN" "$cfg" 2>/dev/null)
    if echo "$output" | grep -q "$expect"; then
        echo "PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        echo "      expected to find: '$expect'"
        echo "      last output:      $(echo "$output" | tail -2)"
        FAIL=$((FAIL + 1))
    fi
}

echo "--- Scenario Tests ---"

# ---- basic completion ----
run_test "small config completes"        config/test_small.txt  "WINS THE COMPETITION" 30
run_test "winner is team A or B"         config/test_small.txt  "TEAM [AB] WINS"        30

# ---- minimum team size (2 members: source is also sink) ----
cat > /tmp/cfg_min_team.txt << 'CFG'
team_size      = 2
num_pieces     = 5
min_pause_ms   = 1
max_pause_ms   = 5
fatigue_factor = 1.0
fatigue_cap_ms = 10
wins_to_match  = 1
gui_enabled    = 0
verbose        = 1
CFG
run_test "minimum team size (2)" /tmp/cfg_min_team.txt "WINS THE COMPETITION" 20

# ---- large team ----
cat > /tmp/cfg_large_team.txt << 'CFG'
team_size      = 8
num_pieces     = 10
min_pause_ms   = 1
max_pause_ms   = 5
fatigue_factor = 1.0
fatigue_cap_ms = 10
wins_to_match  = 1
gui_enabled    = 0
verbose        = 1
CFG
run_test "large team (8 members)" /tmp/cfg_large_team.txt "WINS THE COMPETITION" 30

# ---- single piece per round ----
cat > /tmp/cfg_one_piece.txt << 'CFG'
team_size      = 4
num_pieces     = 1
min_pause_ms   = 1
max_pause_ms   = 5
fatigue_factor = 1.0
fatigue_cap_ms = 10
wins_to_match  = 1
gui_enabled    = 0
verbose        = 1
CFG
run_test "single piece per round" /tmp/cfg_one_piece.txt "WINS THE COMPETITION" 15

# ---- multi-round match ----
cat > /tmp/cfg_multi_round.txt << 'CFG'
team_size      = 4
num_pieces     = 10
min_pause_ms   = 1
max_pause_ms   = 5
fatigue_factor = 1.0
fatigue_cap_ms = 10
wins_to_match  = 3
gui_enabled    = 0
verbose        = 1
CFG
run_test "multi-round match (3 wins)" /tmp/cfg_multi_round.txt "WINS THE COMPETITION" 60

# ---- 2-in-flight verification ----
# fwd=1 bwd=1 must appear at least once in the trace
flt_output=$(FURN_TRACE=1 timeout 20 "$BIN" config/test_small.txt 2>&1)
if echo "$flt_output" | grep -q "fwd=1 bwd=1"; then
    echo "PASS: 2-in-flight pipeline observed (fwd=1 bwd=1 in trace)"
    PASS=$((PASS + 1))
else
    echo "FAIL: 2-in-flight never observed — pipeline not working"
    FAIL=$((FAIL + 1))
fi

# ---- no core dumps left behind ----
if ls core* 2>/dev/null | grep -q core; then
    echo "FAIL: core dump found — a segfault occurred"
    FAIL=$((FAIL + 1))
else
    echo "PASS: no core dumps"
    PASS=$((PASS + 1))
fi

echo ""
echo "Scenario results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
