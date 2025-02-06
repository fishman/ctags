/*
*   Copyright (c) 1998-2003, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   Private definitions for parsing support.
*/
#ifndef CTAGS_MAIN_PARSE_H
#define CTAGS_MAIN_PARSE_H

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */
#include "types.h"

#include "kind.h"
#include "lregex.h"
#include "lxpath.h"
#include "vstring.h"

/*
*   MACROS
*/
#define LANG_AUTO   (-1)
#define LANG_IGNORE (-2)

/*
*   DATA DECLARATIONS
*/
typedef enum {
	RESCAN_NONE,   /* No rescan needed */
	RESCAN_FAILED, /* Scan failed, clear out tags added, rescan */
	RESCAN_APPEND  /* Scan succeeded, rescan */
} rescanReason;

typedef void (*createRegexTag) (const vString* const name);
typedef void (*simpleParser) (void);
typedef rescanReason (*rescanParser) (const unsigned int passCount);
typedef void (*parserInitialize) (langType language);
typedef void (*initStatistics) (langType language);
typedef void (*printStatistics) (langType langType);

/* Per language finalizer is called anytime when ctags exits.
   (Exceptions are a kind of options are given when invoked. Here
   options are: --version, --help, --list-*, and so on.)

   The finalizer is called even when the initializer of the
   same parser is called or not. However, the finalizer can know
   whether the associated initializer is invoked or not with the
   second parameter: INITIALIZED. If it is true, the initializer
   is called. */
typedef void (*parserFinalize) (langType language, bool initialized);

typedef enum {
	METHOD_NOT_CRAFTED    = 1 << 0,
	METHOD_REGEX          = 1 << 1,
	METHOD_XPATH          = 1 << 2,
} parsingMethod;

typedef struct {
	const char *name;
	const int id;
} keywordTable;

typedef enum {
	CORK_NO_USE,
	CORK_QUEUE  = (1 << 0),
	CORK_SYMTAB = (1 << 1),
} corkUsage;

/* optlib2c requires the declaration here. */
enum scriptHook {
	SCRIPT_HOOK_PRELUDE,
	SCRIPT_HOOK_SEQUEL,
	SCRIPT_HOOK_MAKE_TAG_ENTRY_REFLECTION,
	SCRIPT_HOOK_MAX,
};

struct sParserDefinition {
	/* defined by parser */
	char* name;                    /* name of language */

	/* The concept of CURRENT and AGE is taken from libtool.
	 * However, we deleted REVISION in libtool when importing
	 * the concept of versioning from libtool.
	 *
	 * If kinds, roles, fields, and/or extras have been added,
	 * removed or changed since last release, increment CURRENT.
	 * If they have been added since last release, increment AGE.
	 * If they have been removed since last release, set AGE to 0
	 *
	 * From the command line of ctags, you can see the version
	 * information with --version=<LANG>.
	 *
	 * In the tags file, !_TAGS_PARSER_VERSION!<LANG> shows the
	 * information for <LANG>.
	 */
	unsigned int    versionCurrent;
	unsigned int    versionAge;

	kindDefinition* kindTable;     /* tag kinds handled by parser */
	unsigned int kindCount;        /* size of `kinds' list */
	const char *const *extensions; /* list of default extensions */
	const char *const *patterns;   /* list of default file name patterns */
	const char *const *aliases;    /* list of default aliases (alternative names) */
	parserInitialize initialize;   /* initialization routine, if needed */
	parserFinalize finalize;       /* finalize routine, if needed */
	simpleParser parser;           /* simple parser (common case) */
	rescanParser parser2;          /* rescanning parser (unusual case) */
	selectLanguage* selectLanguage; /* may be used to resolve conflicts */
	unsigned int method;           /* see METHOD_ definitions above */
	unsigned int useCork;          /* bit fields of corkUsage */
	bool useMemoryStreamInput;
	bool allowNullTag;             /* allow the parser emit tags with empty
									  strings. If you want to emit a few
									  specified tags with empty strings,
									  you don't need this parser-global
									  allowNullTag; set tagEntryInfo::allowNullTag
									  instead. */
	bool requestAutomaticFQTag;
	tagRegexTable *tagRegexTable;
	unsigned int tagRegexCount;
	const keywordTable *keywordTable;
	unsigned int keywordCount;
	tagXpathTableTable *tagXpathTableTable;
	unsigned int tagXpathTableCount;
	bool invisible;
	fieldDefinition *fieldTable;
	unsigned int fieldCount;
	xtagDefinition *xtagTable;
	unsigned int xtagCount;

