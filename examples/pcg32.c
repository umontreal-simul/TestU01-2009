#include "stdint.h"

// This random number generation algorithm is taken from
// M.E. O'Neil  "PCG: A Family of Simple Fast Space-Efficient"
//              "Statistically Good Algorithms for Random Number" 
//              "Generation"
// https://www.cs.hmc.edu/tr/hmc-cs-2014-0905.pdf
// https://www.pcg-random.org/download.html
//
// *Really* minimal PCG32 code
// (c) 2014 M.E. O'Neill
// pcg-random.org
// Licensed under Apache License 2.0 (NO WARRANTY, etc. see website)

static uint64_t state = 5342;
static uint64_t inc = 521;
uint32_t pcg32_random_r(void)
{
    uint64_t oldstate = state;
    // Advance internal state
    state = oldstate * 6364136223846793005ULL + (inc|1);
    // Calculate output function (XSH RR), uses old state for
    // maximum instruction level parallelism
    uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}
