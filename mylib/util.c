/*************************************************************************\
 *
 * Package:        MyLib
 * File:           util.c
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

#define _FILE_OFFSET_BITS 64

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>


#define MAXCAR 256                      /* Max length of a line of data */



/************************************************************************/

FILE *util_Fopen (const char *path, const char *mode)
{
   FILE *f;
   errno = 0;
   f = fopen (path, mode);
   if (f == NULL) {
      fprintf (stdout, "\nOpening of %s failed: %s\n\n",
               path, strerror (errno));
      exit (EXIT_FAILURE);
      return NULL;     /* to eliminate a warning from the compiler */
   } else
      return f;
}

int util_Fclose (FILE * f)
{
   int s;
   if (f == NULL)
      return 0;
   errno = 0;
   s = fclose (f);
   if (s != 0)
      fprintf (stdout, "\nClosing of file failed: %s\n\n", strerror (errno));
   return s;
}


/************************************************************************/

void *util_Malloc (size_t size)
{
   void *p;
   errno = 0;
   p = malloc (size);
   if (p == NULL) {
      fprintf (stdout, "\nmalloc failed: %s\n\n", strerror (errno));
      exit (EXIT_FAILURE);
      return NULL;     /* to eliminate a warning from the compiler */
   } else
      return p;
}

void *util_Calloc (size_t count, size_t esize)
{
   void *p;
   errno = 0;
   p = calloc (count, esize);
   if (p == NULL) {
      fprintf (stdout, "\ncalloc failed: %s\n\n", strerror (errno));
      exit (EXIT_FAILURE);
      return NULL;     /* to eliminate a warning from the compiler */
   } else
      return p;
}

void *util_Realloc (void *ptr, size_t size)
{
   void *p;
   errno = 0;
   p = realloc (ptr, size);
   if ((p == NULL) && (size != 0)) {
      fprintf (stdout, "\nrealloc failed: %s\n\n", strerror (errno));
      exit (EXIT_FAILURE);
      return ptr;      /* to eliminate a warning from the compiler */
   } else
      return p;

}

void *util_Free (void *p)
{
   if (p == NULL)
      return NULL;
   free (p);
   return NULL;
}


/************************************************************************/

void util_WriteBool (lebool b, int d)
{
   if (b)
      printf ("%*s", d, "TRUE");
   else
      printf ("%*s", d, "FALSE");
}


void util_ReadBool (char S[], lebool *x)
{
   int j;
   char B[6];
   j = sscanf (S, " %6s", B);
   util_Assert (j > 0, "util_ReadBool:   on reading lebool");
   if (!strncmp (B, "TRUE", (size_t) 5))
      *x = TRUE;
   else if (!strncmp (B, "FALSE", (size_t) 6))
      *x = FALSE;
   else {
      util_Error ("util_ReadBool:   lebool values must be TRUE or FALSE");
   }
}


/************************************************************************/

int util_GetLine (FILE *infile, char *Line, char c)
{
   size_t j;

   while (NULL != fgets (Line, MAXCAR, infile)) { /* Not EOF and no error */
     /* Find first non-white character in Line */
     j = strspn (Line, " \t\r\f\v");
     /* Discard blank lines and lines whose first non-white character is c */
     if (Line[j] == '\n' ||  Line[j] == c) 
        continue;
     else {
        char *p;
        /* If the character c appears, delete the rest of the line*/
        if ((p = strchr (Line, c)))
	   *p = '\0';

        else {
        /* Remove the \n char at the end of line */
           j = strlen (Line);
           if (Line[j - 1] == '\n')
	      Line[j - 1] = '\0';
	}
        return 0;
     }
   }

   util_Fclose (infile);
   return -1;
   /*  util_Error ("GetLine: an error has occurred on reading"); */
}
