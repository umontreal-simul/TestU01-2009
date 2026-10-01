/* pgprng_testu01_ckpt_driver.c
 *
 * Checkpointed Crush/BigCrush driver for pgprng V2.3, added 2026-09-29 per
 * Aly's instruction: "do the TestU01 update to checkpointing. Use care.
 * Need to thoroughly validate. Ensure check pointing structure just like
 * PractRand update so direct use of current testing structure with
 * pgprng."
 *
 * WHY THIS EXISTS: Crush and BigCrush are single long-running processes
 * with no resume support in stock TestU01 (see pgprng_testu01_wrapper.c
 * and CODE_CLEANUP_TODO.md's 2026-09-29 feasibility note -- Crush takes
 * roughly 1-2 hours/stream, BigCrush several hours/stream, and a machine
 * going to sleep, a reboot, or any interruption loses all progress). This
 * driver checkpoints after every individual sub-test ("unit") completes,
 * so an interrupted run resumes at the next not-yet-done unit instead of
 * starting over.
 *
 * HOW THE INSTRUMENTED BATTERY CODE WAS PRODUCED -- NOT HAND-RETYPED:
 * Crush() and BigCrush() are ~1000 and ~1100 lines of exact, parameter-
 * sensitive test-family calls (sample counts, bases, dimensions). Retyping
 * that by hand risks a silent transcription error that would only ever
 * surface as a subtly wrong p-value -- exactly what "use care, thoroughly
 * validate" rules out. Instead, gen_ckpt_battery.py (kept alongside this
 * file) mechanically split the ORIGINAL, UNMODIFIED bbattery.c source at
 * every one of the 103 (Crush) / 115 (BigCrush) textual "++j2;" test-unit
 * boundary lines (see "TEST COUNT" below for why 103/115 exceeds the
 * compiled 96/106) -- confirmed by direct grep that every one of those lines has the
 * exact same six-space indentation and nothing else on it, so the split is
 * unambiguous -- and self-verified the split is lossless (preamble + all
 * extracted units, rejoined, reconstructs the original text BYTE FOR BYTE;
 * the script aborts loudly otherwise). Each unit's ORIGINAL, untouched text
 * is then wrapped in a generic skip/run-and-commit shell (see
 * __ckpt_should_skip/__ckpt_restore_unit/__ckpt_commit_unit below) -- no
 * test parameter or name string was ever retyped by hand or by the script;
 * a unit's p-value name(s), however many there are and however they were
 * produced (some via an inline strcpy literal, some via the shared
 * GetPVal_Walk/GetPVal_CPairs helpers that build a name at runtime from a
 * base string + a "mess" suffix), are captured VERBATIM from whatever they
 * actually were after a real run of that unit and persisted in the
 * checkpoint file -- so restoring a skipped unit never has to reproduce a
 * naming convention, only play back exactly what was recorded.
 *
 * TEST COUNT -- THE DRIVER RUNS EXACTLY THE STOCK BATTERIES (corrected
 * 2026-09-30). Textually, bbattery.c contains 103 (Crush) / 115 (BigCrush)
 * "++j2;" unit boundaries, which is more than its CRUSH_NUM = 96 /
 * BIGCRUSH_NUM = 106 constants. The difference is NOT extra active tests:
 * bbattery.c wraps a block of the source in "#ifdef USE_LONGLONG ... #else
 * ... #endif", and the "#else" branch (alternative 32-bit variants of a few
 * tests, used only on platforms without 64-bit integers) is not compiled
 * here. After preprocessing, the compiled code has exactly 96 (Crush) and
 * 106 (BigCrush) units -- verified both by running the preprocessor over
 * this file and by a synthetic run that reached j2 = 106 and no higher.
 * Unit ids in the checkpoint are the textual positions 1..103 / 1..115, so
 * ids 18-24 (Crush) and 22-30 (BigCrush) never appear; that is expected.
 * Rep[1..CRUSH_NUM] / Rep[1..BIGCRUSH_NUM] = 1 below is therefore identical
 * to stock, the stock util_Assert (j2 <= BIGCRUSH_NUM) is satisfied, and a
 * complete run reports all 160 (BigCrush) statistics.
 *
 * CHECKPOINT DESIGN. __ckpt_commit_unit() only ever runs AFTER a unit's real
 * test call has returned, so every word the unit consumed has been read from
 * stdin (exact count in g_words_consumed). It then durably writes the
 * checkpoint (write temp, fsync, rename -- the file is always either the old
 * or the new complete version) recording the finished unit's p-values AND
 * words_consumed, and prints a confirmation line, all before the next unit
 * starts. On restart, finished units are skipped (their saved p-values are
 * played back) and the first unfinished unit runs from its beginning.
 *
 * For a resumed run to be IDENTICAL to an uninterrupted one, the word stream
 * must resume at exactly words_consumed. The driver cannot do that alone:
 * the generator is a separate process. pgprng_v2_stream.py's testu01-sync
 * mode (started with --driver-ckpt pointing at THIS file's checkpoint)
 * reads words_consumed from it, restores its own last anchor checkpoint
 * (never ahead of that position) and fast-forwards, discarding output, to
 * exactly that word before sending anything. The unit that was cut off then
 * re-runs on the same words it saw the first time, so its p-values -- and
 * every later unit's -- match the uninterrupted run bit for bit. Nothing is
 * skipped and nothing is replayed.
 *
 * Usage:
 *   pgprng_testu01_ckpt_driver <hi|lo|hi_rev|lo_rev> <crush|bigcrush> <ckpt_file>
 *
 * Reads raw native-endian uint64_t words from stdin, exactly like
 * pgprng_testu01_wrapper.c (same next_u64()/GetBits_* protocol, duplicated
 * here rather than shared via a header to keep this driver a single
 * self-contained file that gen_ckpt_battery.py's output can be spliced
 * into mechanically).
 *
 * SmallCrush is NOT supported here (use pgprng_testu01_wrapper for it) --
 * it completes in seconds and doesn't need checkpointing; adding it would
 * mean also handling its file-input code path (SmallCrush's static
 * function is shaped differently, taking either a generator or a filename)
 * for no operational benefit.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "chrono.h"
#include "bbattery.h"
#include "smultin.h"
#include "sknuth.h"
#include "smarsa.h"
#include "snpair.h"
#include "svaria.h"
#include "sstring.h"
#include "swalk.h"
#include "scomp.h"
#include "sspectral.h"
#include "swrite.h"
#include "sres.h"
#include "unif01.h"
#include "ufile.h"
#include "gofs.h"
#include "gofw.h"
#include "fdist.h"
#include "fbar.h"
#include "num.h"
#include "chrono.h"
#include "bbattery_ckpt_internal.h"

#define LEN 120
#define NDIM 200
#define THOUSAND 1000
#define MILLION (THOUSAND * THOUSAND)
#define BILLION (THOUSAND * MILLION)
/* Must match bbattery.c's own #defines exactly (see the header comment's
 * "TEST COUNT" section) -- checked against bbattery.c
 * by hand on 2026-09-29; if TestU01's own source ever changes these,
 * re-check bbattery.c's #define CRUSH_NUM / #define BIGCRUSH_NUM lines. */
#define CRUSH_NUM 96
#define BIGCRUSH_NUM 106

/* ------------------------------------------------------------------ *
 * Generator word source -- identical protocol to pgprng_testu01_wrapper.c
 * ------------------------------------------------------------------ */
static long g_words_consumed = 0;

static uint64_t next_u64(void) {
    uint8_t buf[8];
    size_t got = fread(buf, 1, 8, stdin);
    if (got != 8) {
        fprintf(stderr, "pgprng_testu01_ckpt_driver: stdin exhausted after %ld "
                "words (%s) -- generator pipe ended before the battery "
                "finished consuming its required volume.\n",
                g_words_consumed, feof(stdin) ? "EOF" : "short read");
        exit(1);
    }
    uint64_t w;
    memcpy(&w, buf, 8);
    g_words_consumed++;
    return w;
}

static uint32_t rev32(uint32_t x) {
    x = ((x & 0x55555555u) << 1) | ((x >> 1) & 0x55555555u);
    x = ((x & 0x33333333u) << 2) | ((x >> 2) & 0x33333333u);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x >> 4) & 0x0F0F0F0Fu);
    x = (x << 24) | ((x & 0xFF00u) << 8) | ((x >> 8) & 0xFF00u) | (x >> 24);
    return x;
}

static unsigned int GetBits_hi(void)     { uint64_t w = next_u64(); return (unsigned int)(uint32_t)(w >> 32); }
static unsigned int GetBits_lo(void)     { uint64_t w = next_u64(); return (unsigned int)(uint32_t)(w & 0xFFFFFFFFu); }
static unsigned int GetBits_hi_rev(void) { uint64_t w = next_u64(); return (unsigned int)rev32((uint32_t)(w >> 32)); }
static unsigned int GetBits_lo_rev(void) { uint64_t w = next_u64(); return (unsigned int)rev32((uint32_t)(w & 0xFFFFFFFFu)); }

/* ------------------------------------------------------------------ *
 * Checkpoint state -- generic, one shape for every unit regardless of
 * how many p-values it reports (see header comment).
 * ------------------------------------------------------------------ */
#define CKPT_MAX_UNITS 128
#define CKPT_MAX_PVALS_PER_UNIT 8
#define CKPT_NAME_LEN 128

typedef struct {
    int done;
    int npvals;
    double pval[CKPT_MAX_PVALS_PER_UNIT];
    char name[CKPT_MAX_PVALS_PER_UNIT][CKPT_NAME_LEN];
} ckpt_unit_t;

static ckpt_unit_t g_ckpt_units[CKPT_MAX_UNITS + 1];   /* 1-indexed by j2 */
static char g_ckpt_path[4096];
static char g_ckpt_battery[32];
static char g_ckpt_mode[32];
static int g_ckpt_n_done_at_load = 0;

static int __ckpt_should_skip(int j2) {
    if (j2 < 0 || j2 > CKPT_MAX_UNITS) return 0;
    return g_ckpt_units[j2].done;
}

static void __ckpt_restore_unit(int j2, int *pj) {
    ckpt_unit_t *u = &g_ckpt_units[j2];
    int k;
    for (k = 0; k < u->npvals; k++) {
        bbattery_pVal[++(*pj)] = u->pval[k];
        TestNumber[*pj] = j2;
        strcpy(bbattery_TestNames[*pj], u->name[k]);
    }
}

static void ckpt_save(void) {
    char tmp_path[4160];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_ckpt_path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "pgprng_testu01_ckpt_driver: WARNING cannot open %s for "
                "checkpoint save: %s\n", tmp_path, strerror(errno));
        return;
    }
    fprintf(f, "PGPRNG_TESTU01_CKPT_V1\n");
    fprintf(f, "battery=%s\n", g_ckpt_battery);
    fprintf(f, "mode=%s\n", g_ckpt_mode);
    fprintf(f, "words_consumed=%ld\n", g_words_consumed);
    int j2;
    for (j2 = 1; j2 <= CKPT_MAX_UNITS; j2++) {
        ckpt_unit_t *u = &g_ckpt_units[j2];
        if (!u->done) continue;
        int k;
        for (k = 0; k < u->npvals; k++) {
            fprintf(f, "PVAL %d %d %.17g %s\n", j2, k, u->pval[k], u->name[k]);
        }
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp_path, g_ckpt_path) != 0) {
        fprintf(stderr, "pgprng_testu01_ckpt_driver: WARNING rename %s -> %s "
                "failed: %s\n", tmp_path, g_ckpt_path, strerror(errno));
    }
}

static void ckpt_log_confirm(int j2) {
    /* Flushed immediately. The checkpoint file (already durably saved before
     * this is called) is what resume correctness depends on; this line is for
     * humans and scripts (compare_runs.py) watching progress. A kill can land
     * between the save and this line, so a finished unit could in principle
     * be missing its line -- the checkpoint is the authority. */
    fprintf(stderr, "TESTU01_CKPT_CONFIRMED battery=%s mode=%s j2=%d words_consumed=%ld\n",
            g_ckpt_battery, g_ckpt_mode, j2, g_words_consumed);
    fflush(stderr);
}

