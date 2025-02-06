/*
*   Copyright (c) 1998-2003, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   Defines internal interface to option processing.
*/
#ifndef CTAGS_MAIN_OPTIONS_PRIVATE_H
#define CTAGS_MAIN_OPTIONS_PRIVATE_H

#if defined(OPTION_WRITE)
# define CONST_OPTION
#else
# define CONST_OPTION const
#endif

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include "args_p.h"
#include "field.h"
#include "fmt_p.h"
#include "options.h"
#include "parse.h"
#include "strlist.h"
#include "vstring.h"

/*
*   MACROS
*/
#define includeExtensionFlags()         (Option.tagFileFormat > 1)

/*
*   DATA DECLARATIONS
*/
typedef enum { OPTION_NONE, OPTION_SHORT, OPTION_LONG } optionType;

typedef struct sCookedArgs {
	/* private */
	Arguments* args;
	char *shortOptions;
	char simple[2];
	bool isOption;
	bool longOption;
	const char* parameter;
	/* public */
	char* item;
} cookedArgs;

typedef enum eLocate {
	EX_MIX,      /* line numbers for defines, patterns otherwise */
	EX_LINENUM,  /* -n  only line numbers in tag file */
	EX_PATTERN,  /* -N  only patterns in tag file */
	EX_COMBINE,  /* Combine linenum and pattern with `;'*/
} exCmd;

typedef enum sortType {
	SO_UNSORTED,
	SO_SORTED,
	SO_FOLDSORTED
} sortType;

typedef enum eTagRelative {
	TREL_NO,
	TREL_YES,
	TREL_ALWAYS,
	TREL_NEVER,
} tagRelative;

/*  This stores the command line options.
 */
typedef struct sOptionValues {
	bool append;         /* -a  append to "tags" file */
	bool backward;       /* -B  regexp patterns search backwards */
	bool etags;          /* -e  output Emacs style tags file */
	exCmd locate;           /* --excmd  EX command used to locate tag */
	bool recurse;        /* -R  recurse into directories */
	sortType sorted;        /* -u,--sort  sort tags */
	bool xref;           /* -x  generate xref output instead */
	fmtElement *customXfmt;	/* compiled code for --xformat=XFMT */
	char *fileList;         /* -L  name of file containing names of files */
	char *tagFileName;      /* -o  name of tags file */
	stringList* headerExt;  /* -h  header extensions */
	stringList* etagsInclude;/* --etags-include  list of TAGS files to include*/
	unsigned int tagFileFormat;/* --format  tag file format (level) */
#ifdef HAVE_ICONV
	char *inputEncoding;	/* --input-encoding	convert text into --output-encoding */
	char *outputEncoding;	/* --output-encoding	write tags file as this encoding */
#endif
	langType language;      /* --lang specified language override */
	bool followLinks;    /* --link  follow symbolic links? */
	bool filter;         /* --filter  behave as filter: files in, tags out */
	char* filterTerminator; /* --filter-terminator  string to output */
	tagRelative tagRelative;    /* --tag-relative file paths relative to tag file */
	int  printTotals;    /* --totals  print cumulative statistics */
	bool lineDirectives; /* --line-directives  process #line directives */
	bool printLanguage;  /* --print-language */
	bool guessLanguageEagerly; /* --guess-language-eagerly|-G */
	bool quiet;		      /* --quiet */
	bool fatalWarnings;	/* --_fatal-warnings */
	unsigned int patternLengthLimit; /* --pattern-length-limit=N */
	bool putFieldPrefix;		 /* --put-field-prefix */
	unsigned int maxRecursionDepth; /* --maxdepth=<max-recursion-depth> */
	bool fieldsReset;				/* --fields=[^+-] */
	enum interactiveMode { INTERACTIVE_NONE = 0,
						   INTERACTIVE_DEFAULT,
						   INTERACTIVE_SANDBOX, } interactive; /* --interactive */
#ifdef _WIN32
	enum filenameSepOp { FILENAME_SEP_NO_REPLACE = false,
						 FILENAME_SEP_USE_SLASH  = true,
						 FILENAME_SEP_UNSET,
	} useSlashAsFilenameSeparator; /* --use-slash-as-filename-separator */
#endif
#ifdef DEBUG
	unsigned long breakLine;/* -b  input line at which to call lineBreak() */
#endif

} optionValues;

typedef void (* mainLoopFunc) (cookedArgs *args, void *data);

/*
*   GLOBAL VARIABLES
*/

extern CONST_OPTION optionValues		Option;

/*
*   FUNCTION PROTOTYPES
*/
extern void freeList (stringList** const pString);
extern void setDefaultTagFileName (void);
extern void checkOptions (void);
extern bool filesRequired (void);
extern void testEtagsInvocation (void);

extern cookedArgs* cArgNewFromString (const char* string);
extern cookedArgs* cArgNewFromArgv (char* const* const argv);
extern cookedArgs* cArgNewFromFile (FILE* const fp);
extern cookedArgs* cArgNewFromLineFile (FILE* const fp);
extern void cArgDelete (cookedArgs* const current);
extern bool cArgOff (cookedArgs* const current);
extern bool cArgIsOption (cookedArgs* const current);
extern const char* cArgItem (cookedArgs* const current);
extern void cArgForth (cookedArgs* const current);
extern bool isExcludedFile (const char* const name,
							bool falseIfExceptionsAreDefeind);
extern bool isIncludeFile (const char *const fileName);
extern void parseCmdlineOptions (cookedArgs* const cargs);
extern void previewFirstOption (cookedArgs* const cargs);
extern void readOptionConfiguration (void);
extern void initOptions (void);
extern void freeOptionResources (void);

extern langType getLanguageComponentInOption (const char *const option,
					      const char *const prefix);
extern langType getLanguageComponentInOptionFull (const char *const option,
					      const char *const prefix, bool noPretending);

extern void processLanguageDefineOption (const char *const option, const char *const parameter);
extern bool processMapOption (const char *const option, const char *const parameter);
extern bool processParamdefOption (const char *const option, const char *const value);
extern bool processParamOption (const char *const option, const char *const value);
extern bool processKinddefOption (const char *const option, const char *const parameter);
extern bool processKindsOption (const char *const option, const char *const parameter);
extern bool processExtradefOption (const char *const option, const char *const parameter);
extern bool processFielddefOption (const char *const option, const char *const parameter);
extern bool processAliasOption (const char *const option, const char *const parameter);
extern bool processTabledefOption (const char *const option, const char *const parameter);
#ifdef HAVE_ICONV
extern bool processLanguageEncodingOption (const char *const option, const char *const parameter);
#endif
extern bool processRoledefOption (const char *const option, const char *const parameter);
extern bool processScopesepOption (const char *const option, const char *const parameter);
extern bool processPreludeOption (const char *const option, const char *const parameter);
extern bool processSequelOption (const char *const option, const char *const parameter);
extern bool processMakeTagEntryReflectionOption (const char *const option, const char *const parameter);
extern bool processPretendOption (const char *const option, const char *const parameter);
extern bool processRolesOption (const char *const option, const char *const parameter);

extern bool isDestinationStdout (void);

extern void setMainLoop (mainLoopFunc func, void *data);

extern bool ptagMakePatternLengthLimit (ptagDesc *pdesc, langType langType, const void *data);
#endif  /* CTAGS_MAIN_OPTIONS_PRIVATE_H */
