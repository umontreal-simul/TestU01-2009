/* bbattery_ckpt_internal.h
 *
 * Exposes a handful of bbattery.c internals (previously file-static) that
 * the checkpointed Crush/BigCrush driver (pgprng_testu01_ckpt_driver.c)
 * needs to reuse verbatim: the per-test-number bookkeeping array and the
 * exact same init/report/name-formatting helpers bbattery.c's own
 * bbattery_Crush()/bbattery_BigCrush() use, so the checkpointed battery's
 * output format is identical to the stock, non-checkpointed one.
 *
 * The corresponding change in bbattery.c is exactly six `static` keywords
 * removed (TestNumber, GetName, WriteReport, InitBat, GetPVal_Walk,
 * GetPVal_CPairs) -- no other line in bbattery.c was touched. See
 * testu01_checkpointing_visibility.patch for the exact diff.
 *
 * Caller must already have #included unif01.h and chrono.h (for lebool
 * and chrono_Chrono) before this header -- it adds no includes of its own
 * so it can be dropped into any translation unit that already pulls in
 * the standard bbattery.c include set.
 */
#ifndef BBATTERY_CKPT_INTERNAL_H
#define BBATTERY_CKPT_INTERNAL_H

extern int TestNumber[];

void GetName (unif01_Gen * gen, char *genName);

void WriteReport (
   char *genName,
   char *batName,
   int N,
   double pVal[],
   chrono_Chrono * Timer,
   lebool Flag,
   lebool VersionFlag,
   double nb
   );

void InitBat (void);


void GetPVal_Walk (long N, swalk_Res * res, int *pj, char *mess, int j2);

void GetPVal_CPairs (long N, snpair_Res * res, int *pj, char *mess, int j2);

#endif
