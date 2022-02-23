/* After compiling TESTU01 and adding the libraries to your paths,
 * compile this program with
 * cc -O3 testpcg32.c pcg32.c -o testpcg32 -ltestu01 -lprobdist -lmylib -lm
 * then run it using ./testpcg32.c
 * You can change the code to replace bbattery_SmallCrush, with
 * bbattery_Crush and bbattery_BigCrush
 */

#include "unif01.h"
#include "bbattery.h"

uint32_t pcg32_random_r (void);

int main (void)
{
	unif01_Gen *gen;

	gen = unif01_CreateExternGenBits("pcg32_random_r", pcg32_random_r);
	bbattery_SmallCrush (gen);
	unif01_DeleteExternGenBits (gen);

	return 0;
}
