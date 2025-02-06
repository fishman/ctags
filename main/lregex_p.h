/*
*   Copyright (c) 2000-2003, Darren Hiebert
*   Copyright (c) 2017, Red Hat, Inc.
*   Copyright (c) 2017, Masatake YAMATO
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for applying regular expression matching.
*
*   The code for utilizing the Gnu regex package with regards to processing the
*   regex option and checking for regex matches was adapted from routines in
*   Gnu etags.
*/
#ifndef CTAGS_MAIN_LREGEX_PRIVATE_H
#define CTAGS_MAIN_LREGEX_PRIVATE_H

/*
*   INCLUDE FILES
*/
#include "general.h"
#include "flags_p.h"
#include "kind_p.h"
#include "lregex.h"
#include "parse.h"

#include <regex.h>

/*
*   MACRO DEFINITIONS
*/
/* Back-references \0 through \9 */
#define BACK_REFERENCE_COUNT 10

/*
*   DATA DECLARATIONS
*/
enum regexParserType {
	REG_PARSER_SINGLE_LINE,
	REG_PARSER_MULTI_LINE,
	REG_PARSER_MULTI_TABLE,
};

struct lregexControlBlock;

typedef struct sRegexCompiledCode {
	struct regexBackend *backend;
	void * code;
} regexCompiledCode;

struct regexBackend {
	flagDefinition *fdefs;
	unsigned int fdef_count;

	void              (* set_icase_flag) (int *);
	regexCompiledCode (* compile)     (struct regexBackend *,
									   const char* const,
									   int);
	int               (* match)       (struct regexBackend *,
									   void *, const char *, size_t,
									   regmatch_t[BACK_REFERENCE_COUNT]);
	void              (* delete_code) (void *);
};

struct flagDefsDescriptor {
	struct regexBackend *backend;
	int flags;
	enum regexParserType regptype;
};

/*
*   FUNCTION PROTOTYPES
*/
extern struct lregexControlBlock* allocLregexControlBlock (parserDefinition *parser);
extern void freeLregexControlBlock (struct lregexControlBlock* lcb);

extern void processTagRegexOption (struct lregexControlBlock *lcb,
								   enum regexParserType,
								   const char* const parameter);
extern void addTagRegex (struct lregexControlBlock *lcb, const char* const regex,
						 const char* const name, const char* const kinds, const char* const flags,
						 bool *disabled);
extern void addTagMultiLineRegex (struct lregexControlBlock *lcb, const char* const regex,
								  const char* const name, const char* const kinds, const char* const flags,
								  bool *disabled);
extern void addTagMultiTableRegex(struct lregexControlBlock *lcb,
								  const char* const table_name,
								  const char* const regex,
								  const char* const name, const char* const kinds, const char* const flags,
								  bool *disabled);

extern bool lregexControlBlockHasAny(struct lregexControlBlock *lcb);

extern bool matchRegex (struct lregexControlBlock *lcb, const vString* const line, bool postrun);
extern bool regexIsPostRun (struct lregexControlBlock *lcb);

extern bool doesExpectCorkInRegex (struct lregexControlBlock *lcb);
extern void addCallbackRegex (struct lregexControlBlock *lcb,
							  const char* const regex,
							  const char* const flags,
							  const regexCallback callback,
							  bool *disabled,
							  void * userData);
extern bool regexNeedsMultilineBuffer (struct lregexControlBlock *lcb);
extern bool matchMultilineRegex (struct lregexControlBlock *lcb, const vString* const allLines);
extern bool matchMultitableRegex (struct lregexControlBlock *lcb, const vString* const allLines);

extern void notifyRegexInputStart (struct lregexControlBlock *lcb);
extern void notifyRegexInputEnd (struct lregexControlBlock *lcb);
extern void notifyRegexMakeTagEntry (struct lregexControlBlock *lcb, int corkIndex);

extern void addRegexTable (struct lregexControlBlock *lcb, const char *name);
extern void extendRegexTable (struct lregexControlBlock *lcb, const char *src, const char *dist);

extern void initRegexOptscript (void);
extern void listRegexOpscriptOperators (FILE *fp);

extern void addOptscriptToHook (struct lregexControlBlock *lcb, enum scriptHook hook, const char *code);
extern void propagateParamToOptscript (struct lregexControlBlock *lcb, const char *param, const char *value);

extern void printMultitableStatistics (struct lregexControlBlock *lcb);

extern void basic_regex_flag_short (char c, void* data);
extern void basic_regex_flag_long (const char* const s, const char* const unused, void* data);
extern void extend_regex_flag_short (char c, void* data);
extern void extend_regex_flag_long (const char* const s, const char* const unused, void* data);
#ifdef HAVE_PCRE2
extern void pcre2_regex_flag_short (char c, void* data);
extern void pcre2_regex_flag_long (const char* const s, const char* const unused, void* data);
#endif

#endif	/* CTAGS_MAIN_LREGEX_PRIVATEH */
