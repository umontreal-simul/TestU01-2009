/* After compiling TESTU01 and adding the libraries to your paths,
 * compile this program with
 * cc -O3 speedpcg32.c pcg32.c -o speedpcg32 -ltestu01 -lprobdist -lmylib -lm
 * then run it using ./speedpcg32.c
 */

#include "unif01.h"
#include "ulec.h"

uint32_t pcg32_random_r (void);

int main (void)
{
	unif01_Gen *gen;

	gen = unif01_CreateExternGenBits("pcg32_random_r", pcg32_random_r);
	unif01_TimerSumGenWr (gen, 100000000, FALSE);
	ulec_DeleteGen (gen);

	return 0;
}
