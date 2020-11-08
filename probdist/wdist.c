/*************************************************************************\
 *
 * Package:        ProbDist
 * File:           wdist.c
 * Environment:    ANSI C
 *
 * Copyright (c) 2002 Pierre L'Ecuyer, DIRO, Université de Montréal.
 * e-mail: lecuyer@iro.umontreal.ca
 * All rights reserved.
 *
 * This software is provided under the Apache 2 License.
 *
 * In scientific publications which used this software, a reference to it
 * would be appreciated.
 *
\*************************************************************************/

#include "wdist.h"
#include "fdist.h"



/*=========================================================================*/

double wdist_Normal (double Junk[], double x)
{
   return fdist_Normal2 (x);
}


double wdist_ChiSquare (double W[], double x)
{
   long N = (long) W[0];
   return fdist_ChiSquare2 (N, 12, x);
}


double wdist_Unif (double Junk[], double x)
{
   return fdist_Unif (x);
}

