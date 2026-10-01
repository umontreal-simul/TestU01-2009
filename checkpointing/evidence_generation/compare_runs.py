#!/usr/bin/env python3
"""
compare_runs.py -- analyze a matched pair of checkpointed TestU01 runs:
a CONTROL run (never interrupted) and a KILL/RESUME run (repeatedly SIGKILLed
and resumed), both started from the SAME generator genesis state.

Usage (from the evidence_generation folder, in WSL):
    python3 compare_runs.py                      # default folders
    python3 compare_runs.py CONTROL_DIR KILL_RESUME_DIR

What it checks, and why (plain language)
----------------------------------------
Checkpoint/resume is correct only if an interrupted-and-resumed run tests
EXACTLY the same random stream, and so produces EXACTLY the same results, as
an uninterrupted one. The resumed generator is repositioned at the precise
stream word where the last finished test ended, and the test that was cut off
starts over from its beginning on that same word. So, with both runs started
from the same genesis, every result must match bit for bit:

  1. Both runs finished and produced the full report.
  2. Every sub-test was confirmed EXACTLY ONCE in each run: none repeated,
     none missing.
  3. The stream position at the end of every sub-test is identical in the two
     runs (the kill/resume run consumed the same words in the same order;
     nothing skipped, nothing replayed).
  4. Both runs tested the same sub-tests, in the same order, with the same
     number of statistics.
  5. Every p-value of every sub-test is bit-for-bit identical between the runs.
  6. All p-values are inside TestU01's pass window (0.001 .. 0.999);
     anything outside is listed.

Any difference in 3 or 5 is a checkpointing failure and is reported as FAIL.

Drafted by Claude (Anthropic) for Aly Graham, 2026-09-30 (revised for the
exact-resume design the same day).
"""
import os
import re
import sys

PREFIX = "bigcrush_hi_e256_ckpt"          # file-name stem set by wsl_build_testu01_ckpt.sh
CKPT_NAME = "driver_bigcrush_hi.ckpt"     # copied into evidence_state/ at the end of each run
SUSPECT = 0.001
# default work dirs used by prepare_shared_genesis.sh / run_*.sh (only used to peek at
# progress while a run is still going)
LIVE_WORK_DIRS = {
    "control_results": ["~/work/testu01_ckpt_ctrl_pgprng_V2.3"],
    "kill_resume_results": ["~/work/testu01_ckpt_killresume_pgprng_V2.3"],
}

here = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("-")]
ctrl_dir = args[0] if len(args) > 0 else os.path.join(here, "control_results")
kr_dir = args[1] if len(args) > 1 else os.path.join(here, "kill_resume_results")

lines_out = []
fails = 0


def say(s=""):
    print(s)
    lines_out.append(s)


def check(ok, label, detail=""):
    global fails
    tag = "PASS" if ok else "FAIL"
    if not ok:
        fails += 1
    say(f"[{tag}] {label}" + (f" -- {detail}" if detail else ""))


def info(label, detail=""):
    say(f"[INFO] {label}" + (f" -- {detail}" if detail else ""))


