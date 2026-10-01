#!/bin/bash
# reset_evidence_runs.sh -- stop any running evidence runs and clear their
# results and checkpoints, so run_control.sh and run_kill_resume.sh both start
# again from word 0 of the SAME shared genesis.
#
# Use this when the evidence runs must be restarted (e.g. after the generator's
# checkpoint logic changed, as on 2026-09-30). Nothing is deleted: old results
# and old checkpoints are MOVED to ~/work/old_evidence_runs_<timestamp>/.
# The shared genesis file and the already-built TestU01/driver are kept.
#
# Usage:   bash reset_evidence_runs.sh          (asks before stopping anything)
#          bash reset_evidence_runs.sh --yes    (no question)
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME_TAG="pgprng_V2.3"
CTRL_WORK="${CTRL_WORK_DIR:-$HOME/work/testu01_ckpt_ctrl_${NAME_TAG}}"
KILL_WORK="${KILL_WORK_DIR:-$HOME/work/testu01_ckpt_killresume_${NAME_TAG}}"
GEN_NAME="genesis_testu01_ckpt_${NAME_TAG}.json"
PAT="${RESET_PATTERN:-run_control\.sh|run_kill_resume\.sh|wsl_build_testu01_ckpt\.sh|testu01-sync|pgprng_testu01_ckpt_driver}"
# list / kill by PID, never touching this script or the shell that started it
list_procs() { pgrep -af "$1" | awk -v me="$$" -v par="$PPID" '$1!=me && $1!=par'; }
kill_procs() { local p; for p in $(list_procs "$1" | awk '{print $1}'); do kill -KILL "$p" 2>/dev/null; done; }
STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP="${BACKUP_DIR:-$HOME/work/old_evidence_runs_$STAMP}"

echo "=== 1. genesis check ==="
for w in "$CTRL_WORK" "$KILL_WORK"; do
    if [ ! -f "$w/run/$GEN_NAME" ]; then
        echo "ERROR: shared genesis missing: $w/run/$GEN_NAME"
        echo "Nothing was changed. (If the genesis was lost, run prepare_shared_genesis.sh on clean work dirs.)"
        exit 1
    fi
done
A=$(sha256sum "$CTRL_WORK/run/$GEN_NAME" | cut -d' ' -f1)
B=$(sha256sum "$KILL_WORK/run/$GEN_NAME" | cut -d' ' -f1)
if [ "$A" != "$B" ]; then
    echo "ERROR: the two genesis files differ -- the runs are not a matched pair."
    echo "  control:     $A"; echo "  kill/resume: $B"
    echo "Nothing was changed."
    exit 1
fi
echo "genesis files identical (sha256 ${A:0:16}...)"

echo
echo "=== 2. running evidence jobs ==="
PROCS="$(list_procs "$PAT")"
if [ -n "$PROCS" ]; then
    echo "$PROCS" | cut -c1-150
    if [ "${1:-}" != "--yes" ]; then
        read -r -p "Stop the process(es) above? [y/N] " R
        case "$R" in [Yy]*) ;; *) echo "Aborted; nothing changed."; exit 1 ;; esac
    fi
    # wrappers first, so nothing re-launches the pipeline behind our back
    kill_procs "${RESET_WRAPPERS:-run_kill_resume\.sh|run_control\.sh|wsl_build_testu01_ckpt\.sh}"
    kill_procs "$PAT"
    sleep 2
    LEFT="$(list_procs "$PAT")"
    if [ -n "$LEFT" ]; then
        echo "ERROR: still running after kill:"; echo "$LEFT" | cut -c1-150
        exit 1
    fi
    echo "stopped."
else
    echo "none running."
fi

echo
echo "=== 3. moving old results and checkpoints to $BACKUP ==="
mkdir -p "$BACKUP"
for d in control_results kill_resume_results; do
    if [ -d "$HERE/$d" ]; then mv "$HERE/$d" "$BACKUP/$d" && echo "moved $d"; fi
done
for f in comparison_report.txt; do
    [ -f "$HERE/$f" ] && mv "$HERE/$f" "$BACKUP/$f" && echo "moved $f"
done
for pair in "control:$CTRL_WORK" "killresume:$KILL_WORK"; do
    tag="${pair%%:*}"; w="${pair#*:}"
    mkdir -p "$BACKUP/work_$tag"
    for f in "$w"/run/*; do
        [ -e "$f" ] || continue
        [ "$(basename "$f")" = "$GEN_NAME" ] && continue
        mv "$f" "$BACKUP/work_$tag/" && echo "moved $tag: $(basename "$f")"
    done
done

echo
echo "=== 4. result ==="
echo "Both runs are reset. Shared genesis kept in both work dirs:"
echo "  $CTRL_WORK/run/$GEN_NAME"
echo "  $KILL_WORK/run/$GEN_NAME"
echo "Old material is in: $BACKUP"
echo "Next: start both runs again (bash run_control.sh and bash run_kill_resume.sh)."
