#!/bin/bash

BIN=./build/furnishing
RUNS=20
PASS=0
FAIL=0
TIMEOUTS=0
CRASHES=0

cat > /tmp/cfg_stress.txt << 'CFG'
team_size      = 4
num_pieces     = 15
min_pause_ms   = 1
max_pause_ms   = 10
fatigue_factor = 1.0
fatigue_cap_ms = 20
wins_to_match  = 2
gui_enabled    = 0
verbose        = 0
CFG

echo "--- Stress Tests ---"
echo "Running $RUNS back-to-back matches..."
echo ""

for i in $(seq 1 $RUNS); do
    result=$(timeout 30 "$BIN" /tmp/cfg_stress.txt 2>/dev/null)
    exit_code=$?

    if [ $exit_code -eq 124 ]; then
        echo "  Run $i: FAIL (timeout — possible deadlock)"
        TIMEOUTS=$((TIMEOUTS + 1))
        FAIL=$((FAIL + 1))

    elif [ $exit_code -ne 0 ] && ! echo "$result" | grep -q "WINS THE COMPETITION"; then
        echo "  Run $i: FAIL (crashed — exit code $exit_code)"
        CRASHES=$((CRASHES + 1))
        FAIL=$((FAIL + 1))

    elif echo "$result" | grep -q "WINS THE COMPETITION"; then
        echo "  Run $i: PASS"
        PASS=$((PASS + 1))

    else
        echo "  Run $i: FAIL (no winner in output)"
        echo "           last line: $(echo "$result" | tail -1)"
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "-----------------------------"
echo "Stress results : $PASS / $RUNS passed"
echo "Timeouts       : $TIMEOUTS"
echo "Crashes        : $CRASHES"
echo "Other failures : $((FAIL - TIMEOUTS - CRASHES))"
echo "-----------------------------"

# check for core dumps
if ls core* 2>/dev/null | grep -q core; then
    echo ""
    echo "WARNING: core dump(s) found:"
    ls core*
    echo "Run: gdb ./build/furnishing core"
    echo "Then type: bt"
    echo "to see exactly where the crash happened."
fi

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
