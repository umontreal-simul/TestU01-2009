/*************************************************************************\
 *
 * Package:        MyLib
 * File:           gdef.c
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gdef.h"
#ifdef HAVE_WINDOWS_H
   #include <windows.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>

#else
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#endif



void gdef_GetHostName (char machine[], int n)
{
   int j;
#ifdef HAVE_SYS_UTSNAME_H
   struct utsname Z;
#else
   char *name;
#endif

   if (n <= 0 || machine == NULL)
      return;
   machine[0] = '\0';

#ifdef HAVE_SYS_UTSNAME_H
   if (uname(&Z) != -1) {
      strncpy (machine, Z.nodename, (size_t) n);
      j = strlen (machine);
      if (n - j > 2)
         strncat (machine, ", ", (size_t) 2);
      j = strlen (machine);
      if (n - j > 0)
         strncat (machine, Z.sysname, (size_t) (n - j));
      machine[n - 1] = '\0';
      return;   
   }

#else
#ifdef HAVE_UNISTD_H
   gethostname (machine, (size_t) n);
   machine[n - 1] = '\0';
   return;   

#else
   name = getenv ("HOST");
   if (name != NULL) {
      strncpy (machine, name, (size_t) n);
      j = strlen (machine);
      machine[n - 1] = '\0';
      if (j < n - 3) {
         machine[j++] = ',';
         machine[j++] = ' ';
      }
   }
   name = getenv ("OSTYPE");
   if (name != NULL) {
      if ((int) strlen (name) < n - j)
         strncat (machine, name, (size_t) n - j);
      machine[n - 1] = '\0';
   }

#endif
#endif
}


/*------------------------------------------------------------------------*/
#define MAXBYTES 255

void gdef_WriteHostName (void)
{
   char machine[1 + MAXBYTES] = {'\0'};
   gdef_GetHostName (machine, MAXBYTES);
   printf ("%s\n", machine);
}

#if 0
int main()
{
    struct utsname name;

    if (uname(&name) != -1)
        printf("   %s\n", name.nodename);
    else {
        fprintf(stderr, "Can't find system name\n");
        return 1;
    }
    gdef_GetHostName (0, MAXBYTES);
    gdef_WriteHostName ();
    return 0;
}
#endif
