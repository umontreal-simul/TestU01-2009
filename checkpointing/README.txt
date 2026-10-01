Checkpointed Crush/BigCrush driver for TestU01-2009

Author: Aly Graham. Developed with the assistance of Claude (Anthropic); see “Test environment and
methodology”.

Why

bbattery_Crush() and bbattery_BigCrush() are single long-running calls with no resume support. Crush
takes roughly 1-2 hours per stream and BigCrush several hours on ordinary hardware; any interruption
(crash, kill, reboot, a laptop going to sleep) loses all progress and the battery starts over from
zero.

This pull request adds an optional checkpointed driver. It saves progress after every individual
sub-test (“unit”) finishes, so an interrupted run resumes at the first unfinished unit instead of
starting over. A resumed run tests exactly the same random stream as an uninterrupted run and
produces exactly the same p-values (see “Evidence”). The mechanism is generic: any generator that
can be piped as raw 64-bit words on stdin and repositioned to a given word can use it. The worked
example uses the author’s own generator, pgprng V2.3.

What is included

Everything new is in the checkpointing/ directory; paths below are relative to it. The two source
patches are also applied directly to the source tree on this branch (commits 1 and 2), so they
appear as ordinary diffs; the .patch files are kept for reference.

-   patches/gcc15_c23_kr_modernization.patch - converts a handful of pre-C99 K&R-style function
    declarations (ucryptoIS.c, uknuth.c) to standard prototypes, so the source builds under GCC 15’s
    stricter defaults. No logic changed.
-   patches/testu01_checkpointing_visibility.patch - removes exactly six static keywords from
    bbattery.c (TestNumber, GetName, WriteReport, InitBat, GetPVal_Walk, GetPVal_CPairs) so an
    external driver can use the same helpers bbattery_Crush() and bbattery_BigCrush() use, producing
    output in the stock format. No other line is touched.
-   src/bbattery_ckpt_internal.h - declares those six now-visible symbols.
-   src/pgprng_testu01_ckpt_driver.c - the checkpointed driver. It was produced mechanically: the
    ORIGINAL, unmodified bbattery.c was split at every ++j2; unit boundary (the split is verified
    lossless: the pieces, rejoined, reproduce the original text byte for byte) and each unit is
    wrapped in a generic skip / run / commit shell. No test parameter, sample count or name string
    was retyped by hand. It runs exactly the stock batteries: 96 tests for Crush and 106 tests (160
    statistics) for BigCrush. (The stock source also contains an #ifdef USE_LONGLONG branch with
    more tests; it is not compiled by default, which is why the stock counts are 96 and 106. See the
    TEST COUNT comment at the top of the driver.)
-   build_driver.sh - builds TestU01 from this repository and compiles the driver
    (bash checkpointing/build_driver.sh [BUILD_DIR]). Use this with any generator that writes raw
    64-bit words to stdin.
-   evidence_generation/ - the scripts that launched the two evidence runs (run_control.sh,
    run_kill_resume.sh, compare_runs.py, reset_evidence_runs.sh), kept as a record of how the runs
    were made, and the raw results of the two runs (control_results/, kill_resume_results/) with the
    printed analysis (comparison_report.txt). The author’s pgprng harness and the script that
    created the shared genesis file are not included, because they depend on the pgprng producer,
    which is not published.

Checkpoint design

The driver marks a unit finished only after the unit’s real test call has returned. It then writes
the checkpoint (write a temporary file, fsync, rename, so the file is always a complete old or new
version) and prints a confirmation line (TESTU01_CKPT_CONFIRMED ...), all before the next unit
starts. The checkpoint holds every finished unit’s p-values and test names, and words_consumed, the
exact number of 64-bit words read so far.

On restart the driver skips finished units and replays their saved p-values, so the final report is
complete. The first unfinished unit runs again from its beginning.

