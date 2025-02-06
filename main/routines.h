/*
*   Copyright (c) 2002, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   External interface to routines.c
*/
#ifndef CTAGS_MAIN_ROUTINES_H
#define CTAGS_MAIN_ROUTINES_H

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include <stdio.h>


/*
*   MACROS
*/
#define xMalloc(n,Type)    (Type *)eMalloc((size_t)(n) * sizeof (Type))
#define xCalloc(n,Type)    (Type *)eCalloc((size_t)(n), sizeof (Type))
#define xRealloc(p,n,Type) (Type *)eRealloc((p), (n) * sizeof (Type))

#define ARRAY_SIZE(X)      (sizeof (X) / sizeof (X[0]))
#define ARRAY_AND_SIZE(X)  (X), ARRAY_SIZE(X)

#define STRINGIFY(X) STRINGIFY_(X)
#define STRINGIFY_(X) #X

/*
*   DATA DECLARATIONS
*/
typedef int errorSelection;
enum eErrorTypes { FATAL = 1, WARNING = 2, NOTICE = 4, PERROR = 8, };

/*
*   FUNCTION PROTOTYPES
*/
extern void error (const errorSelection selection, const char *const format, ...) CTAGS_ATTR_PRINTF (2, 3);
#define notice(...) error (NOTICE, __VA_ARGS__)

/* Memory allocation functions */
#ifdef NEED_PROTO_MALLOC
extern void *malloc (size_t);
extern void *realloc (void *ptr, size_t);
#endif
extern void *eMalloc (const size_t size);
extern void *eCalloc (const size_t count, const size_t size);
extern void *eRealloc (void *const ptr, const size_t size);
extern void eFree (void *const ptr);
extern void eFreeNoNullCheck (void *const ptr);
extern void eFreeIndirect(void **ptr);

/* String manipulation functions */
extern int struppercmp (const char *s1, const char *s2);
extern int strnuppercmp (const char *s1, const char *s2, size_t n);
extern char* strrstr (const char *str, const char *substr);
extern char* strrpbrk (const char *str, const char *accept);
extern char* eStrdup (const char* str);
extern char* eStrndup (const char* str, size_t len);
extern void toLowerString (char* str);
extern void toUpperString (char* str);
extern char* newLowerString (const char* str);
extern char* newUpperString (const char* str);
extern bool strToUInt(const char *const str, int base, unsigned int *value);
extern bool strToULong(const char *const string, int base, unsigned long *value);
extern bool strToInt(const char *const str, int base, int *value);
extern bool strToLong(const char *const string, int base, long *value);

/* File system functions */
extern const char *baseFilename (const char *const filePath);
extern const char *fileExtension (const char *const fileName);

extern FILE *tempFileFP (const char *const mode, char **const pName);

#endif  /* CTAGS_MAIN_ROUTINES_H */
