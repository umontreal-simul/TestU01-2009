#!/bin/bash
# Kill/resume run: the SAME battery (BigCrush, mode hi, e*=256, pgprng V2.3)
# and the SAME shared genesis as run_control.sh (run prepare_shared_genesis.sh
# first), but this one gets SIGKILLed -- the whole process tree, simulating a
# real crash -- repeatedly, and is re-launched with the identical command
# each time, exactly as the PR's "How to reproduce" section describes. Each
# resume restarts the test that was cut off from its beginning, on the same
# stream words as before, so the final p-values must be bit-for-bit identical
# to the control run's. python3 compare_runs.py (same folder) checks that,
# once both runs have finished.
#
# Uses your real pgprng_2.3/wsl_build_testu01_ckpt.sh and generator in
# place -- nothing here is a copy.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PGPRNG_ROOT="$(cd "$HERE/../.." && pwd)"
E_STAR="${E_STAR:-256}"
NAME_TAG="pgprng_V2.3"    # fixed label used in file/dir names below (not E_STAR).
                          # e* itself is recorded inside the genesis JSON's e1/e2
                          # fields, so it's recoverable from the file, not the name.
export WORK_DIR="${KILL_WORK_DIR:-$HOME/work/testu01_ckpt_killresume_${NAME_TAG}}"
export RESULTS_DIR="$HERE/kill_resume_results"
export GENESIS_FILE="$WORK_DIR/run/genesis_testu01_ckpt_${NAME_TAG}.json"   # the SHARED genesis
KILL_INTERVAL="${KILL_INTERVAL:-600}"   # seconds between kills, default 10 min
MAX_ATTEMPTS="${MAX_ATTEMPTS:-2000}"   # safety cap -- raised 2026-09-30 when this
                                          # switched from Crush (a few hours) to BigCrush
                                          # (can run well over a day): at the default 600s
                                          # kill interval, 100 attempts would only cover
                                          # ~16.7 hours and could stop the run before the
                                          # battery finishes. 2000 attempts covers ~2 weeks
                                          # at this interval -- effectively no practical cap.

if [ -d "$RESULTS_DIR" ] && [ -n "$(ls -A "$RESULTS_DIR" 2>/dev/null)" ]; then
    echo "WARNING: $RESULTS_DIR already has content from an earlier run."
    echo "Starting this again without clearing it out first will APPEND to the .err log"
    echo "and can make it look like units were confirmed twice. Ctrl-C now and clear it"
    echo "out first (or confirm you really mean to continue) if that's not what you want."
    read -r -p "Continue anyway? [y/N] " REPLY
    case "$REPLY" in
        [Yy]*) ;;
        *) echo "Aborted."; exit 1 ;;
    esac
fi

GENESIS="$WORK_DIR/run/genesis_testu01_ckpt_${NAME_TAG}.json"
if [ ! -f "$GENESIS" ]; then
    echo "No shared genesis found at $GENESIS"
    echo "Run prepare_shared_genesis.sh first."
    exit 1
fi

mkdir -p "$RESULTS_DIR"
OUT="$RESULTS_DIR/bigcrush_hi_e${E_STAR}_ckpt.txt"     # names are set by wsl_build_testu01_ckpt.sh
ERR="$RESULTS_DIR/bigcrush_hi_e${E_STAR}_ckpt.err"
LOG="$RESULTS_DIR/kill_resume_transcript.log"
echo "=== kill/resume run -- battery=bigcrush mode=hi e*=$E_STAR ===" > "$LOG"
echo "WORK_DIR=$WORK_DIR  RESULTS_DIR=$RESULTS_DIR" >> "$LOG"
echo "kill interval: every ${KILL_INTERVAL}s, whole process tree, up to $MAX_ATTEMPTS attempts" >> "$LOG"
log() { echo "[$(date -u '+%Y-%m-%d %H:%M:%S UTC')] $*" | tee -a "$LOG"; }

MAX_KILL_INTERVAL="${MAX_KILL_INTERVAL:-14400}"   # cap (4 h) on the adaptive interval below
POLL="${POLL:-5}"                                  # seconds between "is it still running?" checks
count_confirmed() { local n; n=$(grep -c TESTU01_CKPT_CONFIRMED "$ERR" 2>/dev/null); echo "${n:-0}"; }