def read(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError:
        return None


RE_CONF = re.compile(r"TESTU01_CKPT_CONFIRMED battery=\S+ mode=\S+ j2=(\d+) words_consumed=(\d+)")
RE_RESUMED = re.compile(r"RESUMED from .*?: (\d+) unit\(s\) already done")
RE_TOTAL = re.compile(r"consumed (\d+) 64-bit words total")


def parse_err(text):
    """Return (events, total_words). events = ordered list of ('C', j2, words) / ('R', units_done)."""
    events = []
    total = None
    for line in (text or "").splitlines():
        m = RE_CONF.search(line)
        if m:
            events.append(("C", int(m.group(1)), int(m.group(2))))
            continue
        m = RE_RESUMED.search(line)
        if m:
            events.append(("R", int(m.group(1)), 0))
            continue
        m = RE_TOTAL.search(line)
        if m:
            total = int(m.group(1))
    return events, total


def parse_ckpt(text):
    """Return {j2: [(idx, value, name), ...]} from a driver checkpoint file."""
    units = {}
    for line in (text or "").splitlines():
        if not line.startswith("PVAL "):
            continue
        parts = line.split(None, 4)
        if len(parts) < 5:
            continue
        j2, idx, val, name = int(parts[1]), int(parts[2]), float(parts[3]), parts[4]
        units.setdefault(j2, []).append((idx, val, name))
    for j2 in units:
        units[j2].sort()
    return units


def load_run(d):
    err = read(os.path.join(d, PREFIX + ".err"))
    txt = read(os.path.join(d, PREFIX + ".txt"))
    ck = read(os.path.join(d, "evidence_state", CKPT_NAME))
    live = False
    if ck is None:
        # Run still in progress: fall back to the live driver checkpoint in the
        # run's work directory, so progress can be compared before the end.
        for w in LIVE_WORK_DIRS.get(os.path.basename(os.path.normpath(d)), []):
            ck = read(os.path.join(os.path.expanduser(w), "run", CKPT_NAME))
            if ck is not None:
                live = True
                break
    tr = None
    for n in os.listdir(d) if os.path.isdir(d) else []:
        if n.endswith("_transcript.log"):
            tr = read(os.path.join(d, n))
    events, total = parse_err(err)
    return {"dir": d, "err": err, "txt": txt, "ckpt": parse_ckpt(ck) if ck else None,
            "events": events, "total_words": total, "transcript": tr, "live_ckpt": live}


say("=" * 72)
say("Matched-pair analysis: control vs kill/resume")
say("Drafted by Claude (Anthropic) for Aly Graham")
say("=" * 72)
say(f"control:      {ctrl_dir}")
say(f"kill/resume:  {kr_dir}")
say()

C = load_run(ctrl_dir)
K = load_run(kr_dir)
for name, r in (("control", C), ("kill/resume", K)):
    if r["err"] is None:
        check(False, f"{name}: progress log found", f"missing {PREFIX}.err in {r['dir']}")
if C["err"] is None or K["err"] is None:
    say("\nCannot continue without both progress logs.")
    sys.exit(2)

# ---- 1. completion --------------------------------------------------------
say("-- 1. Completion")
done = {}
for name, r in (("control", C), ("kill/resume", K)):
    s = r["txt"] or ""
    done[name] = "Summary results of BigCrush" in s
    m = re.search(r"Number of statistics:\s+(\d+)", s)
    r["nstats"] = int(m.group(1)) if m else None
    r["all_passed"] = "All tests were passed" in s
    r["all_other"] = "All other tests were passed" in s
    confirmed = [e for e in r["events"] if e[0] == "C"]
    info(f"{name}: {len(confirmed)} sub-test confirmations logged so far")
complete = all(done.values())
if complete:
    check(True, "both runs finished (final report present)")
else:
    info("run status", "control %s, kill/resume %s" % (
        "finished" if done["control"] else "still running",
        "finished" if done["kill/resume"] else "still running"))
if not complete:
    say("\nOne or both runs are still in progress; the checks below use what is logged so far.")
    say("Re-run this script after both finish for the full analysis.")
say()

# ---- 2. each unit exactly once, stream moves only forward ---------------
say("-- 2. Each sub-test confirmed exactly once, in order; stream position only moves forward")
for name, r in (("control", C), ("kill/resume", K)):
    conf = [(j, w) for (t, j, w) in r["events"] if t == "C"]
    ids = [j for j, _ in conf]
    dup = sorted({j for j in ids if ids.count(j) > 1})
    check(not dup, f"{name}: no sub-test confirmed twice (no replay)",
          f"repeated unit id(s): {dup}" if dup else f"{len(ids)} unique confirmations")
    asc = all(ids[i] < ids[i + 1] for i in range(len(ids) - 1))
    check(asc, f"{name}: sub-tests confirmed in increasing order")
    words = [w for _, w in conf]
    mono = all(words[i] < words[i + 1] for i in range(len(words) - 1))
    check(mono, f"{name}: words consumed strictly increasing (no rewind)")
    r["ids"] = ids
say()

# ---- 3/4/5/6. need the driver checkpoints ---------------------------------
say("-- 3. Stream position at the end of every sub-test")
pos = {}
for name, r in (("control", C), ("kill/resume", K)):
    d = {}
    for (t, j, w) in r["events"]:
        if t == "C" and j not in d:
            d[j] = w
    pos[name] = d
common = sorted(set(pos["control"]) & set(pos["kill/resume"]))
diffp = [j for j in common if pos["control"][j] != pos["kill/resume"][j]]
check(not diffp, f"{len(common)} sub-test(s) logged by both runs end at the identical stream word",
      "; ".join(f"unit {j}: control {pos['control'][j]:,} vs kill/resume {pos['kill/resume'][j]:,}"
                for j in diffp[:5]) if diffp else "")
only_c = sorted(set(pos["control"]) - set(pos["kill/resume"]))
only_k = sorted(set(pos["kill/resume"]) - set(pos["control"]))
if complete and (only_c or only_k):
    info("unit(s) confirmed in only one run's log (the other run's checkpoint is checked below)",
         f"only control: {only_c[:10]}, only kill/resume: {only_k[:10]}")
say()

say("-- 4/5. Same tests, same order, bit-identical p-values")
if complete:
    check(C["nstats"] == K["nstats"], "number of statistics equal",
          f"control={C['nstats']} kill/resume={K['nstats']}")
if C["ckpt"] is None or K["ckpt"] is None:
    info("driver checkpoints not found for one or both runs",
         "(evidence_state/%s appears when a run finishes; per-p-value checks skipped)" % CKPT_NAME)
else:
    for name, r in (("control", C), ("kill/resume", K)):
        if r["live_ckpt"]:
            info(f"{name}: using the LIVE checkpoint from the work directory (run in progress)")
    units_both = sorted(set(C["ckpt"]) & set(K["ckpt"]))
    if complete:
        check(set(C["ckpt"]) == set(K["ckpt"]), "both runs recorded the same set of sub-tests",
              f"only control: {sorted(set(C['ckpt']) - set(K['ckpt']))[:10]}, "
              f"only kill/resume: {sorted(set(K['ckpt']) - set(C['ckpt']))[:10]}"
              if set(C["ckpt"]) != set(K["ckpt"]) else f"{len(C['ckpt'])} sub-tests")
    else:
        info(f"sub-tests finished in both runs so far: {len(units_both)}")
    names_ok = all([n for (_, _, n) in C["ckpt"][j]] == [n for (_, _, n) in K["ckpt"][j]]
                   for j in units_both)
    check(names_ok, "test names identical, in the same order")
    bad = [j for j in units_both if C["ckpt"][j] != K["ckpt"][j]]
    nvals = sum(len(C["ckpt"][j]) for j in units_both)
    check(not bad, f"all {nvals} p-value(s) in the {len(units_both)} common sub-test(s) are bit-for-bit identical",
          "differing unit id(s): %s" % bad[:10] if bad else "")
    for j in bad[:5]:
        for a_, b_ in zip(C["ckpt"][j], K["ckpt"][j]):
            if a_ != b_:
                info(f"  unit {j} {a_[2]}", f"control {a_[1]!r} vs kill/resume {b_[1]!r}")
    say()

    # ---- 6. all p-values in the pass window -------------------------------
    say("-- 6. p-values inside the pass window")
    for name, r in (("control", C), ("kill/resume", K)):
        flagged = [(j, n, v) for j in sorted(r["ckpt"]) for (_, v, n) in r["ckpt"][j]
                   if v < SUSPECT or v > 1 - SUSPECT]
        allv = [v for j in r["ckpt"] for (_, v, _) in r["ckpt"][j]]
        check(not flagged, f"{name}: all {len(allv)} statistics within [{SUSPECT}, {1 - SUSPECT}]",
              "; ".join(f"unit {j} {n}: {v:.6g}" for j, n, v in flagged) if flagged else "")
    say()

# ---- interruption summary -------------------------------------------------
say("-- Interruption summary (kill/resume run)")
resumes = sum(1 for e in K["events"] if e[0] == "R")
kills = len(re.findall(r"sending SIGKILL", K["transcript"] or ""))
info(f"SIGKILLs sent: {kills}; resumes logged by the driver: {resumes}")
info(f"control resumes (should be 0): {sum(1 for e in C['events'] if e[0] == 'R')}")
check(sum(1 for e in C["events"] if e[0] == "R") == 0, "control run was never interrupted")
if complete:
    check(resumes > 0, "kill/resume run was actually interrupted and resumed at least once")
else:
    info("kill/resume run so far", f"{resumes} resume(s) logged")
for name, r in (("control", C), ("kill/resume", K)):
    if r["total_words"] is not None:
        info(f"{name}: driver's final word count", f"{r['total_words']:,}")
if complete and C["total_words"] is not None and K["total_words"] is not None:
    check(C["total_words"] == K["total_words"], "total words consumed is identical in both runs")
say()
say("=" * 72)
if fails:
    say(f"RESULT: {fails} CHECK(S) FAILED")
elif not complete:
    say("RESULT: no problems so far -- but the runs are not finished, so this is NOT the final verdict")
else:
    say("RESULT: ALL CHECKS PASSED")
say("=" * 72)

outp = os.path.join(here, "comparison_report.txt")
try:
    with open(outp, "w") as f:
        f.write("\n".join(lines_out) + "\n")
    print(f"\n(report also saved to {outp})")
except OSError as e:
    print(f"\n(could not save report: {e})")
sys.exit(1 if fails else 0)
