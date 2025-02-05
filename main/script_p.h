/*
*   Copyright (c) 2020, Masatake YAMATO
*   Copyright (c) 2020, Red Hat, Inc.
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains ctags specific optscript objects
*/

#ifndef CTAGS_MAIN_SCRIPT_PRIVATE_H
#define CTAGS_MAIN_SCRIPT_PRIVATE_H

#include "general.h"  /* must always come first */

#include "field.h"
#include "optscript.h"
#include "mio.h"

struct optscriptOperatorRegistration {
	const char *name;
	OptOperatorFn fn;
	int arity;
	const char *help_str;
};
extern void optscriptRegisterOperators(EsObject * dict,
									   struct optscriptOperatorRegistration regs[], size_t count);

extern EsObject *OPTSCRIPT_ERR_NOTAGENTRY;
extern EsObject *OPTSCRIPT_ERR_UNKNOWNLANGUAGE;
extern EsObject *OPTSCRIPT_ERR_UNKNOWNEXTRA;
extern EsObject *OPTSCRIPT_ERR_FIELDRESET;

extern OptVM *optscriptInit (void);

extern void optscriptInstallProcs (EsObject *dict, OptOperatorFn matchResultAccessor);
extern void optscriptInstallFieldAccessor (EsObject *dict, fieldType ftype);

extern void optscriptSetup (OptVM *vm, EsObject *dict, int corkIndex);
extern void optscriptTeardown (OptVM *vm, EsObject *dict);

/* If len is 0, strlen (src) is used instead of 0. */
extern EsObject *optscriptRead (OptVM *vm, const char *src, size_t len);
extern EsObject *optscriptEval (OptVM *vm, EsObject *code);
extern EsObject *optscriptDefine (EsObject *dict, const char *name, EsObject *obj);

extern EsObject *optscriptReadAndEval   (OptVM *vm, const char *src, size_t len);
extern EsObject *optscriptReadAndDefine (OptVM *vm, EsObject *dict, const char *name,
										 const char *src, size_t len);

extern EsObject *optscriptLoad          (OptVM *vm, MIO *mio);

extern void      optscriptHelp          (OptVM *vm, FILE *fp, EsObject *procdocs);

extern xtagType optscriptGetXtagType (const EsObject *extra);

typedef struct {
	unsigned long base;			/* useless when the parser type is
								 * REG_PARSER_SINGLE_LINE.
								 * base + delta is the file offset. */
	unsigned long delta;		/* for _advanceto operator, relateive from
								 * the base. */
	unsigned long line;
	MIOPos pos;
} matchLoc;
extern int OPT_TYPE_MATCHLOC;

extern int OPT_TYPE_TAG;
#endif	/* CTAGS_MAIN_SCRIPT_PRIVATE_H */