# Adaptive kill interval (added 2026-09-30): a sub-test is only saved when it
# FINISHES. If one sub-test takes longer than the kill interval, killing on a
# fixed schedule would restart it from scratch every time and the run would
# never get past it. So: if an attempt was killed without finishing a single
# new sub-test, double the interval for the next attempt (up to the cap); after
# an attempt that did make progress, go back to the base interval.
ATTEMPT=0
INTERVAL="$KILL_INTERVAL"
COMPLETE=0
while [ "$ATTEMPT" -lt "$MAX_ATTEMPTS" ]; do
    ATTEMPT=$((ATTEMPT+1))
    before=$(count_confirmed)
    log "--- attempt $ATTEMPT: launching 'bash wsl_build_testu01_ckpt.sh bigcrush hi' (fresh process group; kill interval ${INTERVAL}s; $before sub-test(s) confirmed so far) ---"
    set -m
    ( bash "$PGPRNG_ROOT/wsl_build_testu01_ckpt.sh" bigcrush hi >>"$LOG" 2>&1 ) &
    pgid=$!
    set +m

    # Wait up to INTERVAL seconds, polling, so we notice if the script
    # finishes on its own (battery complete) before the kill is due.
    elapsed=0
    while [ "$elapsed" -lt "$INTERVAL" ]; do
        if ! kill -0 "$pgid" 2>/dev/null; then
            break
        fi
        sleep "$POLL"
        elapsed=$((elapsed+POLL))
    done

    if ! kill -0 "$pgid" 2>/dev/null; then
        wait "$pgid" 2>/dev/null
        status=$?
        log "attempt $ATTEMPT: process finished on its own (exit $status) before the next scheduled kill"
        if [ "$status" -eq 0 ] && grep -q "Summary results of BigCrush" "$OUT" 2>/dev/null; then
            COMPLETE=1
            log "battery complete -- see $OUT"
            break
        else
            log "process exited but the final report is not there -- will re-launch (resume) anyway"
            continue
        fi
    fi

    log "attempt $ATTEMPT: sending SIGKILL to process group $pgid (simulated crash, after ~${elapsed}s)"
    kill -KILL -- -"$pgid" 2>>"$LOG"
    wait "$pgid" 2>/dev/null
    after=$(count_confirmed)
    if [ "$after" -gt "$before" ]; then
        INTERVAL="$KILL_INTERVAL"
        log "attempt $ATTEMPT: killed; $((after-before)) new sub-test(s) were confirmed during it ($after total). Re-launching identical command to resume."
    else
        INTERVAL=$((INTERVAL*2))
        [ "$INTERVAL" -gt "$MAX_KILL_INTERVAL" ] && INTERVAL="$MAX_KILL_INTERVAL"
        log "attempt $ATTEMPT: killed; NO new sub-test finished during it. Next kill interval: ${INTERVAL}s. Re-launching identical command to resume."
    fi
done

if [ "$COMPLETE" -eq 1 ]; then
    log "=== DONE: battery complete after $ATTEMPT attempt(s) (kills + resumes), see $OUT ==="
else
    log "=== STOPPED after $ATTEMPT attempt(s) without a confirmed-complete result -- check $OUT and $LOG ==="
fi

# Preserve the starting state and final checkpoints alongside the results --
# same as run_control.sh, so both runs' evidence packages are self-contained
# and directly comparable (same genesis, final driver checkpoints side by
# side).
EVIDENCE_DIR="$RESULTS_DIR/evidence_state"
mkdir -p "$EVIDENCE_DIR"
cp -f "$WORK_DIR/run/genesis_testu01_ckpt_${NAME_TAG}.json" "$EVIDENCE_DIR/" 2>/dev/null
cp -f "$WORK_DIR/run/driver_bigcrush_hi.ckpt" "$EVIDENCE_DIR/" 2>/dev/null
cp -f "$WORK_DIR/run/gen_bigcrush_hi.ckpt" "$EVIDENCE_DIR/" 2>/dev/null
log "Evidence state (genesis + final checkpoints) saved to: $EVIDENCE_DIR"

log "Transcript: $LOG"