static void __ckpt_commit_unit(int j2, int jbefore, int jafter) {
    if (j2 < 1 || j2 > CKPT_MAX_UNITS) {
        fprintf(stderr, "pgprng_testu01_ckpt_driver: j2=%d out of range "
                "1..%d -- aborting.\n", j2, CKPT_MAX_UNITS);
        exit(1);
    }
    int n = jafter - jbefore;
    if (n < 0 || n > CKPT_MAX_PVALS_PER_UNIT) {
        fprintf(stderr, "pgprng_testu01_ckpt_driver: unit j2=%d produced %d "
                "p-value(s), outside 0..%d -- aborting rather than silently "
                "truncate or misrecord.\n", j2, n, CKPT_MAX_PVALS_PER_UNIT);
        exit(1);
    }
    ckpt_unit_t *u = &g_ckpt_units[j2];
    u->npvals = n;
    int k;
    for (k = 0; k < n; k++) {
        u->pval[k] = bbattery_pVal[jbefore + 1 + k];
        strncpy(u->name[k], bbattery_TestNames[jbefore + 1 + k], CKPT_NAME_LEN - 1);
        u->name[k][CKPT_NAME_LEN - 1] = '\0';
    }
    u->done = 1;
    ckpt_save();
    ckpt_log_confirm(j2);
}

static void ckpt_load(const char *path, const char *battery, const char *mode) {
    FILE *f = fopen(path, "r");
    if (!f) return;  /* no checkpoint yet -- fresh run */
    char line[512];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "PGPRNG_TESTU01_CKPT_V1", 22) != 0) {
        fprintf(stderr, "pgprng_testu01_ckpt_driver: %s does not look like a "
                "PGPRNG_TESTU01_CKPT_V1 file -- refusing to resume from it "
                "(move it aside if you intend to start fresh).\n", path);
        exit(1);
    }
    char file_battery[32] = "", file_mode[32] = "";
    long words_consumed_at_save = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "battery=", 8) == 0) {
            sscanf(line + 8, "%31s", file_battery);
        } else if (strncmp(line, "mode=", 5) == 0) {
            sscanf(line + 5, "%31s", file_mode);
        } else if (strncmp(line, "words_consumed=", 15) == 0) {
            sscanf(line + 15, "%ld", &words_consumed_at_save);
        } else if (strncmp(line, "PVAL ", 5) == 0) {
            int j2, idx, consumed = 0;
            double val;
            if (sscanf(line + 5, "%d %d %lf %n", &j2, &idx, &val, &consumed) < 3) {
                fprintf(stderr, "pgprng_testu01_ckpt_driver: malformed PVAL line in "
                        "%s: %s -- refusing to resume from a possibly-corrupt "
                        "checkpoint.\n", path, line);
                exit(1);
            }
            if (j2 < 1 || j2 > CKPT_MAX_UNITS || idx < 0 || idx >= CKPT_MAX_PVALS_PER_UNIT) {
                fprintf(stderr, "pgprng_testu01_ckpt_driver: PVAL j2=%d idx=%d out "
                        "of range in %s -- refusing to resume.\n", j2, idx, path);
                exit(1);
            }
            char *name = line + 5 + consumed;
            size_t nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == '\n' || name[nlen-1] == '\r')) name[--nlen] = '\0';
            ckpt_unit_t *u = &g_ckpt_units[j2];
            if (idx >= u->npvals) u->npvals = idx + 1;
            u->pval[idx] = val;
            strncpy(u->name[idx], name, CKPT_NAME_LEN - 1);
            u->name[idx][CKPT_NAME_LEN - 1] = '\0';
            u->done = 1;
        }
    }
    fclose(f);
    /* Continue the TRUE cumulative words-consumed count across this resume
     * (a fresh process invocation otherwise starts g_words_consumed back at
     * 0, which would make the progress log look like it went backwards --
     * purely cosmetic, since no skip/restore decision depends on this
     * value, but confusing enough to be worth fixing). */
    g_words_consumed = words_consumed_at_save;
    if (strcmp(file_battery, battery) != 0 || strcmp(file_mode, mode) != 0) {
        fprintf(stderr, "pgprng_testu01_ckpt_driver: checkpoint %s was for "
                "battery=%s mode=%s, but this invocation asked for "
                "battery=%s mode=%s -- refusing to silently resume the "
                "wrong run (move the old checkpoint aside, or fix the "
                "command line).\n", path, file_battery, file_mode, battery, mode);
        exit(1);
    }
    int n_done = 0, j2;
    for (j2 = 1; j2 <= CKPT_MAX_UNITS; j2++) if (g_ckpt_units[j2].done) n_done++;
    g_ckpt_n_done_at_load = n_done;
    fprintf(stderr, "pgprng_testu01_ckpt_driver: RESUMED from %s: %d unit(s) already "
            "done (words_consumed at last save: %ld) -- continuing.\n",
            path, n_done, words_consumed_at_save);
}

