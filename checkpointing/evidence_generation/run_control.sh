#!/bin/bash
# Control run: BigCrush, mode hi, e*=256, pgprng V2.3, run straight through
# with NO interruption. It starts from the shared genesis made by
# prepare_shared_genesis.sh (run that first). run_kill_resume.sh runs the same
# battery from the same genesis but gets SIGKILLed and resumed many times.
# Because the resumed generator is put back at exactly the stream word where
# the last finished test ended, the two runs must produce bit-for-bit
# identical p-values. python3 compare_runs.py (same folder) checks that, once
# both runs have finished.
#
# Uses your real pgprng_2.3/wsl_build_testu01_ckpt.sh and generator in
# place -- nothing here is a copy.
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PGPRNG_ROOT="$(cd "$HERE/../.." && pwd)"
E_STAR="${E_STAR:-256}"
NAME_TAG="pgprng_V2.3"    # fixed label used in file/dir names below (not E_STAR).
                          # e* itself is recorded inside the genesis JSON's e1/e2
                          # fields, so it's recoverable from the file, not the name.
export WORK_DIR="${CTRL_WORK_DIR:-$HOME/work/testu01_ckpt_ctrl_${NAME_TAG}}"
export RESULTS_DIR="$HERE/control_results"
export GENESIS_FILE="$WORK_DIR/run/genesis_testu01_ckpt_${NAME_TAG}.json"   # the SHARED genesis

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

if [ ! -f "$WORK_DIR/run/genesis_testu01_ckpt_${NAME_TAG}.json" ]; then
    echo "No shared genesis found at $WORK_DIR/run/genesis_testu01_ckpt_${NAME_TAG}.json"
    echo "Run prepare_shared_genesis.sh first."
    exit 1
fi

mkdir -p "$RESULTS_DIR"
LOG="$RESULTS_DIR/control_run_transcript.log"
echo "=== control run (no interruption) -- battery=bigcrush mode=hi e*=$E_STAR ===" | tee "$LOG"
echo "WORK_DIR=$WORK_DIR  RESULTS_DIR=$RESULTS_DIR" | tee -a "$LOG"
echo "[$(date -u '+%Y-%m-%d %H:%M:%S UTC')] starting" | tee -a "$LOG"

bash "$PGPRNG_ROOT/wsl_build_testu01_ckpt.sh" bigcrush hi 2>&1 | tee -a "$LOG"

# Preserve the starting state and final checkpoints alongside the results --
# small files, and they're the actual evidence that both runs (this one and
# kill_resume) started from the identical genesis. Copied, not moved: the
# originals stay in place under WORK_DIR for any further resume.
EVIDENCE_DIR="$RESULTS_DIR/evidence_state"
mkdir -p "$EVIDENCE_DIR"
cp -f "$WORK_DIR/run/genesis_testu01_ckpt_${NAME_TAG}.json" "$EVIDENCE_DIR/" 2>/dev/null
cp -f "$WORK_DIR/run/driver_bigcrush_hi.ckpt" "$EVIDENCE_DIR/" 2>/dev/null
cp -f "$WORK_DIR/run/gen_bigcrush_hi.ckpt" "$EVIDENCE_DIR/" 2>/dev/null
echo "Evidence state (genesis + final checkpoints) saved to: $EVIDENCE_DIR" | tee -a "$LOG"

echo "[$(date -u '+%Y-%m-%d %H:%M:%S UTC')] finished" | tee -a "$LOG"
echo "Result: $RESULTS_DIR/bigcrush_hi_e${E_STAR}_ckpt.txt" | tee -a "$LOG"
echo "Transcript: $LOG"
