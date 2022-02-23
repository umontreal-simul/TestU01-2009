/* After compiling TESTU01 and adding the libraries to your paths,
 * compile this program with
 * cc -O3 testxoshiro128plusplus.c xoshiro128plusplus.c -o \
 *    testxoshiro128plusplus -ltestu01 -lprobdist -lmylib -lm
 * then run it using ./testxoshiro128plusplus
 * You can change the code to replace bbattery_SmallCrush, with
 * bbattery_Crush and bbattery_BigCrush
 */

#include "unif01.h"
#include "bbattery.h"

uint32_t xoshiro128plusplus (void);

int main (void)
{
	unif01_Gen *gen;

	gen = unif01_CreateExternGenBits("xoshiro128plusplus", xoshiro128plusplus);
	bbattery_SmallCrush (gen);
	unif01_DeleteExternGenBits (gen);

	return 0;
}