For a resumed run to be identical to an uninterrupted one, the word stream must continue at exactly
words_consumed. The driver cannot do that alone, because the generator is a separate process. The
pgprng producer
(pgprng_v2_stream.py --checkpoint-mode testu01-sync --driver-ckpt <driver checkpoint>) reads
words_consumed from the driver’s checkpoint, restores its own last saved state (never ahead of that
word) and fast-forwards, discarding output, to that exact word before sending anything. Nothing is
skipped and nothing is replayed, and the unit that was cut off re-runs on the same words it saw the
first time. Any other producer that can be repositioned to a given word (jump-ahead, or a restart
from its seed with the first words_consumed words discarded) gets the same result.

Evidence

Two BigCrush runs (mode hi, which feeds the battery the high 32 bits of each 64-bit word) were made
against pgprng V2.3 from ONE shared starting state: a single genesis file copied into both work
directories, with the SHA-256 of the two copies checked to match before starting.

A genesis file is a checkpoint file that is created when pgprng execution is started. Starting both
runs from byte-identical copies of the same genesis file guarantees that they begin in the same
state and therefore produce the same stream of words.

-   Control: run straight through, never interrupted. Started 2026-09-30 21:49 UTC, finished
    2026-10-01 02:25 UTC (4 h 36 min elapsed; TestU01 reports 02:17:18 CPU time).
-   Kill/resume: the whole process tree was killed with SIGKILL (a hard kill that simulates a crash)
    on a timer and relaunched with the identical command each time. 32 kills and 32 resumes were
    logged by the driver. Finished 2026-10-01 03:15 UTC (5 h 26 min elapsed).

evidence_generation/compare_runs.py compares the two runs. Its complete printed output (also saved
as evidence_generation/comparison_report.txt):

    ========================================================================
    Matched-pair analysis: control vs kill/resume
    Drafted by Claude (Anthropic) for Aly Graham
    ========================================================================
    control:      evidence_generation/control_results
    kill/resume:  evidence_generation/kill_resume_results

    -- 1. Completion
    [INFO] control: 106 sub-test confirmations logged so far
    [INFO] kill/resume: 106 sub-test confirmations logged so far
    [PASS] both runs finished (final report present)

    -- 2. Each sub-test confirmed exactly once, in order; stream position only moves forward
    [PASS] control: no sub-test confirmed twice (no replay) -- 106 unique confirmations
    [PASS] control: sub-tests confirmed in increasing order
    [PASS] control: words consumed strictly increasing (no rewind)
    [PASS] kill/resume: no sub-test confirmed twice (no replay) -- 106 unique confirmations
    [PASS] kill/resume: sub-tests confirmed in increasing order
    [PASS] kill/resume: words consumed strictly increasing (no rewind)

    -- 3. Stream position at the end of every sub-test
    [PASS] 106 sub-test(s) logged by both runs end at the identical stream word

    -- 4/5. Same tests, same order, bit-identical p-values
    [PASS] number of statistics equal -- control=160 kill/resume=160
    [PASS] both runs recorded the same set of sub-tests -- 106 sub-tests
    [PASS] test names identical, in the same order
    [PASS] all 160 p-value(s) in the 106 common sub-test(s) are bit-for-bit identical

    -- 6. p-values inside the pass window
    [PASS] control: all 160 statistics within [0.001, 0.999]
    [PASS] kill/resume: all 160 statistics within [0.001, 0.999]

    -- Interruption summary (kill/resume run)
    [INFO] SIGKILLs sent: 32; resumes logged by the driver: 32
    [INFO] control resumes (should be 0): 0
    [PASS] control run was never interrupted
    [PASS] kill/resume run was actually interrupted and resumed at least once
    [INFO] control: driver's final word count -- 357,098,462,394
    [INFO] kill/resume: driver's final word count -- 357,098,462,394
    [PASS] total words consumed is identical in both runs

    ========================================================================
    RESULT: ALL CHECKS PASSED
    ========================================================================