static void Crush_Ckpt (unif01_Gen * gen, int Rep[])
/*
 * A battery of stringent statistical tests for Random Number Generators
 * used in simulation.
 * Rep[i] gives the number of times that test i will be done. The default
 * values are Rep[i] = 1 for all i.
 */
{
   const int s = 30;
   const int r = 0;
   int i;
   chrono_Chrono *Timer;
   char genName[LEN + 1] = "";
   int j = -1;
   int j2 = 0;

   Timer = chrono_Create ();
   InitBat ();
   if (swrite_Basic) {
      printf ("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
         "                 Starting Crush\n"
         "                 Version: %s\n"
         "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n\n\n",
         PACKAGE_STRING);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(1)) {
            __ckpt_restore_unit(1, &j);
         } else {
            int __ckpt_jbefore_1 = j;
         smarsa_SerialOver (gen, res, 1, 500 * MILLION, 0, 4096, 2);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SerialOver, t = 2");
            __ckpt_commit_unit(1, __ckpt_jbefore_1, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(2)) {
            __ckpt_restore_unit(2, &j);
         } else {
            int __ckpt_jbefore_2 = j;
         smarsa_SerialOver (gen, res, 1, 300 * MILLION, 0, 64, 4);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SerialOver, t = 4");
            __ckpt_commit_unit(2, __ckpt_jbefore_2, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      smarsa_Res *res;
      res = smarsa_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(3)) {
            __ckpt_restore_unit(3, &j);
         } else {
            int __ckpt_jbefore_3 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 0, 1024 * 1024, 2);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 2");
            __ckpt_commit_unit(3, __ckpt_jbefore_3, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(4)) {
            __ckpt_restore_unit(4, &j);
         } else {
            int __ckpt_jbefore_4 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 10, 1024 * 1024, 2);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 2");
            __ckpt_commit_unit(4, __ckpt_jbefore_4, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(5)) {
            __ckpt_restore_unit(5, &j);
         } else {
            int __ckpt_jbefore_5 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 0, 1024, 4);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 4");
            __ckpt_commit_unit(5, __ckpt_jbefore_5, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(6)) {
            __ckpt_restore_unit(6, &j);
         } else {
            int __ckpt_jbefore_6 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 20, 1024, 4);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 4");
            __ckpt_commit_unit(6, __ckpt_jbefore_6, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(7)) {
            __ckpt_restore_unit(7, &j);
         } else {
            int __ckpt_jbefore_7 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 0, 32, 8);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 8");
            __ckpt_commit_unit(7, __ckpt_jbefore_7, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(8)) {
            __ckpt_restore_unit(8, &j);
         } else {
            int __ckpt_jbefore_8 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 25, 32, 8);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 8");
            __ckpt_commit_unit(8, __ckpt_jbefore_8, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(9)) {
            __ckpt_restore_unit(9, &j);
         } else {
            int __ckpt_jbefore_9 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 0, 4, 20);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 20");
            __ckpt_commit_unit(9, __ckpt_jbefore_9, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(10)) {
            __ckpt_restore_unit(10, &j);
         } else {
            int __ckpt_jbefore_10 = j;
         smarsa_CollisionOver (gen, res, 10, 10 * MILLION, 28, 4, 20);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 20");
            __ckpt_commit_unit(10, __ckpt_jbefore_10, j);
         }
      }
      smarsa_DeleteRes (res);
   }
   {
      sres_Poisson *res;
      res = sres_CreatePoisson ();

#ifdef USE_LONGLONG
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(11)) {
            __ckpt_restore_unit(11, &j);
         } else {
            int __ckpt_jbefore_11 = j;
         long d;
#if LONG_MAX <= 2147483647L
         d = 1073741824L;
         smarsa_BirthdaySpacings (gen, res, 10, 10 * MILLION, 0, d, 2, 1);
#else
         d = 2*1073741824L;
         smarsa_BirthdaySpacings (gen, res, 5, 20 * MILLION, 0, d, 2, 1);
#endif
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 2");
            __ckpt_commit_unit(11, __ckpt_jbefore_11, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(12)) {
            __ckpt_restore_unit(12, &j);
         } else {
            int __ckpt_jbefore_12 = j;
         smarsa_BirthdaySpacings (gen, res, 5, 20 * MILLION, 0, 2097152, 3,
            1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 3");
            __ckpt_commit_unit(12, __ckpt_jbefore_12, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(13)) {
            __ckpt_restore_unit(13, &j);
         } else {
            int __ckpt_jbefore_13 = j;
         smarsa_BirthdaySpacings (gen, res, 5, 20 * MILLION, 0, 65536, 4, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 4");
            __ckpt_commit_unit(13, __ckpt_jbefore_13, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(14)) {
            __ckpt_restore_unit(14, &j);
         } else {
            int __ckpt_jbefore_14 = j;
         smarsa_BirthdaySpacings (gen, res, 3, 20 * MILLION, 0, 512, 7, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 7");
            __ckpt_commit_unit(14, __ckpt_jbefore_14, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(15)) {
            __ckpt_restore_unit(15, &j);
         } else {
            int __ckpt_jbefore_15 = j;
         smarsa_BirthdaySpacings (gen, res, 3, 20 * MILLION, 7, 512, 7, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 7");
            __ckpt_commit_unit(15, __ckpt_jbefore_15, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(16)) {
            __ckpt_restore_unit(16, &j);
         } else {
            int __ckpt_jbefore_16 = j;
         smarsa_BirthdaySpacings (gen, res, 3, 20 * MILLION, 14, 256, 8, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 8");
            __ckpt_commit_unit(16, __ckpt_jbefore_16, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(17)) {
            __ckpt_restore_unit(17, &j);
         } else {
            int __ckpt_jbefore_17 = j;
         smarsa_BirthdaySpacings (gen, res, 3, 20 * MILLION, 22, 256, 8, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 8");
            __ckpt_commit_unit(17, __ckpt_jbefore_17, j);
         }
      }

#else
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(18)) {
            __ckpt_restore_unit(18, &j);
         } else {
            int __ckpt_jbefore_18 = j;
         smarsa_BirthdaySpacings (gen, res, 200, 4 * MILLION / 10, 0,
            67108864, 2, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 2");
            __ckpt_commit_unit(18, __ckpt_jbefore_18, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(19)) {
            __ckpt_restore_unit(19, &j);
         } else {
            int __ckpt_jbefore_19 = j;
         smarsa_BirthdaySpacings (gen, res, 100, 4 * MILLION / 10, 0, 131072,
            3, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 3");
            __ckpt_commit_unit(19, __ckpt_jbefore_19, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(20)) {
            __ckpt_restore_unit(20, &j);
         } else {
            int __ckpt_jbefore_20 = j;
         smarsa_BirthdaySpacings (gen, res, 200, 4 * MILLION / 10, 0,
            1024 * 8, 4, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 4");
            __ckpt_commit_unit(20, __ckpt_jbefore_20, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(21)) {
            __ckpt_restore_unit(21, &j);
         } else {
            int __ckpt_jbefore_21 = j;
         smarsa_BirthdaySpacings (gen, res, 100, 4 * MILLION / 10, 0, 16, 13,
            1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(21, __ckpt_jbefore_21, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(22)) {
            __ckpt_restore_unit(22, &j);
         } else {
            int __ckpt_jbefore_22 = j;
         smarsa_BirthdaySpacings (gen, res, 100, 4 * MILLION / 10, 10, 16,
            13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(22, __ckpt_jbefore_22, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(23)) {
            __ckpt_restore_unit(23, &j);
         } else {
            int __ckpt_jbefore_23 = j;
         smarsa_BirthdaySpacings (gen, res, 100, 4 * MILLION / 10, 20, 16,
            13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(23, __ckpt_jbefore_23, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(24)) {
            __ckpt_restore_unit(24, &j);
         } else {
            int __ckpt_jbefore_24 = j;
         smarsa_BirthdaySpacings (gen, res, 100, 4 * MILLION / 10, 26, 16,
            13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(24, __ckpt_jbefore_24, j);
         }
      }
#endif

      sres_DeletePoisson (res);
   }
   {
      lebool flag = snpair_mNP2S_Flag;
      snpair_Res *res;
      res = snpair_CreateRes ();

      snpair_mNP2S_Flag = FALSE;
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(25)) {
            __ckpt_restore_unit(25, &j);
         } else {
            int __ckpt_jbefore_25 = j;
         snpair_ClosePairs (gen, res, 10, 2 * MILLION, 0, 2, 0, 30);
         GetPVal_CPairs (10, res, &j, ", t = 2", j2);
            __ckpt_commit_unit(25, __ckpt_jbefore_25, j);
         }
      }

      snpair_mNP2S_Flag = TRUE;
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(26)) {
            __ckpt_restore_unit(26, &j);
         } else {
            int __ckpt_jbefore_26 = j;
         snpair_ClosePairs (gen, res, 10, 2 * MILLION, 0, 3, 0, 30);
         GetPVal_CPairs (10, res, &j, ", t = 3", j2);
            __ckpt_commit_unit(26, __ckpt_jbefore_26, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(27)) {
            __ckpt_restore_unit(27, &j);
         } else {
            int __ckpt_jbefore_27 = j;
         snpair_ClosePairs (gen, res, 5, 2 * MILLION, 0, 7, 0, 30);
         GetPVal_CPairs (10, res, &j, ", t = 7", j2);
            __ckpt_commit_unit(27, __ckpt_jbefore_27, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(28)) {
            __ckpt_restore_unit(28, &j);
         } else {
            int __ckpt_jbefore_28 = j;
         snpair_ClosePairsBitMatch (gen, res, 4, 4 * MILLION, 0, 2);
         bbattery_pVal[++j] = res->pVal[snpair_BM];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "ClosePairsBitMatch, t = 2");
            __ckpt_commit_unit(28, __ckpt_jbefore_28, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(29)) {
            __ckpt_restore_unit(29, &j);
         } else {
            int __ckpt_jbefore_29 = j;
         snpair_ClosePairsBitMatch (gen, res, 2, 4 * MILLION, 0, 4);
         bbattery_pVal[++j] = res->pVal[snpair_BM];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "ClosePairsBitMatch, t = 4");
            __ckpt_commit_unit(29, __ckpt_jbefore_29, j);
         }
      }
      snpair_DeleteRes (res);
      snpair_mNP2S_Flag = flag;
   }
   {
      sres_Chi2 *res;
      res = sres_CreateChi2 ();

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(30)) {
            __ckpt_restore_unit(30, &j);
         } else {
            int __ckpt_jbefore_30 = j;
         sknuth_SimpPoker (gen, res, 1, 40 * MILLION, 0, 16, 16);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, d = 16");
            __ckpt_commit_unit(30, __ckpt_jbefore_30, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(31)) {
            __ckpt_restore_unit(31, &j);
         } else {
            int __ckpt_jbefore_31 = j;
         sknuth_SimpPoker (gen, res, 1, 40 * MILLION, 26, 16, 16);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, d = 16");
            __ckpt_commit_unit(31, __ckpt_jbefore_31, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(32)) {
            __ckpt_restore_unit(32, &j);
         } else {
            int __ckpt_jbefore_32 = j;
         sknuth_SimpPoker (gen, res, 1, 10 * MILLION, 0, 64, 64);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, d = 64");
            __ckpt_commit_unit(32, __ckpt_jbefore_32, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(33)) {
            __ckpt_restore_unit(33, &j);
         } else {
            int __ckpt_jbefore_33 = j;
         sknuth_SimpPoker (gen, res, 1, 10 * MILLION, 24, 64, 64);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, d = 64");
            __ckpt_commit_unit(33, __ckpt_jbefore_33, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(34)) {
            __ckpt_restore_unit(34, &j);
         } else {
            int __ckpt_jbefore_34 = j;
         sknuth_CouponCollector (gen, res, 1, 40 * MILLION, 0, 4);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, d = 4");
            __ckpt_commit_unit(34, __ckpt_jbefore_34, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(35)) {
            __ckpt_restore_unit(35, &j);
         } else {
            int __ckpt_jbefore_35 = j;
         sknuth_CouponCollector (gen, res, 1, 40 * MILLION, 28, 4);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, d = 4");
            __ckpt_commit_unit(35, __ckpt_jbefore_35, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(36)) {
            __ckpt_restore_unit(36, &j);
         } else {
            int __ckpt_jbefore_36 = j;
         sknuth_CouponCollector (gen, res, 1, 10 * MILLION, 0, 16);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, d = 16");
            __ckpt_commit_unit(36, __ckpt_jbefore_36, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(37)) {
            __ckpt_restore_unit(37, &j);
         } else {
            int __ckpt_jbefore_37 = j;
         sknuth_CouponCollector (gen, res, 1, 10 * MILLION, 26, 16);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, d = 16");
            __ckpt_commit_unit(37, __ckpt_jbefore_37, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(38)) {
            __ckpt_restore_unit(38, &j);
         } else {
            int __ckpt_jbefore_38 = j;
         sknuth_Gap (gen, res, 1, 100 * MILLION, 0, 0.0, 0.125);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 0");
            __ckpt_commit_unit(38, __ckpt_jbefore_38, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(39)) {
            __ckpt_restore_unit(39, &j);
         } else {
            int __ckpt_jbefore_39 = j;
         sknuth_Gap (gen, res, 1, 100 * MILLION, 27, 0.0, 0.125);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 27");
            __ckpt_commit_unit(39, __ckpt_jbefore_39, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(40)) {
            __ckpt_restore_unit(40, &j);
         } else {
            int __ckpt_jbefore_40 = j;
         sknuth_Gap (gen, res, 1, 5 * MILLION, 0, 0.0, 1.0/256.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 0");
            __ckpt_commit_unit(40, __ckpt_jbefore_40, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(41)) {
            __ckpt_restore_unit(41, &j);
         } else {
            int __ckpt_jbefore_41 = j;
         sknuth_Gap (gen, res, 1, 5 * MILLION, 22, 0.0, 1.0/256.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 22");
            __ckpt_commit_unit(41, __ckpt_jbefore_41, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(42)) {
            __ckpt_restore_unit(42, &j);
         } else {
            int __ckpt_jbefore_42 = j;
         sknuth_Run (gen, res, 1, 500 * MILLION, 0, TRUE);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of U01, r = 0");
            __ckpt_commit_unit(42, __ckpt_jbefore_42, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(43)) {
            __ckpt_restore_unit(43, &j);
         } else {
            int __ckpt_jbefore_43 = j;
         sknuth_Run (gen, res, 1, 500 * MILLION, 15, FALSE);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of U01, r = 15");
            __ckpt_commit_unit(43, __ckpt_jbefore_43, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(44)) {
            __ckpt_restore_unit(44, &j);
         } else {
            int __ckpt_jbefore_44 = j;
         sknuth_Permutation (gen, res, 1, 50 * MILLION, 0, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Permutation, r = 0");
            __ckpt_commit_unit(44, __ckpt_jbefore_44, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(45)) {
            __ckpt_restore_unit(45, &j);
         } else {
            int __ckpt_jbefore_45 = j;
         sknuth_Permutation (gen, res, 1, 50 * MILLION, 15, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Permutation, r = 15");
            __ckpt_commit_unit(45, __ckpt_jbefore_45, j);
         }
      }
      sres_DeleteChi2 (res);
   }
   {
      sknuth_Res2 *res;
      res = sknuth_CreateRes2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(46)) {
            __ckpt_restore_unit(46, &j);
         } else {
            int __ckpt_jbefore_46 = j;
         sknuth_CollisionPermut (gen, res, 5, 10 * MILLION, 0, 13);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionPermut, r = 0");
            __ckpt_commit_unit(46, __ckpt_jbefore_46, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(47)) {
            __ckpt_restore_unit(47, &j);
         } else {
            int __ckpt_jbefore_47 = j;
         sknuth_CollisionPermut (gen, res, 5, 10 * MILLION, 15, 13);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionPermut, r = 15");
            __ckpt_commit_unit(47, __ckpt_jbefore_47, j);
         }
      }
      sknuth_DeleteRes2 (res);
   }
   {
      sknuth_Res1 *res;
      res = sknuth_CreateRes1 ();

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(48)) {
            __ckpt_restore_unit(48, &j);
         } else {
            int __ckpt_jbefore_48 = j;
         sknuth_MaxOft (gen, res, 10, 10 * MILLION, 0, MILLION / 10, 5);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 5");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 5");
            __ckpt_commit_unit(48, __ckpt_jbefore_48, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(49)) {
            __ckpt_restore_unit(49, &j);
         } else {
            int __ckpt_jbefore_49 = j;
         sknuth_MaxOft (gen, res, 5, 10 * MILLION, 0, MILLION / 10, 10);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 10");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 10");
            __ckpt_commit_unit(49, __ckpt_jbefore_49, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(50)) {
            __ckpt_restore_unit(50, &j);
         } else {
            int __ckpt_jbefore_50 = j;
         sknuth_MaxOft (gen, res, 1, 10 * MILLION, 0, MILLION / 10, 20);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 20");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 20");
            __ckpt_commit_unit(50, __ckpt_jbefore_50, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(51)) {
            __ckpt_restore_unit(51, &j);
         } else {
            int __ckpt_jbefore_51 = j;
         sknuth_MaxOft (gen, res, 1, 10 * MILLION, 0, MILLION / 10, 30);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 30");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 30");
            __ckpt_commit_unit(51, __ckpt_jbefore_51, j);
         }
      }
      sknuth_DeleteRes1 (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(52)) {
            __ckpt_restore_unit(52, &j);
         } else {
            int __ckpt_jbefore_52 = j;
         svaria_SampleProd (gen, res, 1, 10 * MILLION, 0, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleProd, t = 10");
            __ckpt_commit_unit(52, __ckpt_jbefore_52, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(53)) {
            __ckpt_restore_unit(53, &j);
         } else {
            int __ckpt_jbefore_53 = j;
         svaria_SampleProd (gen, res, 1, 10 * MILLION, 0, 30);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleProd, t = 30");
            __ckpt_commit_unit(53, __ckpt_jbefore_53, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(54)) {
            __ckpt_restore_unit(54, &j);
         } else {
            int __ckpt_jbefore_54 = j;
         svaria_SampleMean (gen, res, 10*MILLION, 20, 0);
         bbattery_pVal[++j] = res->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleMean");
            __ckpt_commit_unit(54, __ckpt_jbefore_54, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(55)) {
            __ckpt_restore_unit(55, &j);
         } else {
            int __ckpt_jbefore_55 = j;
         svaria_SampleCorr (gen, res, 1, 500 * MILLION, 0, 1);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleCorr");
            __ckpt_commit_unit(55, __ckpt_jbefore_55, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(56)) {
            __ckpt_restore_unit(56, &j);
         } else {
            int __ckpt_jbefore_56 = j;
         svaria_AppearanceSpacings (gen, res, 1, 10 * MILLION, 400 * MILLION,
            r, 30, 15);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AppearanceSpacings, r = 0");
            __ckpt_commit_unit(56, __ckpt_jbefore_56, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(57)) {
            __ckpt_restore_unit(57, &j);
         } else {
            int __ckpt_jbefore_57 = j;
         svaria_AppearanceSpacings (gen, res, 1, 10 * MILLION, 100 * MILLION,
            20, 10, 15);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AppearanceSpacings, r = 20");
            __ckpt_commit_unit(57, __ckpt_jbefore_57, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      smarsa_Res2 *res2;
      sres_Chi2 *res;
      res = sres_CreateChi2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(58)) {
            __ckpt_restore_unit(58, &j);
         } else {
            int __ckpt_jbefore_58 = j;
         svaria_WeightDistrib (gen, res, 1, 2 * MILLION, 0, 256, 0.0, 0.125);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 0");
            __ckpt_commit_unit(58, __ckpt_jbefore_58, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(59)) {
            __ckpt_restore_unit(59, &j);
         } else {
            int __ckpt_jbefore_59 = j;
         svaria_WeightDistrib (gen, res, 1, 2 * MILLION, 8, 256, 0.0, 0.125);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 8");
            __ckpt_commit_unit(59, __ckpt_jbefore_59, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(60)) {
            __ckpt_restore_unit(60, &j);
         } else {
            int __ckpt_jbefore_60 = j;
         svaria_WeightDistrib (gen, res, 1, 2 * MILLION, 16, 256, 0.0, 0.125);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 16");
            __ckpt_commit_unit(60, __ckpt_jbefore_60, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(61)) {
            __ckpt_restore_unit(61, &j);
         } else {
            int __ckpt_jbefore_61 = j;
         svaria_WeightDistrib (gen, res, 1, 2 * MILLION, 24, 256, 0.0, 0.125);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 24");
            __ckpt_commit_unit(61, __ckpt_jbefore_61, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(62)) {
            __ckpt_restore_unit(62, &j);
         } else {
            int __ckpt_jbefore_62 = j;
         svaria_SumCollector (gen, res, 1, 20 * MILLION, 0, 10.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SumCollector");
            __ckpt_commit_unit(62, __ckpt_jbefore_62, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(63)) {
            __ckpt_restore_unit(63, &j);
         } else {
            int __ckpt_jbefore_63 = j;
         smarsa_MatrixRank (gen, res, 1, MILLION, r, s, 2 * s, 2 * s);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, 60 x 60");
            __ckpt_commit_unit(63, __ckpt_jbefore_63, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(64)) {
            __ckpt_restore_unit(64, &j);
         } else {
            int __ckpt_jbefore_64 = j;
         smarsa_MatrixRank (gen, res, 1, MILLION, 20, 10, 2 * s, 2 * s);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, 60 x 60");
            __ckpt_commit_unit(64, __ckpt_jbefore_64, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(65)) {
            __ckpt_restore_unit(65, &j);
         } else {
            int __ckpt_jbefore_65 = j;
         smarsa_MatrixRank (gen, res, 1, 50 * THOUSAND, r, s, 10 * s, 10 * s);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, 300 x 300");
            __ckpt_commit_unit(65, __ckpt_jbefore_65, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(66)) {
            __ckpt_restore_unit(66, &j);
         } else {
            int __ckpt_jbefore_66 = j;
         smarsa_MatrixRank (gen, res, 1, 50 * THOUSAND, 20, 10, 10 * s,
            10 * s);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, 300 x 300");
            __ckpt_commit_unit(66, __ckpt_jbefore_66, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(67)) {
            __ckpt_restore_unit(67, &j);
         } else {
            int __ckpt_jbefore_67 = j;
         smarsa_MatrixRank (gen, res, 1, 2 * THOUSAND, r, s, 40 * s, 40 * s);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, 1200 x 1200");
            __ckpt_commit_unit(67, __ckpt_jbefore_67, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(68)) {
            __ckpt_restore_unit(68, &j);
         } else {
            int __ckpt_jbefore_68 = j;
         smarsa_MatrixRank (gen, res, 1, 2 * THOUSAND, 20, 10, 40 * s, 40 * s);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, 1200 x 1200");
            __ckpt_commit_unit(68, __ckpt_jbefore_68, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(69)) {
            __ckpt_restore_unit(69, &j);
         } else {
            int __ckpt_jbefore_69 = j;
         smarsa_Savir2 (gen, res, 1, 20 * MILLION, 0, 1024*1024, 30);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Savir2");
            __ckpt_commit_unit(69, __ckpt_jbefore_69, j);
         }
      }
      sres_DeleteChi2 (res);

      res2 = smarsa_CreateRes2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(70)) {
            __ckpt_restore_unit(70, &j);
         } else {
            int __ckpt_jbefore_70 = j;
         smarsa_GCD (gen, res2, 1, 100 * MILLION, 0, 30);
         bbattery_pVal[++j] = res2->GCD->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "GCD, r = 0");
            __ckpt_commit_unit(70, __ckpt_jbefore_70, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(71)) {
            __ckpt_restore_unit(71, &j);
         } else {
            int __ckpt_jbefore_71 = j;
         smarsa_GCD (gen, res2, 1, 40 * MILLION, 10, 20);
         bbattery_pVal[++j] = res2->GCD->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "GCD, r = 10");
            __ckpt_commit_unit(71, __ckpt_jbefore_71, j);
         }
      }
      smarsa_DeleteRes2 (res2);
   }
   {
      swalk_Res *res;
      res = swalk_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(72)) {
            __ckpt_restore_unit(72, &j);
         } else {
            int __ckpt_jbefore_72 = j;
         swalk_RandomWalk1 (gen, res, 1, 50 * MILLION, r, s, 90, 90);
         GetPVal_Walk (1, res, &j, " (L = 90)", j2);
            __ckpt_commit_unit(72, __ckpt_jbefore_72, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(73)) {
            __ckpt_restore_unit(73, &j);
         } else {
            int __ckpt_jbefore_73 = j;
         swalk_RandomWalk1 (gen, res, 1, 10 * MILLION, 20, 10, 90, 90);
         GetPVal_Walk (1, res, &j, " (L = 90)", j2);
            __ckpt_commit_unit(73, __ckpt_jbefore_73, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(74)) {
            __ckpt_restore_unit(74, &j);
         } else {
            int __ckpt_jbefore_74 = j;
         swalk_RandomWalk1 (gen, res, 1, 5 * MILLION, r, s, 1000, 1000);
         GetPVal_Walk (1, res, &j, " (L = 1000)", j2);
            __ckpt_commit_unit(74, __ckpt_jbefore_74, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(75)) {
            __ckpt_restore_unit(75, &j);
         } else {
            int __ckpt_jbefore_75 = j;
         swalk_RandomWalk1 (gen, res, 1, MILLION, 20, 10, 1000, 1000);
         GetPVal_Walk (1, res, &j, " (L = 1000)", j2);
            __ckpt_commit_unit(75, __ckpt_jbefore_75, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(76)) {
            __ckpt_restore_unit(76, &j);
         } else {
            int __ckpt_jbefore_76 = j;
         swalk_RandomWalk1 (gen, res, 1, MILLION / 2, r, s, 10000, 10000);
         GetPVal_Walk (1, res, &j, " (L = 10000)", j2);
            __ckpt_commit_unit(76, __ckpt_jbefore_76, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(77)) {
            __ckpt_restore_unit(77, &j);
         } else {
            int __ckpt_jbefore_77 = j;
         swalk_RandomWalk1 (gen, res, 1, MILLION / 10, 20, 10, 10000, 10000);
         GetPVal_Walk (1, res, &j, " (L = 10000)", j2);
            __ckpt_commit_unit(77, __ckpt_jbefore_77, j);
         }
      }
      swalk_DeleteRes (res);
   }
   {
      scomp_Res *res;
      res = scomp_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(78)) {
            __ckpt_restore_unit(78, &j);
         } else {
            int __ckpt_jbefore_78 = j;
         scomp_LinearComp (gen, res, 1, 120 * THOUSAND, r, 1);
         bbattery_pVal[++j] = res->JumpNum->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 0");
         bbattery_pVal[++j] = res->JumpSize->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 0");
            __ckpt_commit_unit(78, __ckpt_jbefore_78, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(79)) {
            __ckpt_restore_unit(79, &j);
         } else {
            int __ckpt_jbefore_79 = j;
         scomp_LinearComp (gen, res, 1, 120 * THOUSAND, 29, 1);
         bbattery_pVal[++j] = res->JumpNum->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 29");
         bbattery_pVal[++j] = res->JumpSize->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 29");
            __ckpt_commit_unit(79, __ckpt_jbefore_79, j);
         }
      }
      scomp_DeleteRes (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(80)) {
            __ckpt_restore_unit(80, &j);
         } else {
            int __ckpt_jbefore_80 = j;
         scomp_LempelZiv (gen, res, 10, 25, r, s);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LempelZiv");
            __ckpt_commit_unit(80, __ckpt_jbefore_80, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      sspectral_Res *res;
      res = sspectral_CreateRes ();

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(81)) {
            __ckpt_restore_unit(81, &j);
         } else {
            int __ckpt_jbefore_81 = j;
         sspectral_Fourier3 (gen, res, 50 * THOUSAND, 14, r, s);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Fourier3, r = 0");
            __ckpt_commit_unit(81, __ckpt_jbefore_81, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(82)) {
            __ckpt_restore_unit(82, &j);
         } else {
            int __ckpt_jbefore_82 = j;
         sspectral_Fourier3 (gen, res, 50 * THOUSAND, 14, 20, 10);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Fourier3, r = 20");
            __ckpt_commit_unit(82, __ckpt_jbefore_82, j);
         }
      }
      sspectral_DeleteRes (res);
   }
   {
      sstring_Res2 *res;
      res = sstring_CreateRes2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(83)) {
            __ckpt_restore_unit(83, &j);
         } else {
            int __ckpt_jbefore_83 = j;
         sstring_LongestHeadRun (gen, res, 1, 1000, r, s, 20 + 10 * MILLION);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 0");
         bbattery_pVal[++j] = res->Disc->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 0");
            __ckpt_commit_unit(83, __ckpt_jbefore_83, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(84)) {
            __ckpt_restore_unit(84, &j);
         } else {
            int __ckpt_jbefore_84 = j;
         sstring_LongestHeadRun (gen, res, 1, 300, 20, 10, 20 + 10 * MILLION);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 20");
         bbattery_pVal[++j] = res->Disc->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 20");
            __ckpt_commit_unit(84, __ckpt_jbefore_84, j);
         }
      }
      sstring_DeleteRes2 (res);
   }
   {
      sres_Chi2 *res;
      res = sres_CreateChi2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(85)) {
            __ckpt_restore_unit(85, &j);
         } else {
            int __ckpt_jbefore_85 = j;
         sstring_PeriodsInStrings (gen, res, 1, 300 * MILLION, r, s);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "PeriodsInStrings, r = 0");
            __ckpt_commit_unit(85, __ckpt_jbefore_85, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(86)) {
            __ckpt_restore_unit(86, &j);
         } else {
            int __ckpt_jbefore_86 = j;
         sstring_PeriodsInStrings (gen, res, 1, 300 * MILLION, 15, 15);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "PeriodsInStrings, r = 15");
            __ckpt_commit_unit(86, __ckpt_jbefore_86, j);
         }
      }
      sres_DeleteChi2 (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(87)) {
            __ckpt_restore_unit(87, &j);
         } else {
            int __ckpt_jbefore_87 = j;
         sstring_HammingWeight2 (gen, res, 100, 100 * MILLION, r, s, MILLION);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingWeight2, r = 0");
            __ckpt_commit_unit(87, __ckpt_jbefore_87, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(88)) {
            __ckpt_restore_unit(88, &j);
         } else {
            int __ckpt_jbefore_88 = j;
         sstring_HammingWeight2 (gen, res, 30, 100 * MILLION, 20, 10, MILLION);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingWeight2, r = 20");
            __ckpt_commit_unit(88, __ckpt_jbefore_88, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      sstring_Res *res;
      res = sstring_CreateRes ();
      /* sstring_HammingCorr will probably be removed: less sensitive than
         svaria_HammingIndep */
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(89)) {
            __ckpt_restore_unit(89, &j);
         } else {
            int __ckpt_jbefore_89 = j;
         sstring_HammingCorr (gen, res, 1, 500 * MILLION, r, s, s);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingCorr, L = 30");
            __ckpt_commit_unit(89, __ckpt_jbefore_89, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(90)) {
            __ckpt_restore_unit(90, &j);
         } else {
            int __ckpt_jbefore_90 = j;
         sstring_HammingCorr (gen, res, 1, 50 * MILLION, r, s, 10 * s);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingCorr, L = 300");
            __ckpt_commit_unit(90, __ckpt_jbefore_90, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(91)) {
            __ckpt_restore_unit(91, &j);
         } else {
            int __ckpt_jbefore_91 = j;
         sstring_HammingCorr (gen, res, 1, 10 * MILLION, r, s, 40 * s);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingCorr, L = 1200");
            __ckpt_commit_unit(91, __ckpt_jbefore_91, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(92)) {
            __ckpt_restore_unit(92, &j);
         } else {
            int __ckpt_jbefore_92 = j;
         sstring_HammingIndep (gen, res, 1, 300 * MILLION, r, s, s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L = 30");
            __ckpt_commit_unit(92, __ckpt_jbefore_92, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(93)) {
            __ckpt_restore_unit(93, &j);
         } else {
            int __ckpt_jbefore_93 = j;
         sstring_HammingIndep (gen, res, 1, 100 * MILLION, 20, 10, s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L = 30");
            __ckpt_commit_unit(93, __ckpt_jbefore_93, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(94)) {
            __ckpt_restore_unit(94, &j);
         } else {
            int __ckpt_jbefore_94 = j;
         sstring_HammingIndep (gen, res, 1, 30 * MILLION, r, s, 10 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L = 300");
            __ckpt_commit_unit(94, __ckpt_jbefore_94, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(95)) {
            __ckpt_restore_unit(95, &j);
         } else {
            int __ckpt_jbefore_95 = j;
         sstring_HammingIndep (gen, res, 1, 10 * MILLION, 20, 10, 10 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L = 300");
            __ckpt_commit_unit(95, __ckpt_jbefore_95, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(96)) {
            __ckpt_restore_unit(96, &j);
         } else {
            int __ckpt_jbefore_96 = j;
         sstring_HammingIndep (gen, res, 1, 10 * MILLION, r, s, 40 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L = 1200");
            __ckpt_commit_unit(96, __ckpt_jbefore_96, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(97)) {
            __ckpt_restore_unit(97, &j);
         } else {
            int __ckpt_jbefore_97 = j;
         sstring_HammingIndep (gen, res, 1, MILLION, 20, 10, 40 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L = 1200");
            __ckpt_commit_unit(97, __ckpt_jbefore_97, j);
         }
      }
      sstring_DeleteRes (res);
   }
   {
      sstring_Res3 *res;
      res = sstring_CreateRes3 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(98)) {
            __ckpt_restore_unit(98, &j);
         } else {
            int __ckpt_jbefore_98 = j;
         sstring_Run (gen, res, 1, 1 * BILLION, r, s);
         bbattery_pVal[++j] = res->NRuns->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 0");
         bbattery_pVal[++j] = res->NBits->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 0");
            __ckpt_commit_unit(98, __ckpt_jbefore_98, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(99)) {
            __ckpt_restore_unit(99, &j);
         } else {
            int __ckpt_jbefore_99 = j;
         sstring_Run (gen, res, 1, 1 * BILLION, 20, 10);
         bbattery_pVal[++j] = res->NRuns->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 20");
         bbattery_pVal[++j] = res->NBits->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 20");
            __ckpt_commit_unit(99, __ckpt_jbefore_99, j);
         }
      }
      sstring_DeleteRes3 (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(100)) {
            __ckpt_restore_unit(100, &j);
         } else {
            int __ckpt_jbefore_100 = j;
         sstring_AutoCor (gen, res, 10, 30 + BILLION, r, s, 1);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d = 1");
            __ckpt_commit_unit(100, __ckpt_jbefore_100, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(101)) {
            __ckpt_restore_unit(101, &j);
         } else {
            int __ckpt_jbefore_101 = j;
         sstring_AutoCor (gen, res, 5, 1 + BILLION, 20, 10, 1);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d = 1");
            __ckpt_commit_unit(101, __ckpt_jbefore_101, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(102)) {
            __ckpt_restore_unit(102, &j);
         } else {
            int __ckpt_jbefore_102 = j;
         sstring_AutoCor (gen, res, 10, 31 + BILLION, r, s, s);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d = 30");
            __ckpt_commit_unit(102, __ckpt_jbefore_102, j);
         }
      }

      ++j2;
 /*     util_Assert (j2 <= CRUSH_NUM, "Crush:   j2 > CRUSH_NUM");  */
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(103)) {
            __ckpt_restore_unit(103, &j);
         } else {
            int __ckpt_jbefore_103 = j;
         sstring_AutoCor (gen, res, 5, 11 + BILLION, 20, 10, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d = 10");
            __ckpt_commit_unit(103, __ckpt_jbefore_103, j);
         }
      }
      sres_DeleteBasic (res);
   }

   bbattery_NTests = ++j;
   GetName (gen, genName);
   WriteReport (genName, "Crush", bbattery_NTests,
      bbattery_pVal, Timer, FALSE, TRUE, 0.0);
   chrono_Delete (Timer);
}


/*=========================================================================*/

static void BigCrush_Ckpt (unif01_Gen * gen, int Rep[])
/*
 * A battery of very stringent statistical tests for Random Number Generators
 * used in simulation.
 * Rep[i] gives the number of times that test i will be done. The default
 * values are Rep[i] = 1 for all i.
 */
{
   const int s = 30;
   const int r = 0;
   int i;
   chrono_Chrono *Timer;
   char genName[LEN + 1] = "";
   int j = -1;
   int j2 = 0;

   Timer = chrono_Create ();
   InitBat ();
   if (swrite_Basic) {
      printf ("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
         "                 Starting BigCrush\n"
         "                 Version: %s\n"
         "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n\n\n",
         PACKAGE_STRING);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(1)) {
            __ckpt_restore_unit(1, &j);
         } else {
            int __ckpt_jbefore_1 = j;
         smarsa_SerialOver (gen, res, 1, BILLION, 0, 256, 3);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SerialOver, r = 0");
            __ckpt_commit_unit(1, __ckpt_jbefore_1, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(2)) {
            __ckpt_restore_unit(2, &j);
         } else {
            int __ckpt_jbefore_2 = j;
         smarsa_SerialOver (gen, res, 1, BILLION, 22, 256, 3);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SerialOver, r = 22");
            __ckpt_commit_unit(2, __ckpt_jbefore_2, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      smarsa_Res *resm;
      resm = smarsa_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(3)) {
            __ckpt_restore_unit(3, &j);
         } else {
            int __ckpt_jbefore_3 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 0, 1024*1024*2, 2);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 2");
            __ckpt_commit_unit(3, __ckpt_jbefore_3, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(4)) {
            __ckpt_restore_unit(4, &j);
         } else {
            int __ckpt_jbefore_4 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 9, 1024*1024*2, 2);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 2");
            __ckpt_commit_unit(4, __ckpt_jbefore_4, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(5)) {
            __ckpt_restore_unit(5, &j);
         } else {
            int __ckpt_jbefore_5 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 0, 1024*16, 3);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 3");
            __ckpt_commit_unit(5, __ckpt_jbefore_5, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(6)) {
            __ckpt_restore_unit(6, &j);
         } else {
            int __ckpt_jbefore_6 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 16, 1024*16, 3);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 3");
            __ckpt_commit_unit(6, __ckpt_jbefore_6, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(7)) {
            __ckpt_restore_unit(7, &j);
         } else {
            int __ckpt_jbefore_7 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 0, 64, 7);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 7");
            __ckpt_commit_unit(7, __ckpt_jbefore_7, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(8)) {
            __ckpt_restore_unit(8, &j);
         } else {
            int __ckpt_jbefore_8 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 24, 64, 7);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 7");
            __ckpt_commit_unit(8, __ckpt_jbefore_8, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(9)) {
            __ckpt_restore_unit(9, &j);
         } else {
            int __ckpt_jbefore_9 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 0, 8, 14);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 14");
            __ckpt_commit_unit(9, __ckpt_jbefore_9, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(10)) {
            __ckpt_restore_unit(10, &j);
         } else {
            int __ckpt_jbefore_10 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 27, 8, 14);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 14");
            __ckpt_commit_unit(10, __ckpt_jbefore_10, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(11)) {
            __ckpt_restore_unit(11, &j);
         } else {
            int __ckpt_jbefore_11 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 0, 4, 21);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 21");
            __ckpt_commit_unit(11, __ckpt_jbefore_11, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(12)) {
            __ckpt_restore_unit(12, &j);
         } else {
            int __ckpt_jbefore_12 = j;
         smarsa_CollisionOver (gen, resm, 30, 20 * MILLION, 28, 4, 21);
         bbattery_pVal[++j] = resm->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionOver, t = 21");
            __ckpt_commit_unit(12, __ckpt_jbefore_12, j);
         }
      }
      smarsa_DeleteRes (resm);
   }
   {
      sres_Poisson *res;
      res = sres_CreatePoisson ();
#ifdef USE_LONGLONG
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(13)) {
            __ckpt_restore_unit(13, &j);
         } else {
            int __ckpt_jbefore_13 = j;
         long d;
#if LONG_MAX <= 2147483647L
         d = 1073741824L;
         smarsa_BirthdaySpacings (gen, res, 250, 4 * MILLION, 0, d, 2, 1);
#else
         d = 2147483648L;
         smarsa_BirthdaySpacings (gen, res, 100, 10 * MILLION, 0, d, 2, 1);
#endif
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 2");
            __ckpt_commit_unit(13, __ckpt_jbefore_13, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(14)) {
            __ckpt_restore_unit(14, &j);
         } else {
            int __ckpt_jbefore_14 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 20 * MILLION, 0, 2097152, 3,
            1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 3");
            __ckpt_commit_unit(14, __ckpt_jbefore_14, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(15)) {
            __ckpt_restore_unit(15, &j);
         } else {
            int __ckpt_jbefore_15 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 30 * MILLION, 14, 65536, 4, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 4");
            __ckpt_commit_unit(15, __ckpt_jbefore_15, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(16)) {
            __ckpt_restore_unit(16, &j);
         } else {
            int __ckpt_jbefore_16 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 20 * MILLION, 0, 512, 7, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 7");
            __ckpt_commit_unit(16, __ckpt_jbefore_16, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(17)) {
            __ckpt_restore_unit(17, &j);
         } else {
            int __ckpt_jbefore_17 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 20 * MILLION, 7, 512, 7, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 7");
            __ckpt_commit_unit(17, __ckpt_jbefore_17, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(18)) {
            __ckpt_restore_unit(18, &j);
         } else {
            int __ckpt_jbefore_18 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 30 * MILLION, 14, 256, 8, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 8");
            __ckpt_commit_unit(18, __ckpt_jbefore_18, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(19)) {
            __ckpt_restore_unit(19, &j);
         } else {
            int __ckpt_jbefore_19 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 30 * MILLION, 22, 256, 8, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 8");
            __ckpt_commit_unit(19, __ckpt_jbefore_19, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(20)) {
            __ckpt_restore_unit(20, &j);
         } else {
            int __ckpt_jbefore_20 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 30 * MILLION, 0, 16, 16, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 16");
            __ckpt_commit_unit(20, __ckpt_jbefore_20, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(21)) {
            __ckpt_restore_unit(21, &j);
         } else {
            int __ckpt_jbefore_21 = j;
         smarsa_BirthdaySpacings (gen, res, 20, 30 * MILLION, 26, 16, 16, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 16");
            __ckpt_commit_unit(21, __ckpt_jbefore_21, j);
         }
      }

#else
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(22)) {
            __ckpt_restore_unit(22, &j);
         } else {
            int __ckpt_jbefore_22 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 0,
            67108864, 2, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 2");
            __ckpt_commit_unit(22, __ckpt_jbefore_22, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(23)) {
            __ckpt_restore_unit(23, &j);
         } else {
            int __ckpt_jbefore_23 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 0,
            1024 * 8, 4, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 4");
            __ckpt_commit_unit(23, __ckpt_jbefore_23, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(24)) {
            __ckpt_restore_unit(24, &j);
         } else {
            int __ckpt_jbefore_24 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 16,
            1024 * 8, 4, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 4");
            __ckpt_commit_unit(24, __ckpt_jbefore_24, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(25)) {
            __ckpt_restore_unit(25, &j);
         } else {
            int __ckpt_jbefore_25 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 0, 16,
            13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(25, __ckpt_jbefore_25, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(26)) {
            __ckpt_restore_unit(26, &j);
         } else {
            int __ckpt_jbefore_26 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 5, 16,
            13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(26, __ckpt_jbefore_26, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(27)) {
            __ckpt_restore_unit(27, &j);
         } else {
            int __ckpt_jbefore_27 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 10,
            16, 13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(27, __ckpt_jbefore_27, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(28)) {
            __ckpt_restore_unit(28, &j);
         } else {
            int __ckpt_jbefore_28 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 15,
            16, 13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(28, __ckpt_jbefore_28, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(29)) {
            __ckpt_restore_unit(29, &j);
         } else {
            int __ckpt_jbefore_29 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 20,
            16, 13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(29, __ckpt_jbefore_29, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(30)) {
            __ckpt_restore_unit(30, &j);
         } else {
            int __ckpt_jbefore_30 = j;
         smarsa_BirthdaySpacings (gen, res, 10 * THOUSAND, MILLION / 10, 26,
            16, 13, 1);
         bbattery_pVal[++j] = res->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "BirthdaySpacings, t = 13");
            __ckpt_commit_unit(30, __ckpt_jbefore_30, j);
         }
      }
#endif
      sres_DeletePoisson (res);
   }
   {
      lebool flag = snpair_mNP2S_Flag;
      snpair_Res *res;
      res = snpair_CreateRes ();

      snpair_mNP2S_Flag = TRUE;
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(31)) {
            __ckpt_restore_unit(31, &j);
         } else {
            int __ckpt_jbefore_31 = j;
         snpair_ClosePairs (gen, res, 30, 6 * MILLION, 0, 3, 0, 30);
         GetPVal_CPairs (40, res, &j, ", t = 3", j2);
            __ckpt_commit_unit(31, __ckpt_jbefore_31, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(32)) {
            __ckpt_restore_unit(32, &j);
         } else {
            int __ckpt_jbefore_32 = j;
         snpair_ClosePairs (gen, res, 20, 4 * MILLION, 0, 5, 0, 30);
         GetPVal_CPairs (40, res, &j, ", t = 5", j2);
            __ckpt_commit_unit(32, __ckpt_jbefore_32, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(33)) {
            __ckpt_restore_unit(33, &j);
         } else {
            int __ckpt_jbefore_33 = j;
         snpair_ClosePairs (gen, res, 10, 3 * MILLION, 0, 9, 0, 30);
         GetPVal_CPairs (20, res, &j, ", t = 9", j2);
            __ckpt_commit_unit(33, __ckpt_jbefore_33, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(34)) {
            __ckpt_restore_unit(34, &j);
         } else {
            int __ckpt_jbefore_34 = j;
         snpair_ClosePairs (gen, res, 5, 2*MILLION, 0, 16, 0, 30);
         GetPVal_CPairs (10, res, &j, ", t = 16", j2);
            __ckpt_commit_unit(34, __ckpt_jbefore_34, j);
         }
      }
      snpair_DeleteRes (res);
      snpair_mNP2S_Flag =flag;
   }
   {
      sres_Chi2 *res;
      res = sres_CreateChi2 ();

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(35)) {
            __ckpt_restore_unit(35, &j);
         } else {
            int __ckpt_jbefore_35 = j;
         sknuth_SimpPoker (gen, res, 1, 400 * MILLION, 0, 8, 8);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, r = 0");
            __ckpt_commit_unit(35, __ckpt_jbefore_35, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(36)) {
            __ckpt_restore_unit(36, &j);
         } else {
            int __ckpt_jbefore_36 = j;
         sknuth_SimpPoker (gen, res, 1, 400 * MILLION, 27, 8, 8);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, r = 27");
            __ckpt_commit_unit(36, __ckpt_jbefore_36, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(37)) {
            __ckpt_restore_unit(37, &j);
         } else {
            int __ckpt_jbefore_37 = j;
         sknuth_SimpPoker (gen, res, 1, 100 * MILLION, 0, 32, 32);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, r = 0");
            __ckpt_commit_unit(37, __ckpt_jbefore_37, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(38)) {
            __ckpt_restore_unit(38, &j);
         } else {
            int __ckpt_jbefore_38 = j;
         sknuth_SimpPoker (gen, res, 1, 100 * MILLION, 25, 32, 32);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SimpPoker, r = 25");
            __ckpt_commit_unit(38, __ckpt_jbefore_38, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(39)) {
            __ckpt_restore_unit(39, &j);
         } else {
            int __ckpt_jbefore_39 = j;
         sknuth_CouponCollector (gen, res, 1, 200 * MILLION, 0, 8);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, r = 0");
            __ckpt_commit_unit(39, __ckpt_jbefore_39, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(40)) {
            __ckpt_restore_unit(40, &j);
         } else {
            int __ckpt_jbefore_40 = j;
         sknuth_CouponCollector (gen, res, 1, 200 * MILLION, 10, 8);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, r = 10");
            __ckpt_commit_unit(40, __ckpt_jbefore_40, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(41)) {
            __ckpt_restore_unit(41, &j);
         } else {
            int __ckpt_jbefore_41 = j;
         sknuth_CouponCollector (gen, res, 1, 200 * MILLION, 20, 8);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, r = 20");
            __ckpt_commit_unit(41, __ckpt_jbefore_41, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(42)) {
            __ckpt_restore_unit(42, &j);
         } else {
            int __ckpt_jbefore_42 = j;
         sknuth_CouponCollector (gen, res, 1, 200 * MILLION, 27, 8);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CouponCollector, r = 27");
            __ckpt_commit_unit(42, __ckpt_jbefore_42, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(43)) {
            __ckpt_restore_unit(43, &j);
         } else {
            int __ckpt_jbefore_43 = j;
         sknuth_Gap (gen, res, 1, BILLION/2, 0, 0.0, 1.0/16.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 0");
            __ckpt_commit_unit(43, __ckpt_jbefore_43, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(44)) {
            __ckpt_restore_unit(44, &j);
         } else {
            int __ckpt_jbefore_44 = j;
         sknuth_Gap (gen, res, 1, 300*MILLION, 25, 0.0, 1.0/32.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 25");
            __ckpt_commit_unit(44, __ckpt_jbefore_44, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(45)) {
            __ckpt_restore_unit(45, &j);
         } else {
            int __ckpt_jbefore_45 = j;
         sknuth_Gap (gen, res, 1, BILLION/10, 0, 0.0, 1.0/128.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 0");
            __ckpt_commit_unit(45, __ckpt_jbefore_45, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(46)) {
            __ckpt_restore_unit(46, &j);
         } else {
            int __ckpt_jbefore_46 = j;
         sknuth_Gap (gen, res, 1, 10*MILLION, 20, 0.0, 1.0/1024.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Gap, r = 20");
            __ckpt_commit_unit(46, __ckpt_jbefore_46, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(47)) {
            __ckpt_restore_unit(47, &j);
         } else {
            int __ckpt_jbefore_47 = j;
         sknuth_Run (gen, res, 5, BILLION, 0, FALSE);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run, r = 0");
            __ckpt_commit_unit(47, __ckpt_jbefore_47, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(48)) {
            __ckpt_restore_unit(48, &j);
         } else {
            int __ckpt_jbefore_48 = j;
         sknuth_Run (gen, res, 10, BILLION, 15, TRUE);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run, r = 15");
            __ckpt_commit_unit(48, __ckpt_jbefore_48, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(49)) {
            __ckpt_restore_unit(49, &j);
         } else {
            int __ckpt_jbefore_49 = j;
         sknuth_Permutation (gen, res, 1, BILLION, 5, 3);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Permutation, t = 3" );
            __ckpt_commit_unit(49, __ckpt_jbefore_49, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(50)) {
            __ckpt_restore_unit(50, &j);
         } else {
            int __ckpt_jbefore_50 = j;
         sknuth_Permutation (gen, res, 1, BILLION, 5, 5);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Permutation, t = 5");
            __ckpt_commit_unit(50, __ckpt_jbefore_50, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(51)) {
            __ckpt_restore_unit(51, &j);
         } else {
            int __ckpt_jbefore_51 = j;
         sknuth_Permutation (gen, res, 1, BILLION/2, 5, 7);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Permutation, t = 7");
            __ckpt_commit_unit(51, __ckpt_jbefore_51, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(52)) {
            __ckpt_restore_unit(52, &j);
         } else {
            int __ckpt_jbefore_52 = j;
         sknuth_Permutation (gen, res, 1, BILLION/2, 10, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Permutation, t = 10");
            __ckpt_commit_unit(52, __ckpt_jbefore_52, j);
         }
      }
      sres_DeleteChi2 (res);
   }
   {
      sknuth_Res2 *res;
      res = sknuth_CreateRes2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(53)) {
            __ckpt_restore_unit(53, &j);
         } else {
            int __ckpt_jbefore_53 = j;
         sknuth_CollisionPermut (gen, res, 20, 20 * MILLION, 0, 14);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionPermut, r = 0");
            __ckpt_commit_unit(53, __ckpt_jbefore_53, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(54)) {
            __ckpt_restore_unit(54, &j);
         } else {
            int __ckpt_jbefore_54 = j;
         sknuth_CollisionPermut (gen, res, 20, 20 * MILLION, 10, 14);
         bbattery_pVal[++j] = res->Pois->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "CollisionPermut, r = 10");
            __ckpt_commit_unit(54, __ckpt_jbefore_54, j);
         }
      }
      sknuth_DeleteRes2 (res);
   }
   {
      sknuth_Res1 *res;
      res = sknuth_CreateRes1 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(55)) {
            __ckpt_restore_unit(55, &j);
         } else {
            int __ckpt_jbefore_55 = j;
         sknuth_MaxOft (gen, res, 40, 10 * MILLION, 0, MILLION / 10, 8);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 8");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 8");
            __ckpt_commit_unit(55, __ckpt_jbefore_55, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(56)) {
            __ckpt_restore_unit(56, &j);
         } else {
            int __ckpt_jbefore_56 = j;
         sknuth_MaxOft (gen, res, 30, 10 * MILLION, 0, MILLION / 10, 16);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 16");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 16");
            __ckpt_commit_unit(56, __ckpt_jbefore_56, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(57)) {
            __ckpt_restore_unit(57, &j);
         } else {
            int __ckpt_jbefore_57 = j;
         sknuth_MaxOft (gen, res, 20, 10 * MILLION, 0, MILLION / 10, 24);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 24");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 24");
            __ckpt_commit_unit(57, __ckpt_jbefore_57, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(58)) {
            __ckpt_restore_unit(58, &j);
         } else {
            int __ckpt_jbefore_58 = j;
         sknuth_MaxOft (gen, res, 20, 10 * MILLION, 0, MILLION / 10, 32);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft, t = 32");
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MaxOft AD, t = 32");
            __ckpt_commit_unit(58, __ckpt_jbefore_58, j);
         }
      }
      sknuth_DeleteRes1 (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(59)) {
            __ckpt_restore_unit(59, &j);
         } else {
            int __ckpt_jbefore_59 = j;
         svaria_SampleProd (gen, res, 40, 10 * MILLION, 0, 8);
         bbattery_pVal[++j] = res->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleProd, t = 8");
            __ckpt_commit_unit(59, __ckpt_jbefore_59, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(60)) {
            __ckpt_restore_unit(60, &j);
         } else {
            int __ckpt_jbefore_60 = j;
         svaria_SampleProd (gen, res, 20, 10*MILLION, 0, 16);
         bbattery_pVal[++j] = res->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleProd, t = 16");
            __ckpt_commit_unit(60, __ckpt_jbefore_60, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(61)) {
            __ckpt_restore_unit(61, &j);
         } else {
            int __ckpt_jbefore_61 = j;
         svaria_SampleProd (gen, res, 20, 10*MILLION, 0, 24);
         bbattery_pVal[++j] = res->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleProd, t = 24");
            __ckpt_commit_unit(61, __ckpt_jbefore_61, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(62)) {
            __ckpt_restore_unit(62, &j);
         } else {
            int __ckpt_jbefore_62 = j;
         svaria_SampleMean (gen, res, 20*MILLION, 30, 0);
         bbattery_pVal[++j] = res->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleMean, r = 0");
            __ckpt_commit_unit(62, __ckpt_jbefore_62, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(63)) {
            __ckpt_restore_unit(63, &j);
         } else {
            int __ckpt_jbefore_63 = j;
         svaria_SampleMean (gen, res, 20*MILLION, 30, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleMean, r = 10");
            __ckpt_commit_unit(63, __ckpt_jbefore_63, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(64)) {
            __ckpt_restore_unit(64, &j);
         } else {
            int __ckpt_jbefore_64 = j;
         svaria_SampleCorr (gen, res, 1, 2*BILLION, 0, 1);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleCorr, k = 1");
            __ckpt_commit_unit(64, __ckpt_jbefore_64, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(65)) {
            __ckpt_restore_unit(65, &j);
         } else {
            int __ckpt_jbefore_65 = j;
         svaria_SampleCorr (gen, res, 1, 2*BILLION, 0, 2);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SampleCorr, k = 2");
            __ckpt_commit_unit(65, __ckpt_jbefore_65, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(66)) {
            __ckpt_restore_unit(66, &j);
         } else {
            int __ckpt_jbefore_66 = j;
         svaria_AppearanceSpacings (gen, res, 1, 10 * MILLION, BILLION,
            r, 3, 15);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AppearanceSpacings, r = 0");
            __ckpt_commit_unit(66, __ckpt_jbefore_66, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(67)) {
            __ckpt_restore_unit(67, &j);
         } else {
            int __ckpt_jbefore_67 = j;
         svaria_AppearanceSpacings (gen, res, 1, 10 * MILLION, BILLION,
            27, 3, 15);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AppearanceSpacings, r = 27");
            __ckpt_commit_unit(67, __ckpt_jbefore_67, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      smarsa_Res2 *res2;
      sres_Chi2 *res;
      res = sres_CreateChi2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(68)) {
            __ckpt_restore_unit(68, &j);
         } else {
            int __ckpt_jbefore_68 = j;
         svaria_WeightDistrib (gen, res, 1, 20 * MILLION, 0, 256, 0.0, 0.25);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 0");
            __ckpt_commit_unit(68, __ckpt_jbefore_68, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(69)) {
            __ckpt_restore_unit(69, &j);
         } else {
            int __ckpt_jbefore_69 = j;
         svaria_WeightDistrib (gen, res, 1, 20 * MILLION, 20, 256, 0.0, 0.25);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 20");
            __ckpt_commit_unit(69, __ckpt_jbefore_69, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(70)) {
            __ckpt_restore_unit(70, &j);
         } else {
            int __ckpt_jbefore_70 = j;
         svaria_WeightDistrib (gen, res, 1, 20 * MILLION, 28, 256, 0.0, 0.25);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 28");
            __ckpt_commit_unit(70, __ckpt_jbefore_70, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(71)) {
            __ckpt_restore_unit(71, &j);
         } else {
            int __ckpt_jbefore_71 = j;
         svaria_WeightDistrib (gen, res, 1, 20 * MILLION, 0, 256, 0.0, 0.0625);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 0");
            __ckpt_commit_unit(71, __ckpt_jbefore_71, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(72)) {
            __ckpt_restore_unit(72, &j);
         } else {
            int __ckpt_jbefore_72 = j;
         svaria_WeightDistrib (gen, res, 1, 20 * MILLION, 10, 256, 0.0, 0.0625);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 10");
            __ckpt_commit_unit(72, __ckpt_jbefore_72, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(73)) {
            __ckpt_restore_unit(73, &j);
         } else {
            int __ckpt_jbefore_73 = j;
         svaria_WeightDistrib (gen, res, 1, 20 * MILLION, 26, 256, 0.0, 0.0625);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "WeightDistrib, r = 26");
            __ckpt_commit_unit(73, __ckpt_jbefore_73, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(74)) {
            __ckpt_restore_unit(74, &j);
         } else {
            int __ckpt_jbefore_74 = j;
         svaria_SumCollector (gen, res, 1, 500 * MILLION, 0, 10.0);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "SumCollector");
            __ckpt_commit_unit(74, __ckpt_jbefore_74, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(75)) {
            __ckpt_restore_unit(75, &j);
         } else {
            int __ckpt_jbefore_75 = j;
         smarsa_MatrixRank (gen, res, 10, MILLION, r, 5, 30, 30);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, L=30, r=0");
            __ckpt_commit_unit(75, __ckpt_jbefore_75, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(76)) {
            __ckpt_restore_unit(76, &j);
         } else {
            int __ckpt_jbefore_76 = j;
         smarsa_MatrixRank (gen, res, 10, MILLION, 25, 5, 30, 30);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, L=30, r=26");
            __ckpt_commit_unit(76, __ckpt_jbefore_76, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(77)) {
            __ckpt_restore_unit(77, &j);
         } else {
            int __ckpt_jbefore_77 = j;
         smarsa_MatrixRank (gen, res, 1, 5 * THOUSAND, r, 4, 1000, 1000);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, L=1000, r=0");
            __ckpt_commit_unit(77, __ckpt_jbefore_77, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(78)) {
            __ckpt_restore_unit(78, &j);
         } else {
            int __ckpt_jbefore_78 = j;
         smarsa_MatrixRank (gen, res, 1, 5 * THOUSAND, 26, 4, 1000, 1000);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, L=1000, r=26");
            __ckpt_commit_unit(78, __ckpt_jbefore_78, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(79)) {
            __ckpt_restore_unit(79, &j);
         } else {
            int __ckpt_jbefore_79 = j;
         smarsa_MatrixRank (gen, res, 1, 80, 15, 15, 5000, 5000);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, L=5000");
            __ckpt_commit_unit(79, __ckpt_jbefore_79, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(80)) {
            __ckpt_restore_unit(80, &j);
         } else {
            int __ckpt_jbefore_80 = j;
         smarsa_MatrixRank (gen, res, 1, 80, 0, 30, 5000, 5000);
         bbattery_pVal[++j] = res->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "MatrixRank, L=5000");
            __ckpt_commit_unit(80, __ckpt_jbefore_80, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(81)) {
            __ckpt_restore_unit(81, &j);
         } else {
            int __ckpt_jbefore_81 = j;
         smarsa_Savir2 (gen, res, 10, 10 * MILLION, 10, 1024*1024, 30);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Savir2");
            __ckpt_commit_unit(81, __ckpt_jbefore_81, j);
         }
      }
      sres_DeleteChi2 (res);

      res2 = smarsa_CreateRes2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(82)) {
            __ckpt_restore_unit(82, &j);
         } else {
            int __ckpt_jbefore_82 = j;
         smarsa_GCD (gen, res2, 10, 50 * MILLION, 0, 30);
         bbattery_pVal[++j] = res2->GCD->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "GCD");
            __ckpt_commit_unit(82, __ckpt_jbefore_82, j);
         }
      }
      smarsa_DeleteRes2 (res2);
   }
   {
      swalk_Res *res;
      res = swalk_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(83)) {
            __ckpt_restore_unit(83, &j);
         } else {
            int __ckpt_jbefore_83 = j;
         swalk_RandomWalk1 (gen, res, 1, 100 * MILLION, r, 5, 50, 50);
         GetPVal_Walk (1, res, &j, " (L=50, r=0)", j2);
            __ckpt_commit_unit(83, __ckpt_jbefore_83, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(84)) {
            __ckpt_restore_unit(84, &j);
         } else {
            int __ckpt_jbefore_84 = j;
         swalk_RandomWalk1 (gen, res, 1, 100 * MILLION, 25, 5, 50, 50);
         GetPVal_Walk (1, res, &j, " (L=50, r=25)", j2);
            __ckpt_commit_unit(84, __ckpt_jbefore_84, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(85)) {
            __ckpt_restore_unit(85, &j);
         } else {
            int __ckpt_jbefore_85 = j;
         swalk_RandomWalk1 (gen, res, 1, 10 * MILLION, r, 10, 1000, 1000);
         GetPVal_Walk (1, res, &j, " (L=1000, r=0)", j2);
            __ckpt_commit_unit(85, __ckpt_jbefore_85, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(86)) {
            __ckpt_restore_unit(86, &j);
         } else {
            int __ckpt_jbefore_86 = j;
         swalk_RandomWalk1 (gen, res, 1, 10 * MILLION, 20, 10, 1000, 1000);
         GetPVal_Walk (1, res, &j, " (L=1000, r=20)", j2);
            __ckpt_commit_unit(86, __ckpt_jbefore_86, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(87)) {
            __ckpt_restore_unit(87, &j);
         } else {
            int __ckpt_jbefore_87 = j;
         swalk_RandomWalk1 (gen, res, 1, 1 * MILLION, r, 15, 10000, 10000);
         GetPVal_Walk (1, res, &j, " (L=10000, r=0)", j2);
            __ckpt_commit_unit(87, __ckpt_jbefore_87, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(88)) {
            __ckpt_restore_unit(88, &j);
         } else {
            int __ckpt_jbefore_88 = j;
         swalk_RandomWalk1 (gen, res, 1, 1 * MILLION, 15, 15, 10000, 10000);
         GetPVal_Walk (1, res, &j, " (L=10000, r=15)", j2);
            __ckpt_commit_unit(88, __ckpt_jbefore_88, j);
         }
      }
      swalk_DeleteRes (res);
   }
   {
      scomp_Res *res;
      res = scomp_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(89)) {
            __ckpt_restore_unit(89, &j);
         } else {
            int __ckpt_jbefore_89 = j;
         scomp_LinearComp (gen, res, 1, 400 * THOUSAND + 20, r, 1);
         bbattery_pVal[++j] = res->JumpNum->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 0");
         bbattery_pVal[++j] = res->JumpSize->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 0");
            __ckpt_commit_unit(89, __ckpt_jbefore_89, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(90)) {
            __ckpt_restore_unit(90, &j);
         } else {
            int __ckpt_jbefore_90 = j;
         scomp_LinearComp (gen, res, 1, 400 * THOUSAND + 20, 29, 1);
         bbattery_pVal[++j] = res->JumpNum->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 29");
         bbattery_pVal[++j] = res->JumpSize->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LinearComp, r = 0");
            __ckpt_commit_unit(90, __ckpt_jbefore_90, j);
         }
      }
      scomp_DeleteRes (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(91)) {
            __ckpt_restore_unit(91, &j);
         } else {
            int __ckpt_jbefore_91 = j;
         scomp_LempelZiv (gen, res, 10, 27, r, s);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LempelZiv, r = 0");
            __ckpt_commit_unit(91, __ckpt_jbefore_91, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(92)) {
            __ckpt_restore_unit(92, &j);
         } else {
            int __ckpt_jbefore_92 = j;
         scomp_LempelZiv (gen, res, 10, 27, 15, 15);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LempelZiv, r = 15");
            __ckpt_commit_unit(92, __ckpt_jbefore_92, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      sspectral_Res *res;
      res = sspectral_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(93)) {
            __ckpt_restore_unit(93, &j);
         } else {
            int __ckpt_jbefore_93 = j;
         sspectral_Fourier3 (gen, res, 100 * THOUSAND, 14, r, 3);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Fourier3, r = 0");
            __ckpt_commit_unit(93, __ckpt_jbefore_93, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(94)) {
            __ckpt_restore_unit(94, &j);
         } else {
            int __ckpt_jbefore_94 = j;
         sspectral_Fourier3 (gen, res, 100 * THOUSAND, 14, 27, 3);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_AD];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Fourier3, r = 27");
            __ckpt_commit_unit(94, __ckpt_jbefore_94, j);
         }
      }
      sspectral_DeleteRes (res);
   }
   {
      sstring_Res2 *res;
      res = sstring_CreateRes2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(95)) {
            __ckpt_restore_unit(95, &j);
         } else {
            int __ckpt_jbefore_95 = j;
         sstring_LongestHeadRun (gen, res, 1, 1000, r, 3, 20 + 10 * MILLION);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 0");
         bbattery_pVal[++j] = res->Disc->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 0");
            __ckpt_commit_unit(95, __ckpt_jbefore_95, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(96)) {
            __ckpt_restore_unit(96, &j);
         } else {
            int __ckpt_jbefore_96 = j;
         sstring_LongestHeadRun (gen, res, 1, 1000, 27, 3, 20 + 10 * MILLION);
         bbattery_pVal[++j] = res->Chi->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 27");
         bbattery_pVal[++j] = res->Disc->pVal2;
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "LongestHeadRun, r = 27");
            __ckpt_commit_unit(96, __ckpt_jbefore_96, j);
         }
      }
      sstring_DeleteRes2 (res);
   }
   {
      sres_Chi2 *res;
      res = sres_CreateChi2 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(97)) {
            __ckpt_restore_unit(97, &j);
         } else {
            int __ckpt_jbefore_97 = j;
         sstring_PeriodsInStrings (gen, res, 10, BILLION/2, r, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "PeriodsInStrings, r = 0");
            __ckpt_commit_unit(97, __ckpt_jbefore_97, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(98)) {
            __ckpt_restore_unit(98, &j);
         } else {
            int __ckpt_jbefore_98 = j;
         sstring_PeriodsInStrings (gen, res, 10, BILLION/2, 20, 10);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "PeriodsInStrings, r = 20");
            __ckpt_commit_unit(98, __ckpt_jbefore_98, j);
         }
      }
      sres_DeleteChi2 (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(99)) {
            __ckpt_restore_unit(99, &j);
         } else {
            int __ckpt_jbefore_99 = j;
         sstring_HammingWeight2 (gen, res, 10, BILLION, r, 3, MILLION);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingWeight2, r = 0");
            __ckpt_commit_unit(99, __ckpt_jbefore_99, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(100)) {
            __ckpt_restore_unit(100, &j);
         } else {
            int __ckpt_jbefore_100 = j;
         sstring_HammingWeight2 (gen, res, 10, BILLION, 27, 3, MILLION);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingWeight2, r = 27");
            __ckpt_commit_unit(100, __ckpt_jbefore_100, j);
         }
      }
      sres_DeleteBasic (res);
   }
   {
      sstring_Res *res;
      res = sstring_CreateRes ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(101)) {
            __ckpt_restore_unit(101, &j);
         } else {
            int __ckpt_jbefore_101 = j;
         sstring_HammingCorr (gen, res, 1, BILLION, 10, 10, s);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingCorr, L = 30");
            __ckpt_commit_unit(101, __ckpt_jbefore_101, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(102)) {
            __ckpt_restore_unit(102, &j);
         } else {
            int __ckpt_jbefore_102 = j;
         sstring_HammingCorr (gen, res, 1, 100 * MILLION, 10, 10, 10 * s);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingCorr, L = 300");
            __ckpt_commit_unit(102, __ckpt_jbefore_102, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(103)) {
            __ckpt_restore_unit(103, &j);
         } else {
            int __ckpt_jbefore_103 = j;
         sstring_HammingCorr (gen, res, 1, 100 * MILLION, 10, 10, 40 * s);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingCorr, L = 1200");
            __ckpt_commit_unit(103, __ckpt_jbefore_103, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(104)) {
            __ckpt_restore_unit(104, &j);
         } else {
            int __ckpt_jbefore_104 = j;
         sstring_HammingIndep (gen, res, 10, 30 * MILLION, r, 3, s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L=30, r=0");
            __ckpt_commit_unit(104, __ckpt_jbefore_104, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(105)) {
            __ckpt_restore_unit(105, &j);
         } else {
            int __ckpt_jbefore_105 = j;
         sstring_HammingIndep (gen, res, 10, 30 * MILLION, 27, 3, s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L=30, r=27");
            __ckpt_commit_unit(105, __ckpt_jbefore_105, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(106)) {
            __ckpt_restore_unit(106, &j);
         } else {
            int __ckpt_jbefore_106 = j;
         sstring_HammingIndep (gen, res, 1, 30 * MILLION, r, 4, 10 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L=300, r=0");
            __ckpt_commit_unit(106, __ckpt_jbefore_106, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(107)) {
            __ckpt_restore_unit(107, &j);
         } else {
            int __ckpt_jbefore_107 = j;
         sstring_HammingIndep (gen, res, 1, 30 * MILLION, 26, 4, 10 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L=300, r=26");
            __ckpt_commit_unit(107, __ckpt_jbefore_107, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(108)) {
            __ckpt_restore_unit(108, &j);
         } else {
            int __ckpt_jbefore_108 = j;
         sstring_HammingIndep (gen, res, 1, 10 * MILLION, r, 5, 40 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L=1200, r=0");
            __ckpt_commit_unit(108, __ckpt_jbefore_108, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(109)) {
            __ckpt_restore_unit(109, &j);
         } else {
            int __ckpt_jbefore_109 = j;
         sstring_HammingIndep (gen, res, 1, 10 * MILLION, 25, 5, 40 * s, 0);
         bbattery_pVal[++j] = res->Bas->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "HammingIndep, L=1200, r=25");
            __ckpt_commit_unit(109, __ckpt_jbefore_109, j);
         }
      }
      sstring_DeleteRes (res);
   }
   {
      sstring_Res3 *res;
      res = sstring_CreateRes3 ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(110)) {
            __ckpt_restore_unit(110, &j);
         } else {
            int __ckpt_jbefore_110 = j;
         sstring_Run (gen, res, 1, 2*BILLION, r, 3);
         bbattery_pVal[++j] = res->NRuns->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 0");
         bbattery_pVal[++j] = res->NBits->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 0");
            __ckpt_commit_unit(110, __ckpt_jbefore_110, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(111)) {
            __ckpt_restore_unit(111, &j);
         } else {
            int __ckpt_jbefore_111 = j;
         sstring_Run (gen, res, 1, 2*BILLION, 27, 3);
         bbattery_pVal[++j] = res->NRuns->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 27");
         bbattery_pVal[++j] = res->NBits->pVal2[gofw_Mean];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "Run of bits, r = 27");
            __ckpt_commit_unit(111, __ckpt_jbefore_111, j);
         }
      }
      sstring_DeleteRes3 (res);
   }
   {
      sres_Basic *res;
      res = sres_CreateBasic ();
      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(112)) {
            __ckpt_restore_unit(112, &j);
         } else {
            int __ckpt_jbefore_112 = j;
         sstring_AutoCor (gen, res, 10, 30 + BILLION, r, 3, 1);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d=1, r=0");
            __ckpt_commit_unit(112, __ckpt_jbefore_112, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(113)) {
            __ckpt_restore_unit(113, &j);
         } else {
            int __ckpt_jbefore_113 = j;
         sstring_AutoCor (gen, res, 10, 30 + BILLION, r, 3, 3);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d=3, r=0");
            __ckpt_commit_unit(113, __ckpt_jbefore_113, j);
         }
      }

      ++j2;
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(114)) {
            __ckpt_restore_unit(114, &j);
         } else {
            int __ckpt_jbefore_114 = j;
         sstring_AutoCor (gen, res, 10, 30 + BILLION, 27, 3, 1);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d=1, r=27");
            __ckpt_commit_unit(114, __ckpt_jbefore_114, j);
         }
      }

      ++j2;
      util_Assert (j2 <= BIGCRUSH_NUM, "BigCrush:   j2 > BIGCRUSH_NUM");
      for (i = 0; i < Rep[j2]; ++i) {
         if (__ckpt_should_skip(115)) {
            __ckpt_restore_unit(115, &j);
         } else {
            int __ckpt_jbefore_115 = j;
         sstring_AutoCor (gen, res, 10, 30 + BILLION, 27, 3, 3);
         bbattery_pVal[++j] = res->pVal2[gofw_Sum];
         TestNumber[j] = j2;
         strcpy (bbattery_TestNames[j], "AutoCor, d=3, r=27");
            __ckpt_commit_unit(115, __ckpt_jbefore_115, j);
         }
      }
      sres_DeleteBasic (res);
   }

   bbattery_NTests = ++j;
   GetName (gen, genName);
   WriteReport (genName, "BigCrush", bbattery_NTests, bbattery_pVal,
      Timer, FALSE, TRUE, 0.0);
   chrono_Delete (Timer);
}

/* ------------------------------------------------------------------ *
 * Entry points mirroring bbattery_Crush()/bbattery_BigCrush() exactly --
 * same Rep[] shape (see header comment's "TEST COUNT" section).
 * ------------------------------------------------------------------ */
void bbattery_CrushCkpt(unif01_Gen *gen) {
    int i;
    int Rep[NDIM + 1] = {0};
    for (i = 1; i <= CRUSH_NUM; ++i) Rep[i] = 1;
    Crush_Ckpt(gen, Rep);
}

void bbattery_BigCrushCkpt(unif01_Gen *gen) {
    int i;
    int Rep[NDIM + 1] = {0};
    for (i = 1; i <= BIGCRUSH_NUM; ++i) Rep[i] = 1;
    BigCrush_Ckpt(gen, Rep);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <hi|lo|hi_rev|lo_rev> <crush|bigcrush> <ckpt_file>\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const char *battery = argv[2];
    const char *ckpt_file = argv[3];

    if (strcmp(mode, "hi") && strcmp(mode, "lo") && strcmp(mode, "hi_rev") && strcmp(mode, "lo_rev")) {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }
    if (strcmp(battery, "crush") && strcmp(battery, "bigcrush")) {
        fprintf(stderr, "unknown battery: %s (only crush|bigcrush support checkpointing here -- "
                "smallcrush completes in seconds and doesn't need it; use "
                "pgprng_testu01_wrapper for smallcrush)\n", battery);
        return 2;
    }

    strncpy(g_ckpt_path, ckpt_file, sizeof(g_ckpt_path) - 1);
    strncpy(g_ckpt_mode, mode, sizeof(g_ckpt_mode) - 1);
    strncpy(g_ckpt_battery, battery, sizeof(g_ckpt_battery) - 1);

    ckpt_load(ckpt_file, battery, mode);

    unsigned int (*getbits)(void) = NULL;
    if (!strcmp(mode, "hi")) getbits = GetBits_hi;
    else if (!strcmp(mode, "lo")) getbits = GetBits_lo;
    else if (!strcmp(mode, "hi_rev")) getbits = GetBits_hi_rev;
    else if (!strcmp(mode, "lo_rev")) getbits = GetBits_lo_rev;

    char name[128];
    snprintf(name, sizeof(name), "pgprng_v2.3(%s)", mode);
    unif01_Gen *gen = unif01_CreateExternGenBits(name, getbits);

    if (!strcmp(battery, "crush")) {
        bbattery_CrushCkpt(gen);
    } else {
        bbattery_BigCrushCkpt(gen);
    }

    fprintf(stderr, "pgprng_testu01_ckpt_driver: consumed %ld 64-bit words total "
            "for mode=%s battery=%s.\n", g_words_consumed, mode, battery);
    unif01_DeleteExternGenBits(gen);
    return 0;
}