	parserDependency * dependencies;
	unsigned int dependencyCount;

	paramDefinition  *paramTable;
	unsigned int paramCount;

	xpathFileSpec *xpathFileSpecs;
	unsigned int xpathFileSpecCount;

	/* Following two fields are used in a parser using cork. */
	const char *defaultScopeSeparator;
	const char *defaultRootScopeSeparator;

	initStatistics initStats;
	printStatistics printStats;

	/* used internally */
	langType id;		    /* id assigned to language */
	unsigned int enabled:1;	       /* currently enabled? */
	unsigned int traced:1;
};

typedef parserDefinition* (parserDefinitionFunc) (void);

/*
*   FUNCTION PROTOTYPES
*/

/* Language processing and parsing */
extern int makeSimpleTag (const vString* const name, const int kindIndex);
extern int makeSimpleRefTag (const vString* const name, const int kindIndex,
			     int roleIndex);
extern int makeSimplePlaceholder(const vString* const name);
extern parserDefinition* parserNew (const char* name);

extern const char *getLanguageName (const langType language);
extern const char *getLanguageKindName (const langType language, const int kindIndex);

extern langType getNamedLanguage (const char *const name, size_t len);
extern langType getNamedLanguageOrAlias (const char *const name, size_t len);
extern langType getLanguageForFilenameAndContents (const char *const fileName);
extern langType getLanguageForCommand (const char *const command, langType startFrom);
extern langType getLanguageForFilename (const char *const filename, langType startFrom);
extern bool isLanguageEnabled (const langType language);
extern bool isLanguageKindEnabled (const langType language, int kindIndex);
extern bool isLanguageRoleEnabled (const langType language, int kindIndex, int roleIndex);

extern kindDefinition* getLanguageKindForName (const langType language, const char *kindName);
extern roleDefinition* getLanguageRoleForName (const langType language, int kindIndex,
											   const char *roleName);

extern void initializeParser (langType language);
extern unsigned int getLanguageCorkUsage (langType language);

#ifdef HAVE_ICONV
extern const char *getLanguageEncoding (const langType language);
#endif


/* Regex interface */
extern void addLanguageCallbackRegex (const langType language, const char *const regex, const char *const flags,
									  const regexCallback callback, bool *disabled, void *userData);
extern void findRegexTagsMainloop (int (* driver)(void));
extern void findRegexTags (void);

/* Multiline Regex Interface */
extern void addLanguageRegexTable (const langType language, const char *name);
extern void addLanguageTagMultiTableRegex(const langType language,
										  const char* const table_name,
										  const char* const regex,
										  const char* const name, const char* const kinds, const char* const flags,
										  bool *disabled);

extern void addLanguageOptscriptToHook (langType language, enum scriptHook hook, const char *const src);

extern void anonGenerateFull (vString *buffer, const char *prefix, langType lang, int kind);
#define anonGenerate(B,P,K) anonGenerateFull((B), (P), LANG_AUTO, (K))
extern void anonConcatFull   (vString *buffer, langType lang, int kind);
#define anonConcat(B,K) anonConcatFull((B), LANG_AUTO, (K))
extern vString *anonGenerateNewFull (const char *prefix, langType lang, int kind);
#define anonGenerateNew(P,K) anonGenerateNewFull((P), LANG_AUTO, (K))
extern void anonHashString (const char *filename, char buf[9]);

#endif  /* CTAGS_MAIN_PARSE_H */