In plain terms: the killed-and-resumed run and the uninterrupted run used exactly the same
357,098,462,394 words, finished every one of the 106 sub-tests at the same stream position, and
produced bit-for-bit identical results for all 160 statistics. Both runs passed the full battery
(“All tests were passed”), so the same evidence also shows pgprng V2.3 passing BigCrush in this
configuration.

Where the data is (all under evidence_generation/):

-   comparison_report.txt - the printed analysis above.
-   control_results/bigcrush_hi_e256_ckpt.txt - the control run’s full BigCrush report; .err has the
    driver’s progress lines (TESTU01_CKPT_CONFIRMED, RESUMED); control_run_transcript.log is the
    launch log.
-   kill_resume_results/ - the same files for the kill/resume run, plus kill_resume_transcript.log
    (every kill and relaunch). Its .txt report holds only the final segment’s output; the complete
    result set (all 160 p-values) is in the driver checkpoint
    evidence_state/driver_bigcrush_hi.ckpt, which is what compare_runs.py reads.
-   */evidence_state/ - the final driver checkpoint for each run (p-values and word count only). The
    generator checkpoints and the shared genesis file are withheld because pgprng V2.3 is not yet
    published.

Earlier development testing of the same mechanism: a stand-in driver with the same stdin protocol
and checkpoint format survived 85 random whole-pipeline SIGKILLs with all 40 unit results identical
to an uninterrupted run.

Test environment and methodology

-   Hardware: home desktop PC, Dell Tower ECT1250; Intel Core Ultra 7 265 (20 cores, 20 logical
    processors); 31.5 GiB RAM installed (15.3 GiB visible to WSL); NVMe and SATA SSD storage. Not a
    server or cluster.
-   Operating system: Windows 11 Pro, version 26H2 (build 26300); tests ran under WSL 2.7.14.0
    (kernel 6.18.33.2-microsoft-standard-WSL2), Ubuntu 26.04.1 LTS.
-   Software: GCC 15.2.0, GNU Make 4.4.1, Python 3.14.4, GNU patch 2.8, git 2.53.0. TestU01-2009
    source: the umontreal-simul/TestU01-2009 GitHub master archive (testu01.tar.gz, SHA-256
    529b542dec2ba1625812003d9b31cc23e75c1bdd2f52eae4c456a7d7577451a3) with the two patches above.
-   Generator: pgprng V2.3, run as a Python producer piping raw 64-bit words into the C driver.
-   How the evidence runs were launched: two background WSL jobs (control and kill/resume), each in
    its own work directory, started from the shared genesis. The kill/resume job kills on a timer
    (default every 10 minutes, doubling after an attempt that finished no new sub-test so a long
    sub-test is not restarted forever) and relaunches with the same command.
-   Development assistance: the driver, scripts, analysis and this write-up were developed with the
    assistance of Claude (Anthropic), almost entirely Claude Sonnet 5.5 with some Sonnet 5, under
    the author’s direction. The author reviewed and ran everything.

The specific numbers come from pgprng V2.3, which is not yet published (V1.1 is public at
https://github.com/Alligator101/pgprng). A reviewer cannot re-run the exact same generator, only
their own. That does not affect the driver or the patches, which are generator-agnostic.

How to reproduce

To build the driver for your own generator:

    bash checkpointing/build_driver.sh ~/testu01_ckpt_build
    <your generator> | ~/testu01_ckpt_build/pgprng_testu01_ckpt_driver hi bigcrush bigcrush.ckpt

The driver takes the mode (hi, lo, hi_rev, lo_rev), the battery (crush or bigcrush) and a checkpoint
file. After an interruption, run the same command again; the generator must continue at the
words_consumed recorded in the checkpoint file.

To use the driver with your own generator, feed the raw 64-bit words to it on stdin and, after any
interruption, run the same command again; the generator must support the exact-resume protocol
described above. Ctrl-C, a kill -9, or a reboot stops the current attempt, and rerunning the
identical command resumes from the checkpoint. The scripts in evidence_generation/ document how the
matched-pair experiment was run with the author’s pgprng producer; compare_runs.py produces the
analysis from the two result folders.
