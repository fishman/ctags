/*
*   Copyright (c) 1996-2003, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for managing input languages and
*   dispatching files to the appropriate language parser.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

/* TODO: This definition should be removed. */
#define OPTION_WRITE
#include "options_p.h"

#include <string.h>

#include "ctags.h"
#include "debug.h"
#include "entry_p.h"
#include "field_p.h"
#include "flags_p.h"
#include "htable.h"
#include "keyword.h"
#include "lxpath_p.h"
#include "param.h"
#include "param_p.h"
#include "parse_p.h"
#include "parsers_p.h"
#include "promise.h"
#include "promise_p.h"
#include "ptag_p.h"
#include "ptrarray.h"
#include "read.h"
#include "read_p.h"
#include "routines.h"
#include "routines_p.h"
#include "stats_p.h"
#include "subparser.h"
#include "subparser_p.h"
#include "trace.h"
#include "trashbox.h"
#include "trashbox_p.h"
#include "vstring.h"
#ifdef HAVE_ICONV
# include "mbcs_p.h"
#endif
#include "writer_p.h"
#include "xtag_p.h"

/*
 * DATA TYPES
 */
enum specType {
	SPEC_NONE,
	SPEC_NAME,
	SPEC_ALIAS = SPEC_NAME,
	SPEC_EXTENSION,
	SPEC_PATTERN,
};
const char *specTypeName [] = {
	"none", "name", "extension", "pattern"
};

typedef struct {
	langType lang;
	const char* spec;
	enum specType specType;
}  parserCandidate;

typedef struct sParserObject {
	parserDefinition *def;

	kindDefinition* fileKind;

	stringList* currentPatterns;   /* current list of file name patterns */
	stringList* currentExtensions; /* current list of extensions */
	stringList* currentAliases;    /* current list of aliases */

	unsigned int initialized:1;    /* initialize() is called or not */
	unsigned int dontEmit:1;	   /* run but don't emit tags.
									  This parser was disabled but a subparser on
									  this parser makes this parser run (to drive
									  the subparser). */
	unsigned int pseudoTagPrinted:1;   /* pseudo tags about this parser
										  is emitted or not. */
	unsigned int justRunForSchedulingBase:1;
	unsigned int used;			/* Used for printing language specific statistics. */

	unsigned int anonymousIdentiferId; /* managed by anon* functions */

	struct slaveControlBlock *slaveControlBlock;
	struct kindControlBlock  *kindControlBlock;
	struct lregexControlBlock *lregexControlBlock;
	struct paramControlBlock *paramControlBlock;

	langType pretendingAsLanguage; /* OLDLANG in --_pretend-<NEWLANG>=<OLDLANG>
									  is set here if this parser is NEWLANG.
									  LANG_IGNORE is set if no pretending. */
	langType pretendedAsLanguage;  /* NEWLANG in --_pretend-<NEWLANG>=<OLDLANG>
									  is set here if this parser is OLDLANG.
									  LANG_IGNORE is set if no being pretended. */

	parserDefinitionFunc *parserDefFunc; /* Used when --languages=_CATEGORY
											is specified. */
} parserObject;

/*
 * FUNCTION PROTOTYPES
 */

static void lazyInitialize (langType language);
static void addParserPseudoTags (langType language);
static void installKeywordTable (const langType language);
static void installTagRegexTable (const langType language);
static void installTagXpathTable (const langType language);
static void anonResetMaybe (parserObject *parser);
static void setupAnon (void);
static void teardownAnon (void);
static void uninstallTagXpathTable (const langType language);
static bool hasLanguageAnyRegexPatterns (const langType language);

/*
*   DATA DEFINITIONS
*/
static parserDefinition *FallbackParser (void);
static parserDefinition *CTagsParser (void);
static parserDefinition *CTagsSelfTestParser (void);
static parserDefinitionFunc* BuiltInParsers[] = {
#ifdef EXTERNAL_PARSER_LIST
	EXTERNAL_PARSER_LIST
#else  /* ! EXTERNAL_PARSER_LIST */
	CTagsParser,				/* This must be first entry. */
	FallbackParser,				/* LANG_FALLBACK */
	CTagsSelfTestParser,

	PARSER_LIST,
	XML_PARSER_LIST
#ifdef HAVE_LIBXML
	,
#endif
	YAML_PARSER_LIST
#ifdef HAVE_LIBYAML
	,
#endif
       PEG_PARSER_LIST
#ifdef HAVE_PACKCC
       ,
#endif
	   OPTLIB2C_PCRE2_PARSER_LIST
#ifdef HAVE_PCRE2
      ,
#endif
#endif	/* EXTERNAL_PARSER_LIST */
};
static parserObject* LanguageTable = NULL;
static unsigned int LanguageCount = 0;
static hashTable* LanguageHTable = NULL;
static kindDefinition defaultFileKind = {
	.enabled     = false,
	.letter      = KIND_FILE_DEFAULT_LETTER,
	.name        = KIND_FILE_DEFAULT_NAME,
	.description = KIND_FILE_DEFAULT_NAME,
};

static langType ctagsSelfTestLang;

/*
*   FUNCTION DEFINITIONS
*/

static bool isLanguageNameChar(int c)
{
	if (isgraph(c))
	{
		if (c == '\'' || c == '"' || c == ';')
			return false;
		return true;
	}
	else
		return false;
}

extern unsigned int countParsers (void)
{
	return LanguageCount;
}

extern int makeSimpleTag (
		const vString* const name, const int kindIndex)
{
	return makeSimpleRefTag (name, kindIndex, ROLE_DEFINITION_INDEX);
}

extern int makeSimpleRefTag (const vString* const name, const int kindIndex,
			     int roleIndex)
{
	int r = CORK_NIL;

	Assert (roleIndex < (int)countInputLanguageRoles(kindIndex));

	/* do not check for kind being disabled - that happens later in makeTagEntry() */
	if (name != NULL  &&  vStringLength (name) > 0)
	{
	    tagEntryInfo e;
	    initRefTagEntry (&e, vStringValue (name), kindIndex, roleIndex);

	    r = makeTagEntry (&e);
	}
	return r;
}

extern int makeSimplePlaceholder(const vString* const name)
{
	return makePlaceholder (vStringValue (name));
}

extern bool isLanguageEnabled (const langType language)
{
	const parserDefinition* const def = LanguageTable [language].def;
	return def->enabled;
}

extern bool isLanguageVisible (const langType language)
{
	const parserDefinition* const lang = LanguageTable [language].def;

	return !lang->invisible;
}

/*
*   parserDescription mapping management
*/

extern parserDefinition* parserNew (const char* name)
{
	parserDefinition* result = xCalloc (1, parserDefinition);
	result->name = eStrdup (name);

	result->enabled = true;
	return result;
}

extern bool doesLanguageAllowNullTag (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	return LanguageTable [language].def->allowNullTag;
}

extern bool doesLanguageRequestAutomaticFQTag (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	return LanguageTable [language].def->requestAutomaticFQTag;
}

static const char *getLanguageNameFull (const langType language, bool noPretending)
{
	const char* result;

	if (language == LANG_IGNORE)
		result = "unknown";
	else
	{
		Assert (0 <= language  &&  language < (int) LanguageCount);
		if (noPretending)
			result = LanguageTable [language].def->name;
		else
		{
			langType real_language = LanguageTable [language].pretendingAsLanguage;
			if (real_language == LANG_IGNORE)
				result = LanguageTable [language].def->name;
			else
			{
				Assert (0 <= real_language  &&  real_language < (int) LanguageCount);
				result = LanguageTable [real_language].def->name;
			}
		}
	}
	return result;
}

extern const char *getLanguageName (const langType language)
{
	return getLanguageNameFull (language, false);
}

extern const char *getLanguageKindName (const langType language, const int kindIndex)
{
	kindDefinition* kdef = getLanguageKind (language, kindIndex);
	return kdef->name;
}

static kindDefinition kindGhost = {
	.letter = KIND_GHOST_LETTER,
	.name = KIND_GHOST_NAME,
	.description = KIND_GHOST_NAME,
};

extern int defineLanguageKind (const langType language, kindDefinition *def,
							   freeKindDefFunc freeKindDef)
{
	return defineKind (LanguageTable [language].kindControlBlock, def, freeKindDef);
}

extern unsigned int countLanguageKinds (const langType language)
{
	return countKinds (LanguageTable [language].kindControlBlock);
}

extern unsigned int countLanguageRoles (const langType language, int kindIndex)
{
	return countRoles (LanguageTable [language].kindControlBlock, kindIndex);
}

extern kindDefinition* getLanguageKind (const langType language, int kindIndex)
{
	kindDefinition* kdef;

	Assert (0 <= language  &&  language < (int) LanguageCount);

	switch (kindIndex)
	{
	case KIND_FILE_INDEX:
		kdef = LanguageTable [language].fileKind;
		break;
	case KIND_GHOST_INDEX:
		kdef = &kindGhost;
		break;
	default:
		Assert (kindIndex >= 0);
		kdef = getKind (LanguageTable [language].kindControlBlock, kindIndex);
	}
	return kdef;
}

extern kindDefinition* getLanguageKindForLetter (const langType language, char kindLetter)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	if (kindLetter == LanguageTable [language].fileKind->letter)
		return LanguageTable [language].fileKind;
	else if (kindLetter == KIND_GHOST_LETTER)
		return &kindGhost;
	else
		return getKindForLetter (LanguageTable [language].kindControlBlock, kindLetter);
}

extern kindDefinition* getLanguageKindForName (const langType language, const char *kindName)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (kindName);

	if (strcmp(kindName, LanguageTable [language].fileKind->name) == 0)
		return LanguageTable [language].fileKind;
	else if (strcmp(kindName, KIND_GHOST_NAME) == 0)
		return &kindGhost;
	else
		return getKindForName (LanguageTable [language].kindControlBlock, kindName);
}

extern roleDefinition* getLanguageRole(const langType language, int kindIndex, int roleIndex)
{
	return getRole (LanguageTable [language].kindControlBlock, kindIndex, roleIndex);
}

extern roleDefinition* getLanguageRoleForName (const langType language, int kindIndex,
											   const char *roleName)
{
	return getRoleForName (LanguageTable [language].kindControlBlock, kindIndex, roleName);
}

extern langType getNamedLanguageFull (const char *const name, size_t len, bool noPretending,
									  bool include_aliases)
{
	langType result = LANG_IGNORE;
	unsigned int i;
	Assert (name != NULL);

	if (len == 0)
	{
		parserDefinition *def = (parserDefinition *)hashTableGetItem (LanguageHTable, name);
		if (def)
			result = def->id;
	}
	else
		for (i = 0  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
		{
			const parserDefinition* const lang = LanguageTable [i].def;
			Assert (lang->name);
			vString* vstr = vStringNewInit (name);
			vStringTruncate (vstr, len);

			if (strcasecmp (vStringValue (vstr), lang->name) == 0)
				result = i;
			else if (include_aliases)
			{
				stringList* const aliases = LanguageTable [i].currentAliases;
				if (aliases && stringListCaseMatched (aliases, vStringValue (vstr)))
					result = i;
			}
			vStringDelete (vstr);
		}

	if (result != LANG_IGNORE
		&& (!noPretending)
		&& LanguageTable [result].pretendedAsLanguage != LANG_IGNORE)
		result = LanguageTable [result].pretendedAsLanguage;

	return result;
}

extern langType getNamedLanguage (const char *const name, size_t len)
{
	return getNamedLanguageFull (name, len, false, false);
}

extern langType getNamedLanguageOrAlias (const char *const name, size_t len)
{
	return getNamedLanguageFull (name, len, false, true);
}

static langType getNameOrAliasesLanguageAndSpec (const char *const key, langType start_index,
						 const char **const spec, enum specType *specType)
{
	langType result = LANG_IGNORE;
	unsigned int i;


	if (start_index == LANG_AUTO)
	        start_index = 0;
	else if (start_index == LANG_IGNORE || start_index >= (int) LanguageCount)
		return result;

	for (i = start_index  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		if (! isLanguageEnabled (i))
			continue;

		const parserObject* const parser = LanguageTable + i;
		stringList* const aliases = parser->currentAliases;
		vString* tmp;

		if (parser->def->name != NULL && strcasecmp (key, parser->def->name) == 0)
		{
			result = i;
			*spec = parser->def->name;
			*specType = SPEC_NAME;
		}
		else if (aliases != NULL  &&  (tmp = stringListFileFinds (aliases, key)))
		{
			result = i;
			*spec = vStringValue(tmp);
			*specType = SPEC_ALIAS;
		}
	}
	return result;
}

extern langType getLanguageForCommand (const char *const command, langType startFrom)
{
	const char *const tmp_command = baseFilename (command);
	char *tmp_spec;
	enum specType tmp_specType;

	return getNameOrAliasesLanguageAndSpec (tmp_command, startFrom,
											(const char **const)&tmp_spec,
											&tmp_specType);
}

static langType getPatternLanguageAndSpec (const char *const baseName, langType start_index,
					   const char **const spec, enum specType *specType)
{
	langType result = LANG_IGNORE;
	unsigned int i;

	if (start_index == LANG_AUTO)
	        start_index = 0;
	else if (start_index == LANG_IGNORE || start_index >= (int) LanguageCount)
		return result;

	*spec = NULL;
	for (i = start_index  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		if (! isLanguageEnabled (i))
			continue;

		parserObject *parser = LanguageTable + i;
		stringList* const ptrns = parser->currentPatterns;
		vString* tmp;

		if (ptrns != NULL && (tmp = stringListFileFinds (ptrns, baseName)))
		{
			result = i;
			*spec = vStringValue(tmp);
			*specType = SPEC_PATTERN;
			goto found;
		}
	}

	for (i = start_index  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		if (! isLanguageEnabled (i))
			continue;

		parserObject *parser = LanguageTable + i;
		stringList* const exts = parser->currentExtensions;
		vString* tmp;

		if (exts != NULL && (tmp = stringListExtensionFinds (exts,
								     fileExtension (baseName))))
		{
			result = i;
			*spec = vStringValue(tmp);
			*specType = SPEC_EXTENSION;
			goto found;
		}
	}
found:
	return result;
}

extern langType getLanguageForFilename (const char *const filename, langType startFrom)
{
	const char *const tmp_filename = baseFilename (filename);
	char *tmp_spec;
	enum specType tmp_specType;

	return getPatternLanguageAndSpec (tmp_filename, startFrom,
									  (const char **const)&tmp_spec,
									  &tmp_specType);
}

const char *scopeSeparatorFor (langType language, int kindIndex, int parentKindIndex)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);

	parserObject *parser = LanguageTable + language;
	struct kindControlBlock *kcb = parser->kindControlBlock;

	const scopeSeparator *sep = getScopeSeparator (kcb, kindIndex, parentKindIndex);
	return sep? sep->separator: NULL;
}

static bool processLangDefineScopesep(const langType language,
								  const char *const option,
								  const char *const parameter)
{
	parserObject *parser;
	const char * p = parameter;


	char parentKletter;
	int parentKindex = KIND_FILE_INDEX;
	char kletter;
	int kindex = KIND_FILE_INDEX;
	const char *separator;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;


	/*
	 * Parent
	 */
	parentKletter = p[0];

	if (parentKletter == '\0')
		error (FATAL, "no scope separator specified in \"--%s\" option", option);
	else if (parentKletter == '/')
		parentKindex = KIND_GHOST_INDEX;
	else if (parentKletter == KIND_WILDCARD_LETTER)
		parentKindex = KIND_WILDCARD_INDEX;
	else if (parentKletter == KIND_FILE_DEFAULT_LETTER)
		error (FATAL,
			   "the kind letter `%c' in \"--%s\" option is reserved for \"%s\" kind and no separator can be assigned to",
			   KIND_FILE_DEFAULT_LETTER, option, KIND_FILE_DEFAULT_NAME);
	else if (isalpha ((unsigned char) parentKletter))
	{
		kindDefinition *kdef = getKindForLetter (parser->kindControlBlock, parentKletter);
		if (kdef == NULL)
			error (FATAL,
				   "the kind for letter `%c' specified in \"--%s\" option is not defined.",
				   parentKletter, option);
		parentKindex = kdef->id;
	}
	else
		error (FATAL,
			   "the kind letter `%c` given in \"--%s\" option is not an alphabet",
			   parentKletter, option);


	/*
	 * Child
	 */
	if (parentKindex == KIND_GHOST_INDEX)
		kletter = p[1];
	else
	{
		if (p[1] != '/')
			error (FATAL,
				   "wrong separator specification in \"--%s\" option: no slash after parent kind letter `%c'",
				   option, parentKletter);
		kletter = p[2];
	}

	if (kletter == '\0')
		error (FATAL, "no child kind letter in \"--%s\" option", option);
	else if (kletter == '/')
		error (FATAL,
			   "wrong separator specification in \"--%s\" option: don't specify slash char twice: %s",
			   option, parameter);
	else if (kletter == ':')
		error (FATAL,
			   "no child kind letter in \"--%s\" option", option);
	else if (kletter == KIND_WILDCARD_LETTER)
	{
		if (parentKindex != KIND_WILDCARD_INDEX
			&& parentKindex != KIND_GHOST_INDEX)
			error (FATAL,
				   "cannot use wild card for child kind unless parent kind is also wild card or empty");
		kindex = KIND_WILDCARD_INDEX;
	}
	else if (kletter == KIND_FILE_DEFAULT_LETTER)
		error (FATAL,
			   "the kind letter `%c' in \"--%s\" option is reserved for \"%s\" kind and no separator can be assigned to",
			   KIND_FILE_DEFAULT_LETTER, option, KIND_FILE_DEFAULT_NAME);
	else if (isalpha ((unsigned char) kletter))
	{
		kindDefinition *kdef = getKindForLetter (parser->kindControlBlock, kletter);
		if (kdef == NULL)
			error (FATAL,
				   "the kind for letter `%c' specified in \"--%s\" option is not defined.",
				   kletter, option);
		kindex = kdef->id;
	}
	else
		error (FATAL,
			   "the kind letter `%c` given in \"--%s\" option is not an alphabet",
			   kletter, option);

	/*
	 * Separator
	 */
	if (parentKindex == KIND_GHOST_INDEX)
	{
		if (p[2] != ':')
			error (FATAL,
				   "wrong separator specification in \"--%s\" option: cannot find a colon after child kind: %s",
				   option, parameter);
		separator = p + 3;
	}
	else
	{
		if (p[3] != ':')
			error (FATAL,
				   "wrong separator specification in \"--%s\" option: cannot find a colon after child kind: %s",
				   option, parameter);
		separator = p + 4;
	}

	Assert (parentKindex != KIND_FILE_INDEX);
	Assert (kindex != KIND_FILE_INDEX);
	defineScopeSeparator (parser->kindControlBlock, kindex, parentKindex, separator);
	return true;
}

extern bool processScopesepOption (const char *const option, const char * const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_scopesep-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineScopesep (language, option, parameter);
}

static parserCandidate* parserCandidateNew(unsigned int count CTAGS_ATTR_UNUSED)
{
	parserCandidate* candidates;
	unsigned int i;

	candidates= xMalloc(LanguageCount, parserCandidate);
	for (i = 0; i < LanguageCount; i++)
	{
		candidates[i].lang = LANG_IGNORE;
		candidates[i].spec = NULL;
		candidates[i].specType = SPEC_NONE;
	}
	return candidates;
}

/* If multiple parsers are found, return LANG_AUTO */
static unsigned int nominateLanguageCandidates (const char *const key, parserCandidate** candidates)
{
	unsigned int count;
	langType i;
	const char* spec = NULL;
	enum specType specType = SPEC_NONE;

	*candidates = parserCandidateNew(LanguageCount);

	for (count = 0, i = LANG_AUTO; i != LANG_IGNORE; )
	{
		i = getNameOrAliasesLanguageAndSpec (key, i, &spec, &specType);
		if (i != LANG_IGNORE)
		{
			(*candidates)[count].lang = i++;
			(*candidates)[count].spec = spec;
			(*candidates)[count++].specType = specType;
		}
	}

	return count;
}

static unsigned int
nominateLanguageCandidatesForPattern(const char *const baseName, parserCandidate** candidates)
{
	unsigned int count;
	langType i;
	const char* spec;
	enum specType specType = SPEC_NONE;

	*candidates = parserCandidateNew(LanguageCount);

	for (count = 0, i = LANG_AUTO; i != LANG_IGNORE; )
	{
		i = getPatternLanguageAndSpec (baseName, i, &spec, &specType);
		if (i != LANG_IGNORE)
		{
			(*candidates)[count].lang = i++;
			(*candidates)[count].spec = spec;
			(*candidates)[count++].specType = specType;
		}
	}
	return count;
}

static vString* extractEmacsModeAtFirstLine(MIO* input);

/*  The name of the language interpreter, either directly or as the argument
 *  to "env".
 */
static vString* determineInterpreter (const char* const cmd)
{
	vString* const interpreter = vStringNew ();
	const char* p = cmd;
	do
	{
		vStringClear (interpreter);
		for ( ;  isspace ((unsigned char) *p)  ;  ++p)
			;  /* no-op */
		for ( ;  *p != '\0'  &&  ! isspace ((unsigned char) *p)  ;  ++p)
			vStringPut (interpreter, *p);
	} while (strcmp (vStringValue (interpreter), "env") == 0);
	return interpreter;
}

static vString* extractInterpreter (MIO* input)
{
	vString* const vLine = vStringNew ();
	const char* const line = readLineRaw (vLine, input);
	vString* interpreter = NULL;

	if (line != NULL  &&  line [0] == '#'  &&  line [1] == '!')
	{
		/* "48.2.4.1 Specifying File Variables" of Emacs info:
		   ---------------------------------------------------
		   In shell scripts, the first line is used to
		   identify the script interpreter, so you
		   cannot put any local variables there.  To
		   accommodate this, Emacs looks for local
		   variable specifications in the _second_
		   line if the first line specifies an
		   interpreter.  */

		interpreter = extractEmacsModeAtFirstLine(input);
		if (!interpreter)
		{
			const char* const lastSlash = strrchr (line, '/');
			const char *const cmd = lastSlash != NULL ? lastSlash+1 : line+2;
			interpreter = determineInterpreter (cmd);
		}
	}
	vStringDelete (vLine);
	return interpreter;
}

static bool isShellZsh (const char *p)
{
	p = strstr (p, "sh-set-shell");
	if (!p)
		return false;
	p += strlen("sh-set-shell");

	if (*p == ':')
		p++;
	while (isspace ((unsigned char) *p))
		p++;

	if (strncmp (p, "\"zsh\"", 5) == 0
		|| strncmp (p, "zsh", 3) == 0)
		return true;
	return false;
}

static vString* determineEmacsModeAtFirstLine (const char* const line)
{
	vString* mode = vStringNew ();

	const char* p = strstr(line, "-*-");
	if (p == NULL)
		goto out;
	p += strlen("-*-");

	for ( ;  isspace ((unsigned char) *p)  ;  ++p)
		;  /* no-op */

	if (strncasecmp(p, "mode:", strlen("mode:")) == 0)
	{
		/* -*- mode: MODE; -*- */
		p += strlen("mode:");
		for ( ;  isspace ((unsigned char) *p)  ;  ++p)
			;  /* no-op */
		for ( ;  *p != '\0'  &&  isLanguageNameChar ((unsigned char) *p)  ;  ++p)
			vStringPut (mode, *p);

		if ((strcmp(vStringValue (mode), "sh") == 0
			 || strcmp(vStringValue (mode), "shell-script") == 0)
			&& isShellZsh (p))
			vStringCopyS (mode, "Zsh");
	}
	else
	{
		/* -*- MODE -*- */
		const char* end = strstr (p, "-*-");

		if (end == NULL)
			goto out;

		for ( ;  p < end &&  isLanguageNameChar ((unsigned char) *p)  ;  ++p)
			vStringPut (mode, *p);

		for ( ;  isspace ((unsigned char) *p)  ;  ++p)
			;  /* no-op */
		if (strncmp(p, "-*-", strlen("-*-")) != 0)
			vStringClear (mode);
	}

	vStringLower (mode);

out:
	return mode;

}

static vString* extractEmacsModeAtFirstLine(MIO* input)
{
	vString* const vLine = vStringNew ();
	const char* const line = readLineRaw (vLine, input);
	vString* mode = NULL;
	if (line != NULL)
		mode = determineEmacsModeAtFirstLine (line);
	vStringDelete (vLine);

	if (mode && (vStringLength(mode) == 0))
	{
		vStringDelete(mode);
		mode = NULL;
	}
	return mode;
}

static vString* determineEmacsModeAtEOF (MIO* const fp)
{
	vString* const vLine = vStringNew ();
	const char* line;
	bool headerFound = false;
	const char* p;
	vString* mode = vStringNew ();
	bool is_shell_mode = false;

	while ((line = readLineRaw (vLine, fp)) != NULL)
	{
		if (headerFound && ((p = strstr (line, "mode:")) != NULL))
		{
			vStringClear (mode);
			headerFound = false;

			p += strlen ("mode:");
			for ( ;  isspace ((unsigned char) *p)  ;  ++p)
				;  /* no-op */
			for ( ;  *p != '\0'  &&  isLanguageNameChar ((unsigned char) *p)  ;  ++p)
				vStringPut (mode, *p);

			is_shell_mode = ((strcasecmp (vStringValue (mode), "sh") == 0
								 || strcasecmp (vStringValue (mode), "shell-script") == 0));
		}
		else if (headerFound && (p = strstr(line, "End:")))
			headerFound = false;
		else if (strstr (line, "Local Variables:"))
			headerFound = true;
		else if (is_shell_mode && (p = strstr (line, "sh-set-shell")))
		{
			p += strlen("sh-set-shell");
			while (isspace ((unsigned char) *p))
				p++;
			if (strncmp (p, "\"zsh\"", 5) == 0)
				vStringCopyS (mode, "Zsh");
		}
	}
	vStringDelete (vLine);
	return mode;
}

static vString* extractEmacsModeLanguageAtEOF (MIO* input)
{
	vString* mode;

	/* "48.2.4.1 Specifying File Variables" of Emacs info:
	   ---------------------------------------------------
	   you can define file local variables using a "local
	   variables list" near the end of the file.  The start of the
	   local variables list should be no more than 3000 characters
	   from the end of the file, */
	mio_seek(input, -3000, SEEK_END);

	mode = determineEmacsModeAtEOF (input);
	if (mode && (vStringLength (mode) == 0))
	{
		vStringDelete (mode);
		mode = NULL;
	}

	return mode;
}

static vString* determineVimFileType (const char *const modeline)
{
	/* considerable combinations:
	   --------------------------
	   ... filetype=
	   ... ft= */

	unsigned int i;
	const char* p;

	const char* const filetype_prefix[] = {"filetype=", "ft="};
	vString* const filetype = vStringNew ();

	for (i = 0; i < ARRAY_SIZE(filetype_prefix); i++)
	{
		if ((p = strrstr(modeline, filetype_prefix[i])) == NULL)
			continue;

		p += strlen(filetype_prefix[i]);
		for ( ;  *p != '\0'  &&  isalnum ((unsigned char) *p)  ;  ++p)
			vStringPut (filetype, *p);
		break;
	}
	return filetype;
}

static vString* extractVimFileTypeCommon(MIO* input, bool eof)
{
	/* http://vimdoc.sourceforge.net/htmldoc/options.html#modeline

	   [text]{white}{vi:|vim:|ex:}[white]se[t] {options}:[text]
	   options=> filetype=TYPE or ft=TYPE

	   'modelines' 'mls'	number	(default 5)
			global
			{not in Vi}
	    If 'modeline' is on 'modelines' gives the number of lines that is
	    checked for set commands. */

	vString* filetype = NULL;
#define RING_SIZE 5
	vString* ring[RING_SIZE];
	int i, j;
	unsigned int k;
	const char* const prefix[] = {
		"vim:", "vi:", "ex:"
	};

	for (i = 0; i < RING_SIZE; i++)
		ring[i] = vStringNew ();

	i = 0;
	while ((readLineRaw (ring[i++], input)) != NULL)
		if (i == RING_SIZE)
		{
			i = 0;
			if (!eof)
			{
				i++;
				break;
			}
		}

	j = i;
	do
	{
		const char* p;

		j--;
		if (j < 0)
			j = RING_SIZE - 1;

		for (k = 0; k < ARRAY_SIZE(prefix); k++)
			if ((p = strstr (vStringValue (ring[j]), prefix[k])) != NULL)
			{
				p += strlen(prefix[k]);
				for ( ;  isspace ((unsigned char) *p)  ;  ++p)
					;  /* no-op */
				filetype = determineVimFileType(p);
				break;
			}
	} while (((i == RING_SIZE)? (j != RING_SIZE - 1): (j != i)) && (!filetype));

	for (i = RING_SIZE - 1; i >= 0; i--)
		vStringDelete (ring[i]);
#undef RING_SIZE

	if (filetype && (vStringLength (filetype) == 0))
	{
		vStringDelete (filetype);
		filetype = NULL;
	}
	return filetype;

	/* TODO:
	   [text]{white}{vi:|vim:|ex:}[white]{options} */
}

static vString* extractVimFileTypeAtBOF(MIO* input)
{
	return extractVimFileTypeCommon (input, false);
}

static vString* extractVimFileTypeAtEOF(MIO* input)
{
	return extractVimFileTypeCommon (input, true);
}

static vString* extractMarkGeneric (MIO* input,
									vString * (* determiner)(const char *const, void *),
									void *data)
{
	vString* const vLine = vStringNew ();
	const char* const line = readLineRaw (vLine, input);
	vString* mode = NULL;

	if (line)
		mode = determiner (line, data);

	vStringDelete (vLine);
	return mode;
}

static vString* determineZshAutoloadTag (const char *const modeline,
										 void *data CTAGS_ATTR_UNUSED)
{
	/* See "Autoloaded files" in zsh info.
	   -------------------------------------
	   #compdef ...
	   #autoload [ OPTIONS ] */

	if (((strncmp (modeline, "#compdef", 8) == 0)
	     && isspace ((unsigned char) *(modeline + 8)))
	    || ((strncmp (modeline, "#autoload", 9) == 0)
	        && (isspace ((unsigned char) *(modeline + 9))
	            || *(modeline + 9) == '\0')))
		return vStringNewInit ("zsh");
	else
		return NULL;
}

static vString* extractZshAutoloadTag(MIO* input)
{
	return extractMarkGeneric (input, determineZshAutoloadTag, NULL);
}

static vString* determinePHPMark(const char *const modeline,
		void *data CTAGS_ATTR_UNUSED)
{
	if (strncmp (modeline, "<?php", 5) == 0)
		return vStringNewInit ("php");
	else
		return NULL;
}

static vString* extractPHPMark(MIO* input)
{
	return extractMarkGeneric (input, determinePHPMark, NULL);
}


struct getLangCtx {
    const char *fileName;
    MIO        *input;
    bool     err;
};

#define GLC_FOPEN_IF_NECESSARY0(_glc_, _label_) do {        \
    if (!(_glc_)->input) {                                  \
	    (_glc_)->input = getMio((_glc_)->fileName, "rb", false);	\
        if (!(_glc_)->input) {                              \
            (_glc_)->err = true;                            \
            goto _label_;                                   \
        }                                                   \
    }                                                       \
} while (0)                                                 \

#define GLC_FOPEN_IF_NECESSARY(_glc_, _label_, _doesParserRequireMemoryStream_) \
	do {								\
		if (!(_glc_)->input)					\
			GLC_FOPEN_IF_NECESSARY0 (_glc_, _label_);	\
		if ((_doesParserRequireMemoryStream_) &&		\
		    (mio_memory_get_data((_glc_)->input, NULL) == NULL)) \
		{							\
			MIO *tmp_ = (_glc_)->input;			\
			(_glc_)->input = mio_new_mio (tmp_, 0, -1);	\
			mio_unref (tmp_);				\
			if (!(_glc_)->input) {				\
				(_glc_)->err = true;			\
				goto _label_;				\
			}						\
		}							\
	} while (0)

#define GLC_FCLOSE(_glc_) do {                              \
    if ((_glc_)->input) {                                   \
        mio_unref((_glc_)->input);                             \
        (_glc_)->input = NULL;                              \
    }                                                       \
} while (0)

static const struct taster {
	vString* (* taste) (MIO *);
	const char     *msg;
} eager_tasters[] = {
	{
		.taste  = extractInterpreter,
		.msg    = "interpreter",
	},
	{
		.taste  = extractZshAutoloadTag,
		.msg    = "zsh autoload tag",
	},
	{
		.taste  = extractEmacsModeAtFirstLine,
		.msg    = "emacs mode at the first line",
	},
	{
		.taste  = extractEmacsModeLanguageAtEOF,
		.msg    = "emacs mode at the EOF",
	},
	{
		.taste  = extractVimFileTypeAtBOF,
		.msg    = "vim modeline at the BOF",
	},
	{
		.taste  = extractVimFileTypeAtEOF,
		.msg    = "vim modeline at the EOF",
	},
	{
		.taste  = extractPHPMark,
		.msg    = "PHP marker",
	}
};
static langType tasteLanguage (struct getLangCtx *glc, const struct taster *const tasters, int n_tasters,
			      langType *fallback);

/* If all the candidates have the same specialized language selector, return
 * it.  Otherwise, return NULL.
 */
static bool
hasTheSameSelector (langType lang, selectLanguage candidate_selector)
{
	selectLanguage *selector;

	selector = LanguageTable[ lang ].def->selectLanguage;
	if (selector == NULL)
		return false;

	while (*selector)
	{
		if (*selector == candidate_selector)
			return true;
		selector++;
	}
	return false;
}

static selectLanguage
commonSelector (const parserCandidate *candidates, int n_candidates)
{
    Assert (n_candidates > 1);
    selectLanguage *selector;
    int i;

    selector = LanguageTable[ candidates[0].lang ].def->selectLanguage;
    if (selector == NULL)
	    return NULL;

    while (*selector)
    {
	    for (i = 1; i < n_candidates; ++i)
		    if (! hasTheSameSelector (candidates[i].lang, *selector))
			    break;
	    if (i == n_candidates)
		    return *selector;
	    selector++;
    }
    return NULL;
}


/* Calls the selector and returns the integer value of the parser for the
 * language associated with the string returned by the selector.
 */
static int
pickLanguageBySelection (selectLanguage selector, MIO *input,
						 parserCandidate *candidates,
						 unsigned int nCandidates)
{
	const char *lang;
	langType *cs = xMalloc(nCandidates, langType);
	unsigned int i;

	for (i = 0; i < nCandidates; i++)
		cs[i] = candidates[i].lang;
    lang = selector(input, cs, nCandidates);
	eFree (cs);

    if (lang)
    {
        verbose ("		selection: %s\n", lang);
        return getNamedLanguage(lang, 0);
    }
    else
    {
	verbose ("		no selection\n");
        return LANG_IGNORE;
    }
}

static int compareParsersByName (const void *a, const void* b)
{
	const parserDefinition *const *la = a, *const *lb = b;
	return strcasecmp ((*la)->name, (*lb)->name);
}

static int sortParserCandidatesBySpecType (const void *a, const void *b)
{
	const parserCandidate *ap = a, *bp = b;
	if (ap->specType > bp->specType)
		return -1;
	else if (ap->specType == bp->specType)
	{
		/* qsort, the function calling this function,
		   doesn't do "stable sort". To make the result of
		   sorting predictable, compare the names of parsers
		   when their specType is the same. */
		parserDefinition *la = LanguageTable [ap->lang].def;
		parserDefinition *lb = LanguageTable [bp->lang].def;
		return compareParsersByName (&la, &lb);
	}
	else
		return 1;
}

static unsigned int sortAndFilterParserCandidates (parserCandidate  *candidates,
						   unsigned int n_candidates)
{
	enum specType highestSpecType;
	unsigned int i;
	unsigned int r;

	if (n_candidates < 2)
		return n_candidates;

	qsort (candidates, n_candidates, sizeof(*candidates),
	       sortParserCandidatesBySpecType);

	highestSpecType = candidates [0].specType;
	r = 1;
	for (i = 1; i < n_candidates; i++)
	{
		if (candidates[i].specType == highestSpecType)
			r++;
	}
	return r;
}

static void verboseReportCandidate (const char *header,
				    parserCandidate  *candidates,
				    unsigned int n_candidates)
{
	unsigned int i;
	verbose ("		#%s: %u\n", header, n_candidates);
	for (i = 0; i < n_candidates; i++)
		verbose ("			%u: %s (%s: \"%s\")\n",
			 i,
			 LanguageTable[candidates[i].lang].def->name,
			 specTypeName [candidates[i].specType],
			 candidates[i].spec);
}

static bool doesCandidatesRequireMemoryStream(const parserCandidate *candidates,
						 int n_candidates)
{
	int i;

	for (i = 0; i < n_candidates; i++)
		if (doesParserRequireMemoryStream (candidates[i].lang))
			return true;

	return false;
}

static langType getSpecLanguageCommon (const char *const spec, struct getLangCtx *glc,
				       unsigned int nominate (const char *const, parserCandidate**),
				       langType *fallback)
{
	langType language;
	parserCandidate  *candidates;
	unsigned int n_candidates;

	if (fallback)
		*fallback = LANG_IGNORE;

	n_candidates = (*nominate)(spec, &candidates);
	verboseReportCandidate ("candidates",
				candidates, n_candidates);

	n_candidates = sortAndFilterParserCandidates (candidates, n_candidates);
	verboseReportCandidate ("candidates after sorting and filtering",
				candidates, n_candidates);

	if (n_candidates == 1)
	{
		language = candidates[0].lang;
	}
	else if (n_candidates > 1)
	{
		selectLanguage selector = commonSelector(candidates, n_candidates);
		bool memStreamRequired = doesCandidatesRequireMemoryStream (candidates,
									       n_candidates);

		GLC_FOPEN_IF_NECESSARY(glc, fopen_error, memStreamRequired);
		if (selector) {
			verbose ("	selector: %p\n", selector);
			language = pickLanguageBySelection(selector, glc->input, candidates, n_candidates);
		} else {
			verbose ("	selector: NONE\n");
		fopen_error:
			language = LANG_IGNORE;
		}

		Assert(language != LANG_AUTO);

		if (fallback)
			*fallback = candidates[0].lang;
	}
	else
	{
		language = LANG_IGNORE;
	}

	eFree(candidates);
	candidates = NULL;

	return language;
}

static langType getSpecLanguage (const char *const spec,
                                 struct getLangCtx *glc,
				 langType *fallback)
{
	return getSpecLanguageCommon(spec, glc, nominateLanguageCandidates,
				     fallback);
}

static langType getPatternLanguage (const char *const baseName,
                                    struct getLangCtx *glc,
				    langType *fallback)
{
	return getSpecLanguageCommon(baseName, glc,
				     nominateLanguageCandidatesForPattern,
				     fallback);
}

/* This function tries to figure out language contained in a file by
 * running a series of tests, trying to find some clues in the file.
 */
static langType
tasteLanguage (struct getLangCtx *glc, const struct taster *const tasters, int n_tasters,
	      langType *fallback)
{
    int i;

    if (fallback)
	    *fallback = LANG_IGNORE;
    for (i = 0; i < n_tasters; ++i) {
        langType language;
        vString* spec;

        mio_rewind(glc->input);
	spec = tasters[i].taste(glc->input);

        if (NULL != spec) {
            verbose ("	%s: %s\n", tasters[i].msg, vStringValue (spec));
            language = getSpecLanguage (vStringValue (spec), glc,
					(fallback && (*fallback == LANG_IGNORE))? fallback: NULL);
            vStringDelete (spec);
            if (language != LANG_IGNORE)
                return language;
        }
    }

    return LANG_IGNORE;
}


struct GetLanguageRequest {
	enum { GLR_OPEN, GLR_DISCARD, GLR_REUSE, } type;
	const char *const fileName;
	MIO *mio;
	time_t mtime;
};

static langType
getFileLanguageForRequestInternal (struct GetLanguageRequest *req)
{
	const char *const fileName = req->fileName;
    langType language;

    /* ctags tries variety ways(HINTS) to choose a proper language
       for given fileName. If multiple candidates are chosen in one of
       the hint, a SELECTOR common between the candidate languages
       is called.

       "selection failure" means a selector common between the
       candidates doesn't exist or the common selector returns NULL.

       "hint failure" means the hint finds no candidate or
       "selection failure" occurs though the hint finds multiple
       candidates.

       If a hint chooses multiple candidates, and selection failure is
       occurred, the hint records one of the candidates as FALLBACK for
       the hint. (The candidates are stored in an array. The first
       element of the array is recorded. However, there is no
       specification about the order of elements in the array.)

       If all hints are failed, FALLBACKs of the hints are examined.
       Which fallbacks should be chosen?  `enum hint' defines the order. */
    enum hint {
	    HINT_INTERP,
	    HINT_OTHER,
	    HINT_FILENAME,
	    HINT_TEMPLATE,
	    N_HINTS,
    };
    langType fallback[N_HINTS];
    int i;
    struct getLangCtx glc = {
        .fileName = fileName,
        .input    = (req->type == GLR_REUSE)? mio_ref (req->mio): NULL,
        .err      = false,
    };
    const char* const baseName = baseFilename (fileName);
    char *templateBaseName = NULL;
    fileStatus *fstatus = NULL;

    for (i = 0; i < N_HINTS; i++)
	fallback [i] = LANG_IGNORE;

    verbose ("Get file language for %s\n", fileName);

    verbose ("	pattern: %s\n", baseName);
    language = getPatternLanguage (baseName, &glc,
				   fallback + HINT_FILENAME);
    if (language != LANG_IGNORE || glc.err)
        goto cleanup;

    {
        const char* const tExt = ".in";
        templateBaseName = baseFilenameSansExtensionNew (fileName, tExt);
        if (templateBaseName)
        {
            verbose ("	pattern + template(%s): %s\n", tExt, templateBaseName);
            GLC_FOPEN_IF_NECESSARY(&glc, cleanup, false);
            mio_rewind(glc.input);
            language = getPatternLanguage(templateBaseName, &glc,
					  fallback + HINT_TEMPLATE);
            if (language != LANG_IGNORE)
                goto cleanup;
        }
    }

	/* If the input is already opened, we don't have to verify the existence. */
    if (glc.input || ((fstatus = eStat (fileName)) && fstatus->exists))
    {
	    if ((fstatus && fstatus->isExecutable) || Option.guessLanguageEagerly)
	    {
		    GLC_FOPEN_IF_NECESSARY (&glc, cleanup, false);
		    language = tasteLanguage(&glc, eager_tasters, 1,
					    fallback + HINT_INTERP);
	    }
	    if (language != LANG_IGNORE)
		    goto cleanup;

	    if (Option.guessLanguageEagerly)
	    {
		    GLC_FOPEN_IF_NECESSARY(&glc, cleanup, false);
		    language = tasteLanguage(&glc,
					     eager_tasters + 1,
					     ARRAY_SIZE(eager_tasters) - 1,
					     fallback + HINT_OTHER);
	    }
    }


  cleanup:
	if (req->type == GLR_OPEN && glc.input)
	{
		req->mio = mio_ref (glc.input);
		if (!fstatus)
			fstatus = eStat (fileName);
		if (fstatus)
			req->mtime = fstatus->mtime;
	}
    GLC_FCLOSE(&glc);
    if (fstatus)
	    eStatFree (fstatus);
    if (templateBaseName)
        eFree (templateBaseName);

    for (i = 0;
	 language == LANG_IGNORE && i < N_HINTS;
	 i++)
    {
        language = fallback [i];
	if (language != LANG_IGNORE)
        verbose ("	fallback[hint = %d]: %s\n", i, getLanguageName (language));
    }

	if (language == LANG_IGNORE
		&& isLanguageEnabled (LANG_FALLBACK))
	{
		language = LANG_FALLBACK;
		verbose ("	last resort: using \"%s\" parser\n",
				 getLanguageName (LANG_FALLBACK));
	}
    return language;
}

static langType getFileLanguageForRequest (struct GetLanguageRequest *req)
{
	langType l = Option.language;

	if (l == LANG_AUTO)
		return getFileLanguageForRequestInternal(req);
	else if (! isLanguageEnabled (l))
	{
		error (FATAL,
		       "%s parser specified with --language-force is disabled",
		       getLanguageName (l));
		/* For suppressing warnings. */
		return LANG_AUTO;
	}
	else
		return Option.language;
}

extern langType getLanguageForFilenameAndContents (const char *const fileName)
{
	struct GetLanguageRequest req = {
		.type = GLR_DISCARD,
		.fileName = fileName,
		.mtime = (time_t)0,
	};

	return getFileLanguageForRequest (&req);
}

typedef void (*languageCallback)  (langType language, void* user_data);
static void foreachLanguage(languageCallback callback, void *user_data)
{
	langType result = LANG_IGNORE;

	unsigned int i;
	for (i = 0  ;  i < LanguageCount  &&  result == LANG_IGNORE  ;  ++i)
	{
		const parserDefinition* const lang = LanguageTable [i].def;
		if (lang->name != NULL)
			callback(i, user_data);
	}
}

static void printLanguageMap (const langType language, FILE *fp)
{
	bool first = true;
	unsigned int i;
	parserObject *parser = LanguageTable + language;
	stringList* map = parser->currentPatterns;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	for (i = 0  ;  map != NULL  &&  i < stringListCount (map)  ;  ++i)
	{
		fprintf (fp, "%s(%s)", (first ? "" : " "),
			 vStringValue (stringListItem (map, i)));
		first = false;
	}
	map = parser->currentExtensions;
	for (i = 0  ;  map != NULL  &&  i < stringListCount (map)  ;  ++i)
	{
		fprintf (fp, "%s.%s", (first ? "" : " "),
			 vStringValue (stringListItem (map, i)));
		first = false;
	}
}

extern void installLanguageMapDefault (const langType language)
{
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (parser->currentPatterns != NULL)
		stringListDelete (parser->currentPatterns);
	if (parser->currentExtensions != NULL)
		stringListDelete (parser->currentExtensions);

	if (parser->def->patterns == NULL)
		parser->currentPatterns = stringListNew ();
	else
	{
		parser->currentPatterns =
			stringListNewFromArgv (parser->def->patterns);
	}
	if (parser->def->extensions == NULL)
		parser->currentExtensions = stringListNew ();
	else
	{
		parser->currentExtensions =
			stringListNewFromArgv (parser->def->extensions);
	}
	BEGIN_VERBOSE(vfp);
	{
	printLanguageMap (language, vfp);
	putc ('\n', vfp);
	}
	END_VERBOSE();
}

extern void installLanguageMapDefaults (void)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
	{
		verbose ("    %s: ", getLanguageName (i));
		installLanguageMapDefault (i);
	}
}

extern void installLanguageAliasesDefault (const langType language)
{
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (parser->currentAliases != NULL)
		stringListDelete (parser->currentAliases);

	if (parser->def->aliases == NULL)
		parser->currentAliases = stringListNew ();
	else
	{
		parser->currentAliases =
			stringListNewFromArgv (parser->def->aliases);
	}
	BEGIN_VERBOSE(vfp);
	if (parser->currentAliases != NULL)
		for (unsigned int i = 0  ;  i < stringListCount (parser->currentAliases)  ;  ++i)
			fprintf (vfp, " %s", vStringValue (
						 stringListItem (parser->currentAliases, i)));
	putc ('\n', vfp);
	END_VERBOSE();
}

extern void installLanguageAliasesDefaults (void)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
	{
		verbose ("    %s: ", getLanguageName (i));
		installLanguageAliasesDefault (i);
	}
}

extern void clearLanguageMap (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	stringListClear ((LanguageTable + language)->currentPatterns);
	stringListClear ((LanguageTable + language)->currentExtensions);
}

extern void clearLanguageAliases (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);

	parserObject* parser = (LanguageTable + language);
	if (parser->currentAliases)
		stringListClear (parser->currentAliases);
}

static bool removeLanguagePatternMap1(const langType language, const char *const pattern)
{
	bool result = false;
	stringList* const ptrn = (LanguageTable + language)->currentPatterns;

	if (ptrn != NULL && stringListDeleteItemExtension (ptrn, pattern))
	{
		verbose (" (removed from %s)", getLanguageName (language));
		result = true;
	}
	return result;
}

extern bool removeLanguagePatternMap (const langType language, const char *const pattern)
{
	bool result = false;

	if (language == LANG_AUTO)
	{
		unsigned int i;
		for (i = 0  ;  i < LanguageCount  &&  ! result ;  ++i)
			result = removeLanguagePatternMap1 (i, pattern) || result;
	}
	else
		result = removeLanguagePatternMap1 (language, pattern);
	return result;
}

extern void addLanguagePatternMap (const langType language, const char* ptrn,
				   bool exclusiveInAllLanguages)
{
	vString* const str = vStringNewInit (ptrn);
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (exclusiveInAllLanguages)
		removeLanguagePatternMap (LANG_AUTO, ptrn);
	stringListAdd (parser->currentPatterns, str);
}

static bool removeLanguageExtensionMap1 (const langType language, const char *const extension)
{
	bool result = false;
	stringList* const exts = (LanguageTable + language)->currentExtensions;

	if (exts != NULL  &&  stringListDeleteItemExtension (exts, extension))
	{
		verbose (" (removed from %s)", getLanguageName (language));
		result = true;
	}
	return result;
}

extern bool removeLanguageExtensionMap (const langType language, const char *const extension)
{
	bool result = false;

	if (language == LANG_AUTO)
	{
		unsigned int i;
		for (i = 0  ;  i < LanguageCount ;  ++i)
			result = removeLanguageExtensionMap1 (i, extension) || result;
	}
	else
		result = removeLanguageExtensionMap1 (language, extension);
	return result;
}

extern void addLanguageExtensionMap (
		const langType language, const char* extension,
		bool exclusiveInAllLanguages)
{
	vString* const str = vStringNewInit (extension);
	Assert (0 <= language  &&  language < (int) LanguageCount);
	if (exclusiveInAllLanguages)
		removeLanguageExtensionMap (LANG_AUTO, extension);
	stringListAdd ((LanguageTable + language)->currentExtensions, str);
}

extern void addLanguageAlias (const langType language, const char* alias)
{
	vString* const str = vStringNewInit (alias);
	parserObject* parser;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	if (parser->currentAliases == NULL)
		parser->currentAliases = stringListNew ();
	stringListAdd (parser->currentAliases, str);
}

extern void enableLanguage (const langType language, const bool state)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	LanguageTable [language].def->enabled = state;
}

#ifdef DO_TRACING
extern void traceLanguage (langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	LanguageTable [language].def->traced = true;
}
extern bool isLanguageTraced (langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	return LanguageTable [language].def->traced;
}
#endif /* DO_TRACING */

extern void enableLanguages (const bool state)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
		enableLanguage (i, state);
}

static void installFieldDefinition (const langType language)
{
	unsigned int i;
	parserDefinition * parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable [language].def;

	if (parser->fieldTable != NULL)
	{
		for (i = 0; i < parser->fieldCount; i++)
			defineField (& parser->fieldTable [i], language);
	}
}

static void installXtagDefinition (const langType language)
{
	unsigned int i;
	parserDefinition * parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable [language].def;

	if (parser->xtagTable != NULL)
	{
		for (i = 0; i < parser->xtagCount; i++)
			defineXtag (& parser->xtagTable [i], language);
	}
}

static void initializeParserOne (langType lang)
{
	parserObject *const parser = LanguageTable + lang;

	if (parser->initialized)
		goto out;

	verbose ("Initialize parser: %s\n", parser->def->name);
	parser->initialized = true;

	installKeywordTable (lang);
	installTagXpathTable (lang);
	installFieldDefinition     (lang);
	installXtagDefinition      (lang);

	/* regex definitions refers xtag definitions.
	   So installing RegexTable must be after installing
	   xtag definitions. */
	installTagRegexTable (lang);

	if (parser->def->initialize != NULL)
		parser->def->initialize (lang);

	initializeDependencies (parser->def, parser->slaveControlBlock);

	Assert (parser->fileKind != NULL);
	Assert (!doesParserUseKind (parser->kindControlBlock, parser->fileKind->letter));

	return;

 out:
	/* lazyInitialize() installs findRegexTags() to parser->parser.
	   findRegexTags() should be installed to a parser if the parser is
	   optlib based(created by --langdef) and has some regex patterns(defined
	   with --regex-<LANG>). findRegexTags() makes regex matching work.

	   If a parser can be initialized during evaluating options,
	   --fields-<LANG>=+{something}, for an example.
	   If such option is evaluated first, evaluating --regex-<LANG>=...
	   option doesn't cause installing findRegexTags. As the result
	   regex matching doesn't work. lazyInitialize was called only
	   once when --fields-<LANG>=+{something} was evaluated. In the
	   timing ctags had not seen --regex-<LANG>=.... Even though
	   ctags saw --regex-<LANG>=.... after initializing, there
	   was no chance to install findRegexTags() to parser->parser.

	   Following code block gives extra chances to call lazyInitialize)
	   which installs findRegexTags() to parser->parser.	 */
	if (parser->def->initialize == lazyInitialize)
		parser->def->initialize (lang);
}

extern void initializeParser (langType lang)
{
	if (lang == LANG_AUTO)
	{
		unsigned int i;
		for (i = 0; i < countParsers(); i++)
			initializeParserOne (i);
	}
	else
		initializeParserOne (lang);
}

static void linkDependenciesAtInitializeParsing (parserDefinition *const parser)
{
	unsigned int i;
	parserDependency *d;
	langType upper;
	parserDefinition *lowerParser;
	parserObject *upperParser;

	for (i = 0; i < parser->dependencyCount; i++)
	{
		d = parser->dependencies + i;

		if (d->type == DEPTYPE_FOREIGNER)
		{
			upper = parser->id;
			langType lower = getNamedLanguage (d->upperParser, 0);
			if (lower == LANG_IGNORE)
				error (FATAL,
					   "Unknown language: \"%s\" as a foreigner for %s",
					   d->upperParser, parser->name);

			lowerParser = LanguageTable [lower].def;
		}
		else
		{
			upper = getNamedLanguage (d->upperParser, 0);
			lowerParser = parser;
		}

		upperParser = LanguageTable + upper;

		verbose ("link dependencies: type = %s, upper = %s, lower = %s\n",
				 dependencyTypeString(d->type),
				 upperParser->def->name, lowerParser->name);
		linkDependencyAtInitializeParsing (d->type, upperParser->def,
										   upperParser->slaveControlBlock,
										   upperParser->kindControlBlock,
										   lowerParser,
										   (LanguageTable + lowerParser->id)->kindControlBlock,
										   d->data);
	}
}

/* Used in both builtin and optlib parsers. */
static void initializeParsingCommon (parserDefinition *def, bool is_builtin,
									 parserDefinitionFunc* parserDefFunc)
{
	parserObject *parser;

	if (is_builtin)
		verbose ("%s%s", LanguageCount > 0 ? ", " : "", def->name);
	else
		verbose ("Add optlib parser: %s\n", def->name);

	def->id = LanguageCount++;
	parser = LanguageTable + def->id;
	parser->def = def;
	parser->parserDefFunc = parserDefFunc;

	hashTablePutItem (LanguageHTable, def->name, def);

	parser->fileKind = &defaultFileKind;

	parser->kindControlBlock  = allocKindControlBlock (def);
	parser->slaveControlBlock = allocSlaveControlBlock (def);
	parser->lregexControlBlock = allocLregexControlBlock (def);
	parser->paramControlBlock = allocParamControlBlock (def);
}

static char *acceptableLangName(char *name)
{
	for (char *c = name; *c != '\0'; c++)
	{
		if (isalnum ((unsigned char)*c))
			continue;
		else if (*c == '+' || *c == '#')
			continue;
		else
			return c;
	}
	return NULL;
}

static enum parserCategory getCategoryForParserFunc(parserDefinitionFunc* func)
{
	/* Putting a NULL not to make a zero-sized array that some compilers don't support. */
	parserDefinitionFunc* libxml_fa [] = { NULL, XML_PARSER_LIST };
	parserDefinitionFunc* libyaml_fa[] = { NULL, YAML_PARSER_LIST };
	parserDefinitionFunc* packcc_fa[] = { NULL, PEG_PARSER_LIST };

#define RETURN_IF_FOUND(c,C)									\
	do {														\
		for (size_t i = 0; i < ARRAY_SIZE(c##_fa); i++)			\
		{														\
			if (c##_fa[i] == func)								\
				return PARSER_CATEGORY_##C;						\
		}														\
	} while (0)

	RETURN_IF_FOUND(libxml, LIBXML);
	RETURN_IF_FOUND(libyaml, LIBYAML);
	RETURN_IF_FOUND(packcc, PACKCC);

#undef RETURN_IF_FOUND

	return PARSER_CATEGORY_NONE;
}

extern void initializeParsing (void)
{
	unsigned int builtInCount;
	unsigned int i;

	builtInCount = ARRAY_SIZE (BuiltInParsers);
	LanguageTable = xMalloc (builtInCount, parserObject);
	memset(LanguageTable, 0, builtInCount * sizeof (parserObject));
	for (i = 0; i < builtInCount; ++i)
	{
		LanguageTable [i].pretendingAsLanguage = LANG_IGNORE;
		LanguageTable [i].pretendedAsLanguage = LANG_IGNORE;
	}

	LanguageHTable = hashTableNew (127,
								   hashCstrcasehash,
								   hashCstrcaseeq,
								   NULL,
								   NULL);
	DEFAULT_TRASH_BOX(LanguageHTable, hashTableDelete);

	verbose ("Installing parsers: ");
	for (i = 0  ;  i < builtInCount  ;  ++i)
	{
		parserDefinition* const def = (*BuiltInParsers [i]) ();
		if (def != NULL)
		{
			Assert (def->name);
			Assert (def->name[0] != '\0');
			Assert (strcmp (def->name, RSV_LANG_ALL));
			Assert (acceptableLangName (def->name) == NULL);

			if (def->method & METHOD_NOT_CRAFTED)
				def->parser = findRegexTags;
			else
				/* parser definition must define one and only one parsing routine */
				Assert ((!!def->parser) + (!!def->parser2) == 1);

			initializeParsingCommon (def, true, BuiltInParsers [i]);
		}
	}
	verbose ("\n");

	for (i = 0; i < builtInCount  ;  ++i)
		linkDependenciesAtInitializeParsing (LanguageTable [i].def);
}

extern void freeParserResources (void)
{
	unsigned int i;
	for (i = 0  ;  i < LanguageCount  ;  ++i)
	{
		parserObject* const parser = LanguageTable + i;

		if (parser->def->finalize)
			(parser->def->finalize)((langType)i, (bool)parser->initialized);

		uninstallTagXpathTable (i);

		freeLregexControlBlock (parser->lregexControlBlock);
		freeKindControlBlock (parser->kindControlBlock);
		parser->kindControlBlock = NULL;

		finalizeDependencies (parser->def, parser->slaveControlBlock);
		freeSlaveControlBlock (parser->slaveControlBlock);
		parser->slaveControlBlock = NULL;

		freeParamControlBlock (parser->paramControlBlock);

		freeList (&parser->currentPatterns);
		freeList (&parser->currentExtensions);
		freeList (&parser->currentAliases);

		eFree (parser->def->name);
		parser->def->name = NULL;
		eFree (parser->def);
		parser->def = NULL;
	}
	if (LanguageTable != NULL)
		eFree (LanguageTable);
	LanguageTable = NULL;
	LanguageCount = 0;
}

static void doNothing (void)
{
}

static void optlibRunBaseParser (void)
{
	scheduleRunningBaseparser (0);
}

static bool optlibIsDedicatedSubparser (parserDefinition* def)
{
	return (def->dependencies
			&& (def->dependencies->type == DEPTYPE_SUBPARSER)
			&& ((subparser *)def->dependencies->data)->direction & SUBPARSER_SUB_RUNS_BASE);
}

static void lazyInitialize (langType language)
{
	parserDefinition* def;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	def = LanguageTable [language].def;

	def->parser = doNothing;

	if (def->method & METHOD_REGEX)
	{
		if (optlibIsDedicatedSubparser (def))
			def->parser = optlibRunBaseParser;
		else
			def->parser = findRegexTags;
	}
}

extern void enableDefaultFileKind (bool state)
{
	defaultFileKind.enabled = state;
}

/*
*   Option parsing
*/
struct preLangDefFlagData
{
	const char *const name;
	char *base;
	subparserRunDirection direction;
	bool autoFQTag;
	intArray *foreignLanguages;
	unsigned int versionCurrent;
	unsigned int versionAge;
};

static void pre_lang_def_flag_base_long (const char* const optflag, const char* const param, void* data)
{
	struct preLangDefFlagData * flag_data = data;
	langType base;

	if (param[0] == '\0')
	{
		error (WARNING, "No base parser specified for \"%s\" flag of --langdef option", optflag);
		return;
	}

	base = getNamedLanguage (param, 0);
	if (base == LANG_IGNORE)
	{
		error (WARNING, "Unknown language(%s) is specified for \"%s\" flag of --langdef option",
			   param, optflag);
		return;

	}

	langType cpreproc = getNamedLanguage ("CPreProcessor", 0);
	if (base == cpreproc)
	{
		/* See Tmain/optscript-preludes-stack.d */
		error (WARNING,
			   "Because of an internal limitation, Making a sub parser based on the CPreProcessor parser is not allowed: %s",
			   param);
		return;
	}

	flag_data->base = eStrdup(param);
}

#define LANGDEF_FLAG_DEDICATED "dedicated"
#define LANGDEF_FLAG_SHARED    "shared"
#define LANGDEF_FLAG_BIDIR     "bidirectional"
static void pre_lang_def_flag_direction_long (const char* const optflag, const char* const param CTAGS_ATTR_UNUSED, void* data)
{
	struct preLangDefFlagData * flag_data = data;

	if (strcmp(optflag, LANGDEF_FLAG_DEDICATED) == 0)
		flag_data->direction = SUBPARSER_SUB_RUNS_BASE;
	else if (strcmp(optflag, LANGDEF_FLAG_SHARED) == 0)
		flag_data->direction = SUBPARSER_BASE_RUNS_SUB;
	else if (strcmp(optflag, LANGDEF_FLAG_BIDIR) == 0)
		flag_data->direction = SUBPARSER_BI_DIRECTION;
	else
		AssertNotReached ();
}

static void pre_lang_def_flag_autoFQTag_long (const char* const optflag,
											  const char* const param CTAGS_ATTR_UNUSED,
											  void* data)
{
	struct preLangDefFlagData * flag_data = data;
	flag_data->autoFQTag = true;
}

static void pre_lang_def_flag_foreignLanguage_long (const char* const optflag,
											const char* const param,
											void* data)
{
	struct preLangDefFlagData * flag_data = data;
	if (!param)
	{
		error (WARNING, "value for '%s' flag is empty", optflag);
		return;
	}

	langType lang = getNamedLanguage (param, 0);
	if (lang == LANG_IGNORE)
		error (FATAL, "language named '%s' is not found or not initialized yet",
			   param);

	verbose ("Foreign language for %s: %s\n", flag_data->name, getLanguageName (lang));
	intArrayAdd (flag_data->foreignLanguages, lang);
}

static void pre_lang_def_flag_version_long (const char* const optflag CTAGS_ATTR_UNUSED,
											const char* const param,
											void* data)
{
	struct preLangDefFlagData * flag_data = data;
	char * verstr = eStrdup (param);
	char * age = strchr(verstr, '.');
	if (!age)
		error (FATAL, "Faile to parse the version number ('.') for language \"%s\": %s",
			   flag_data->name, param);
	*age = '\0';
	age++;

	if (!strToUInt (verstr, 10, &flag_data->versionCurrent))
		error (FATAL, "Faile to parse the version number (the current part) for language \"%s\": %s",
			   flag_data->name, param);

	if (!strToUInt (age, 10, &flag_data->versionAge))
		error (FATAL, "Faile to parse the version number (the age part) for language \"%s\": %s",
			   flag_data->name, param);

	eFree (verstr);
}

static flagDefinition PreLangDefFlagDef [] = {
	{ '\0',  "base", NULL, pre_lang_def_flag_base_long,
	  "BASEPARSER", "utilize as a base parser"},
	{ '\0',  LANGDEF_FLAG_DEDICATED,  NULL,
	  pre_lang_def_flag_direction_long,
	  NULL, "make the base parser dedicated to this subparser"},
	{ '\0',  LANGDEF_FLAG_SHARED,     NULL,
	  pre_lang_def_flag_direction_long,
	  NULL, "share the base parser with the other subparsers"
	},
	{ '\0',  LANGDEF_FLAG_BIDIR,      NULL,
	  pre_lang_def_flag_direction_long,
	  NULL, "utilize the base parser both 'dedicated' and 'shared' way"
	},
	{ '\0',  "_autoFQTag", NULL, pre_lang_def_flag_autoFQTag_long,
	  NULL, "make full qualified tags automatically based on scope information"},
	{ '\0', "_foreignLanguage",    NULL, pre_lang_def_flag_foreignLanguage_long,
	  "LANG", "initialize another parser" },
	{ '\0', "version",     NULL, pre_lang_def_flag_version_long,
	  NULL, "set the version of the parser (current.age)"},
};

static void optlibFreeDep (langType lang, bool initialized CTAGS_ATTR_UNUSED)
{
	parserDefinition * pdef = LanguageTable [lang].def;

	for (size_t i = 0; i < pdef->dependencyCount; i++)
	{
		parserDependency *dep = pdef->dependencies + i;

		eFree ((char *)dep->upperParser); /* Dirty cast */
		dep->upperParser = NULL;

		if (dep->data)
		{
			eFree (dep->data);
			dep->data = NULL;
		}
	}
	if (pdef->dependencies)
	{
		eFree (pdef->dependencies);
		pdef->dependencies = NULL;
	}
}

static parserDefinition* OptlibParser(const char *name, const char *base,
									  subparserRunDirection direction,
									  intArray *foreignLanguages)
{
	parserDefinition *def;
	parserDependency *dep = NULL;

	def = parserNew (name);
	def->initialize        = lazyInitialize;
	def->method            = METHOD_NOT_CRAFTED;

	size_t dep_count = (base? 1: 0) + intArrayCount (foreignLanguages);

	if (dep_count)
	{
		dep = xCalloc (dep_count, parserDependency);
		def->dependencies = dep;
		def->dependencyCount = dep_count;
		def->finalize = optlibFreeDep;
	}

	if (base)
	{
		subparser *sub = xCalloc (1, subparser);

		sub->direction = direction;
		dep[0].type = DEPTYPE_SUBPARSER;
		dep[0].upperParser = eStrdup (base);
		dep[0].data = sub;
	}

	for (size_t i = 0 ; i < intArrayCount (foreignLanguages); i++)
	{
		size_t index = (base? 1: 0) + i;
		langType lang = intArrayItem (foreignLanguages, i);
		Assert (lang != LANG_IGNORE
				&& lang != LANG_AUTO
				&& lang < (int) LanguageCount);

		dep[index].type = DEPTYPE_FOREIGNER;
		dep[index].upperParser = eStrdup (getLanguageName (lang));
		dep[index].data = NULL;
	}

	return def;
}

extern void processLanguageDefineOption (
		const char *const option, const char *const parameter)
{
	char *name;
	char *flags;
	parserDefinition*  def;

	flags = strchr (parameter, LONG_FLAGS_OPEN);
	if (flags)
		name = eStrndup (parameter, flags - parameter);
	else
		name = eStrdup (parameter);

	/* Veirfy that the name of new language is acceptable or not. */
	char *unacceptable;
	if (name [0] == '\0')
	{
		eFree (name);
		error (FATAL, "No language specified for \"%s\" option", option);
	}
	else if (getNamedLanguage (name, 0) != LANG_IGNORE)
	{
		/* name cannot be freed because it is used in the FATAL message. */
		error (FATAL, "Language \"%s\" already defined", name);
	}
	else if (strcmp(name, RSV_LANG_ALL) == 0)
	{
		eFree (name);
		error (FATAL, "\"" RSV_LANG_ALL "\" is reserved; don't use it as the name for defining a new language");
	}
	else if (strcmp(name, RSV_NONE) == 0)
	{
		eFree (name);
		error (FATAL, "\"" RSV_NONE "\" is reserved; don't use it as the name for defining a new language");

	}
	else if ((unacceptable = acceptableLangName(name)))
	{
		char c = *unacceptable;

		/* name cannot be freed because it is used in the FATAL message. */
		/* We accept '_'.
		 * We accept # and + because they are already used in C# parser and C++ parser.
		 * {... is already trimmed at the beginning of this function. */
		if ((c == '`') || (c == '\''))
			error (FATAL, "don't use \"%c\" in a language name (%s)", c, name);
		else
			error (FATAL, "don't use `%c' in a language name (%s)", c, name);
	}

	LanguageTable = xRealloc (LanguageTable, LanguageCount + 1, parserObject);
	memset (LanguageTable + LanguageCount, 0, sizeof(parserObject));

	struct preLangDefFlagData data = {
		.name = name,
		.base = NULL,
		.direction = SUBPARSER_UNKNOWN_DIRECTION,
		.autoFQTag = false,
		.versionCurrent = 0,
		.versionAge = 0,
	};
	data.foreignLanguages = intArrayNew ();

	flagsEval (flags, PreLangDefFlagDef, ARRAY_SIZE (PreLangDefFlagDef), &data);

	if (data.base == NULL && data.direction != SUBPARSER_UNKNOWN_DIRECTION)
		error (WARNING, "Ignore the direction of subparser because \"{base=}\" is not given");

	if (data.base && data.direction == SUBPARSER_UNKNOWN_DIRECTION)
		data.direction = SUBPARSER_BASE_RUNS_SUB;

	def = OptlibParser (name, data.base, data.direction,
						data.foreignLanguages);
	if (data.base)
		eFree (data.base);

	def->requestAutomaticFQTag = data.autoFQTag;
	def->versionCurrent = data.versionCurrent;
	def->versionAge = data.versionAge;

	initializeParsingCommon (def, false, NULL);
	linkDependenciesAtInitializeParsing (def);

	LanguageTable [def->id].currentPatterns = stringListNew ();
	LanguageTable [def->id].currentExtensions = stringListNew ();
	LanguageTable [def->id].pretendingAsLanguage = LANG_IGNORE;
	LanguageTable [def->id].pretendedAsLanguage = LANG_IGNORE;

	intArrayDelete (data.foreignLanguages);
	eFree (name);
}

extern bool doesLanguageHaveForeignDependency (const langType lang,
											   const langType foreignLang)
{
	Assert (lang != LANG_IGNORE
			&& lang != LANG_AUTO
			&& lang < (int) LanguageCount);
	Assert (foreignLang != LANG_IGNORE
			&& foreignLang != LANG_AUTO
			&& foreignLang < (int) LanguageCount);


	parserDefinition * pdef = LanguageTable [lang].def;

	for (size_t i = 0; i < pdef->dependencyCount; i++)
	{
		parserDependency *dep = pdef->dependencies + i;

		if (dep->type == DEPTYPE_FOREIGNER)
		{
			if (getNamedLanguage (dep->upperParser, 0) == foreignLang)
				return true;
		}
	}

	return false;
}

extern bool isLanguageKindEnabled (const langType language, int kindIndex)
{
	kindDefinition * kdef = getLanguageKind (language, kindIndex);
	return kdef->enabled;
}

extern bool isLanguageRoleEnabled (const langType language, int kindIndex, int roleIndex)
{
	return isRoleEnabled(LanguageTable [language].kindControlBlock,
						 kindIndex, roleIndex);
}

extern bool isLanguageKindRefOnly (const langType language, int kindIndex)
{
	kindDefinition * def =  getLanguageKind(language, kindIndex);
	return def->referenceOnly;
}

static void resetLanguageKinds (const langType language, const bool mode)
{
	const parserObject* parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;

	{
		unsigned int i;
		struct kindControlBlock *kcb = parser->kindControlBlock;

		for (i = 0  ;  i < countKinds (kcb)  ;  ++i)
		{
			kindDefinition *kdef = getKind (kcb, i);
			enableKind (kdef, mode);
		}
	}
}

static bool enableLanguageKindForLetter (
		const langType language, const int kind, const bool mode)
{
	bool result = false;
	kindDefinition* const def = getLanguageKindForLetter (language, kind);
	if (def != NULL)
	{
		enableKind (def, mode);
		result = true;
	}
	return result;
}

static bool enableLanguageKindForName (
	const langType language, const char * const name, const bool mode)
{
	bool result = false;
	kindDefinition* const def = getLanguageKindForName (language, name);
	if (def != NULL)
	{
		enableKind (def, mode);
		result = true;
	}
	return result;
}

static void processLangKindDefinition (
		const langType language, const char *const option,
		const char *const parameter)
{
	const char *p = parameter;
	bool mode = true;
	int c;
	static vString *longName;
	bool inLongName = false;
	const char *k;
	bool r;

	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParser (language);
	if (*p == '*')
	{
		resetLanguageKinds (language, true);
		p++;
	}
	else if (*p != '+'  &&  *p != '-')
		resetLanguageKinds (language, false);

	longName = vStringNewOrClearWithAutoRelease (longName);

	while ((c = (unsigned char) *p++) != '\0')
	{
		switch (c)
		{
		case '+':
			if (inLongName)
				vStringPut (longName, c);
			else
				mode = true;
			break;
		case '-':
			if (inLongName)
				vStringPut (longName, c);
			else
				mode = false;
			break;
		case '{':
			if (inLongName)
				error(FATAL,
				      "unexpected character in kind specification: \'%c\'",
				      c);
			inLongName = true;
			break;
		case '}':
			if (!inLongName)
				error(FATAL,
				      "unexpected character in kind specification: \'%c\'",
				      c);
			k = vStringValue (longName);
			r = enableLanguageKindForName (language, k, mode);
			if (! r)
				error (WARNING, "Unsupported kind: '%s' for --%s option",
				       k, option);

			inLongName = false;
			vStringClear (longName);
			break;
		default:
			if (inLongName)
				vStringPut (longName, c);
			else
			{
				r = enableLanguageKindForLetter (language, c, mode);
				if (! r)
					error (WARNING, "Unsupported kind: '%c' for --%s option",
					       c, option);
			}
			break;
		}
	}
}

static void freeKdef (kindDefinition *kdef)
{
	eFree (kdef->name);
	eFree (kdef->description);
	eFree (kdef);
}

static char *extractDescriptionAndFlags(const char *input, const char **flags)
{
	vString *vdesc = vStringNew();
	bool escaped = false;

	if (flags)
		*flags = NULL;

	while (*input != '\0')
	{
		if (escaped)
		{
			vStringPut (vdesc, *input);
			escaped = false;

		}
		else if (*input == '\\')
			escaped = true;
		else if (*input == LONG_FLAGS_OPEN)
		{
			if (flags)
				*flags = input;
			break;
		}
		else
			vStringPut (vdesc, *input);
		input++;
	}
	return vStringDeleteUnwrap(vdesc);
}

static void pre_kind_def_flag_refonly_long (const char* const optflag,
											const char* const param, void* data)
{
	kindDefinition *kdef = data;
	kdef->referenceOnly = true;
}

static flagDefinition PreKindDefFlagDef [] = {
	{ '\0', "_refonly", NULL, pre_kind_def_flag_refonly_long,
	  NULL, "use this kind reference tags only"},
};

static bool processLangDefineKind(const langType language,
								  const char *const option,
								  const char *const parameter)
{
	parserObject *parser;

	kindDefinition *kdef;
	char letter;
	const char * p = parameter;
	char *name;
	char *description;
	const char *name_start;
	const char *marker_end;
	size_t name_len;
	const char *flags;


	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;

	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no kind definition specified in \"--%s\" option", option);

	letter = p[0];
	if (letter == ',')
		error (FATAL, "no kind letter specified in \"--%s\" option", option);
	if (/* See #1697. isalnum expects 0~255 as the range of characters. */
		!isalpha ((unsigned char)letter)
		)
		error (FATAL, "the kind letter given in \"--%s\" option is not an alphabet", option);
	else if (letter == KIND_FILE_DEFAULT_LETTER)
		error (FATAL, "the kind letter `%c' in \"--%s\" option is reserved for \"%s\" kind",
			   KIND_FILE_DEFAULT_LETTER, option, KIND_FILE_DEFAULT_NAME);
	else if (getKindForLetter (parser->kindControlBlock, letter))
	{
		error (WARNING, "the kind for letter `%c' specified in \"--%s\" option is already defined.",
			   letter, option);
		return true;
	}

	if (p[1] != ',')
		error (FATAL, "wrong kind definition in \"--%s\" option: no comma after letter", option);

	p += 2;
	if (p[0] == '\0')
		error (FATAL, "no kind name specified in \"--%s\" option", option);
	marker_end = strchr (p, ',');
	if (!marker_end)
		error (FATAL, "no kind description specified in \"--%s\" option", option);

	name_start = p;
	while (p != marker_end)
	{
		if (p == name_start)
		{
			if (!isalpha((unsigned char) *p))
			{
				char *name_in_msg = eStrndup (name_start, marker_end - name_start);
				error (FATAL,
					   "a kind name doesn't start with an alphabetical character: "
					   "'%s' in \"--%s\" option",
					   name_in_msg, option);
			}
		}
		else
		{
			if (!isalnum ((unsigned char) *p))
			{
				char *name_in_msg = eStrndup (name_start, marker_end - name_start);
				error (FATAL,
					   "non-alphanumeric char is used as part of kind name: "
					   "'%s' in \"--%s\" option",
					   name_in_msg, option);
			}
		}
		p++;
	}

	if (marker_end == name_start)
		error (FATAL, "the kind name in \"--%s\" option is empty", option);

	name_len = marker_end - name_start;
	if (strncmp (name_start, KIND_FILE_DEFAULT_NAME, name_len) == 0)
		error (FATAL,
			   "the kind name " KIND_FILE_DEFAULT_NAME " in \"--%s\" option is reserved",
			   option);

	name = eStrndup (name_start, name_len);
	if (getKindForName (parser->kindControlBlock, name))
	{
		error (WARNING, "the kind for name `%s' specified in \"--%s\" option is already defined.",
			   name, option);
		eFree (name);
		return true;
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "found an empty kind description in \"--%s\" option", option);

	description = extractDescriptionAndFlags (p, &flags);

	kdef = xCalloc (1, kindDefinition);
	kdef->enabled = true;
	kdef->letter = letter;
	kdef->name = name;
	kdef->description = description;
	if (flags)
		flagsEval (flags, PreKindDefFlagDef, ARRAY_SIZE (PreKindDefFlagDef), kdef);

	defineKind (parser->kindControlBlock, kdef, freeKdef);
	return true;
}

static void freeRdef (roleDefinition *rdef)
{
	eFree (rdef->name);
	eFree (rdef->description);
	eFree (rdef);
}

static bool processLangDefineRole(const langType language,
								  const char *const kindSpec,
								  const char *const option,
								  const char *const parameter)
{
	parserObject *parser;

	kindDefinition *kdef;
	roleDefinition *rdef;
	char *name;
	char *description;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (parameter);

	parser = LanguageTable + language;

	if (*kindSpec == '{')
	{
		const char *end = strchr (kindSpec, '}');
		if (end == NULL)
			error (FATAL, "no '}' representing the end of kind name in --%s option: %s",
				   option, kindSpec);
		if (*(end + 1) != '\0')
			error (FATAL, "garbage after the kind specification %s in --%s option",
				   kindSpec, option);
		char *kindName = eStrndup (kindSpec + 1, end - (kindSpec + 1));
		if (strcmp (kindName, KIND_FILE_DEFAULT_NAME) == 0)
			error (FATAL, "don't define a role for %c/%s kind; it has no role: --%s",
				   KIND_FILE_DEFAULT_LETTER, KIND_FILE_DEFAULT_NAME,
				   option);
		kdef = getKindForName (parser->kindControlBlock, kindName);
		if (kdef == NULL)
			error (FATAL, "the kind for name `%s' specified in \"--%s\" option is not defined.",
				   kindName, option);
		eFree (kindName);
	}
	else
	{
		char kletter = *kindSpec;
		if (!isalnum ((unsigned char)kletter))
			error (FATAL, "the kind letter given in \"--%s\" option is not an alphabet or a number", option);
		else if (kletter == KIND_FILE_DEFAULT_LETTER)
			error (FATAL, "the kind letter `%c' in \"--%s\" option is reserved for \"%s\" kind, and no role can be attached to it",
				   KIND_FILE_DEFAULT_LETTER, option, KIND_FILE_DEFAULT_NAME);
		else if (*(kindSpec + 1) != '\0')
			error (FATAL, "more than one letters are specified as a kind spec in \"--%s\" option: use `{' and `}' for specifying a kind name",
				   option);

		kdef = getKindForLetter (parser->kindControlBlock, kletter);
		if (kdef == NULL)
		{
			error (FATAL, "the kind for letter `%c' specified in \"--%s\" option is not defined.",
				   *kindSpec, option);
			return true;
		}
	}

	const char * p = parameter;
	const char *tmp_end = strchr (p, ',');
	if (!tmp_end)
		error (FATAL, "no role description specified in \"--%s\" option", option);

	const char * tmp_start = p;
	while (p != tmp_end)
	{
		if (!isalnum ((unsigned char) *p))
			error (FATAL, "unacceptable char as part of role name in \"--%s\" option: '%c'",
				   option, *p);
		p++;
	}

	if (tmp_end == tmp_start)
		error (FATAL, "the role name in \"--%s\" option is empty", option);

	name = eStrndup (tmp_start, tmp_end - tmp_start);
	if (getRoleForName (parser->kindControlBlock, kdef->id, name))
	{
		error (WARNING, "the role for name `%s' specified in \"--%s\" option is already defined.",
			   name, option);
		eFree (name);
		return true;
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "found an empty role description in \"--%s\" option", option);

	const char *flags;
	description = extractDescriptionAndFlags (p, &flags);

	rdef = xCalloc (1, roleDefinition);
	rdef->enabled = true;
	rdef->name = name;
	rdef->description = description;

	if (flags)
		flagsEval (flags, NULL, 0, rdef);

	defineRole (parser->kindControlBlock, kdef->id, rdef, freeRdef);

	return true;
}

extern bool processKinddefOption (const char *const option, const char * const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "kinddef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineKind (language, option, parameter);
}

extern bool processRoledefOption (const char *const option, const char * const parameter)
{
#define PREFIX "_roledef-"
#define PREFIX_LEN strlen(PREFIX)

	langType language = getLanguageComponentInOption (option, PREFIX);
	if (language == LANG_IGNORE)
		return false;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	const char* kindSpec = option + PREFIX_LEN + strlen (getLanguageName (language));
	if (*kindSpec == '\0')
		error (FATAL, "no kind is specifined in \"--%s=%s\"", option, parameter);
	if (*kindSpec != '.')
		error (FATAL, "no delimiter (.) where a kindspec starts is found in \"--%s\": %c",
			   option, *kindSpec);
	kindSpec++;

	return processLangDefineRole (language, kindSpec, option, parameter);
#undef PREFIX
#undef PREFIX_LEN
}

struct langKindDefinitionStruct {
	const char *const option;
	const char *const parameter;
};
static void processLangKindDefinitionEach(
	langType lang, void* user_data)
{
	struct langKindDefinitionStruct *arg = user_data;
	processLangKindDefinition (lang, arg->option, arg->parameter);
}

static bool parameterEnablingAllOrFileKind (const char *const option,
											const char *const parameter,
											bool following_plus_or_minus_op)
{
	size_t file_long_flag_len = strlen(KIND_FILE_DEFAULT_NAME);

	switch (parameter[0])
	{
	/* Though only '*' is documented as an acceptable kind spec for
	 * --kinds-all option in our man page, we accept '\0' here because
	 * it will be useful for testing purpose. */
	case '\0':
		if (following_plus_or_minus_op)
			error(FATAL, "no kind specification after + (or -) in --%s option",
				  option);
		else
			return true;
	case '+':
	case '-':
		if (following_plus_or_minus_op)
			error(FATAL, "don't repeat + (nor -) in --%s option",
				  option);
		else
			return parameterEnablingAllOrFileKind (option, parameter + 1, true);
	case KIND_WILDCARD_LETTER:
		if (following_plus_or_minus_op)
			error(FATAL, "don't use '*' after + (nor -) in --%s option",
				  option);
		else
			return parameterEnablingAllOrFileKind (option, parameter + 1, false);
	case KIND_FILE_DEFAULT_LETTER:
		return parameterEnablingAllOrFileKind (option, parameter + 1, false);
	case '{':
		if (strncmp (parameter + 1, KIND_FILE_DEFAULT_NAME, file_long_flag_len) == 0
			&& parameter [1 + file_long_flag_len] == '}')
			return parameterEnablingAllOrFileKind (option,
												   parameter + 1 + file_long_flag_len + 1,
												   false);
		break;
	}
	return false;
}

extern bool processKindsOption (
		const char *const option, const char *const parameter)
{
#define PREFIX "kinds-"
#define PREFIX_LEN strlen(PREFIX)

	bool handled = false;
	struct langKindDefinitionStruct arg = {
		.option = option,
		.parameter = parameter,
	};
	langType language;

	const char* const dash = strchr (option, '-');
	if (dash != NULL  &&
		(strcmp (dash + 1, "kinds") == 0  ||  strcmp (dash + 1, "types") == 0))
	{
		size_t len = dash - option;
		char *langName = eStrndup (option, len);

		if ((len == 3) && (strcmp (langName, RSV_LANG_ALL) == 0))
		{
			error (WARNING,
				   "\"--%s\" option is obsolete; use \"--kinds-%s\" instead",
				   option, langName);
			if (!parameterEnablingAllOrFileKind (option, parameter, false))
				error (FATAL, "only '*', 'F', \"{file}\" or their combination is acceptable as kind letter for --%s", option);
			foreachLanguage(processLangKindDefinitionEach, &arg);
		}
		else
		{
			language = getNamedLanguage (langName, 0);
			if (language == LANG_IGNORE)
				error (WARNING, "Unknown language \"%s\" in \"%s\" option", langName, option);
			else
				processLangKindDefinition (language, option, parameter);
		}
		eFree (langName);
		handled = true;
	}
	else if ( strncmp (option, PREFIX, PREFIX_LEN) == 0 )
	{
		const char* lang;

		lang = option + PREFIX_LEN;
		if (lang[0] == '\0')
			error (WARNING, "No language given in \"%s\" option", option);
		else if (strcmp (lang, RSV_LANG_ALL) == 0)
		{
			if (!parameterEnablingAllOrFileKind (option, parameter, false))
				error (FATAL, "only '*', 'F', \"{file}\" or their combination is acceptable as kind letter for --%s", option);
			foreachLanguage(processLangKindDefinitionEach, &arg);
		}
		else
		{
			language = getNamedLanguage (lang, 0);
			if (language == LANG_IGNORE)
				error (WARNING, "Unknown language \"%s\" in \"%s\" option", lang, option);
			else
				processLangKindDefinition (language, option, parameter);
		}
		handled = true;
	}
	return handled;
#undef PREFIX
#undef PREFIX_LEN
}

/*
 * The argument specification for --roles-<LANG>:<KIND>= option
 * =====================================================================
 *
 * --roles-all.*=
 * --roles-all=
 * => Disable all roles of all kinds in all languages.
 *
 * --roles-all.*='*'
 * --roles-all='*'
 * => Enable all roles of all kinds in all languages.
 *
 * --roles-<LANG>.*=
 * --roles-<LANG>=
 * => Disable all roles of all kinds.
 *
 * --roles-<LANG>.*=*
 * --roles-<LANG>=*
 * => Enable all roles of all kinds.
 *
 * --roles-<LANG>.{kind}=
 * --roles-<LANG>.k=
 * => Disable all roles of the kind specified with a letter.
 *
 * --roles-<LANG>.{kind}=*
 * --roles-<LANG>.k=*
 * => Enable all roles of the kind specified with a letter.
 *
 * --roles-<LANG>.{kind}=[+|-]{role}
 * --roles-<LANG>.k=[+|-]{role}
 * => Enable/disable the role of the kind specified with a letter.
 *
 *
 * Examples of combination
 * ---------------------------------------------------------------------
 *
 * --roles-<LANG>.k0=+{role0}-{role1}{role2}
 * --roles-<LANG>.{kind1}=+{role0}-{role1}{role2}
 *
 *
 * How --roledef should be change to align --roles-<LANG> notation
 * ---------------------------------------------------------------------
 *
 * --_roledef-<LANG>.k=role,description
 * --_roledef-<LANG>.{kind}=role,description
 *
 * The notation was
 *  --_roledef-<LANG>=k.role,description
 *
 *
 * How --param should be change to align --roles-<LANG> notation
 * ---------------------------------------------------------------------
 *
 * --param-<LANG>.name=argument
 *
 * The notation was
 * --param-<LANG>:name=argument
 *
 *
 * How --paramdef should be to align --roles-<LANG> notation
 * ---------------------------------------------------------------------
 *
 * --_paramdef-<LANG>.name=[ default (desription) ]
 *
 *
 * Discussion: which shoule we use . or : ?
 * ---------------------------------------------------------------------
 *
 * `.' is better because `:' implies fields.
 *
 */
struct langKindRoleDefinitionStruct {
	int kindIndex;
	const char *const option;
	const char *const parameter;
};

typedef void (*kindCallback)  (langType language, int kindIndex, void* user_data);
static void foreachKind(langType language, kindCallback callback, void *user_data)
{
	unsigned int c = countLanguageKinds (language);
	for (unsigned int i = 0; i < c; i++)
		callback (language, (int)i, user_data);
}

static void resetKindRoles (const langType language, int kindIndex, const bool mode)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	unsigned int c = countLanguageRoles (language, kindIndex);

	for (unsigned int i = 0; i < c; i++)
	{
		roleDefinition* rdef = getLanguageRole (language, kindIndex, (int)i);
		enableRole (rdef, mode);
	}
}

static void resetKindRolesAsCallback (const langType language, int kindIndex, void *user_data)
{
	bool mode = (bool)user_data;
	resetKindRoles (language, kindIndex, mode);
}

static void processLangKindRoleDefinition (
		const langType language, const int kindIndex, const char *const option,
		const char *const parameter)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (kindIndex != KIND_GHOST_INDEX);
	initializeParser (language);

	const char *p = parameter;
	bool mode = true;

	if (*p == '\0')
	{
		resetKindRoles (language, kindIndex, false);
		return;
	}
	else if (*p != '+' && *p != '-' )
		resetKindRoles (language, kindIndex, false);

	while (*p != '\0')
	{
		if (*p == '+')
		{
			mode = true;
			p++;
		}
		else if (*p == '-')
		{
			mode = false;
			p++;
		}
		else if (*p == '{')
		{
			p++;
			char *q = strchr (p, '}');
			if (!q)

				error (FATAL, "no '}' representing the end of role name in --%s option: %s",
					   option, p);
			if (p == q)
				error (FATAL, "empty role for the kind letter: %c",
					   getLanguageKind (language, kindIndex)->letter);

			char *rname = eStrndup (p,  q - p);
			roleDefinition *rdef = getLanguageRoleForName (language, kindIndex, rname);
			if (!rdef)
				error (WARNING, "no such role: \"%s\" in kind \'%c\' in language \"%s\"",
					   rname, getLanguageKind (language, kindIndex)->letter,
					   getLanguageName (language));
			else
				enableRole (rdef, mode);
			eFree (rname);
			p = q + 1;
		}
		else if (*p == '*')
		{
			resetKindRoles (language, kindIndex, true);
			p++;
		}
		else
			error (FATAL, "unexpected character %c in --%s=%s option",
				   *p, option, parameter);
	}
}

static void processLangKindRoleDefinitionEach (langType language, void* user_data)
{
	struct langKindRoleDefinitionStruct *arg = user_data;

	if (arg->kindIndex == KIND_GHOST_INDEX)
	{
		initializeParser (language);
		foreachKind (language, resetKindRolesAsCallback,
					 ((*(arg->parameter) == '*')? (void *)true: (void *)false));
	}
	else
		processLangKindRoleDefinition (language, arg->kindIndex,
									   arg->option, arg->parameter);
}

extern bool processRolesOption (const char *const option, const char *const parameter)
{
#define PREFIX "roles-"
#define PREFIX_LEN strlen(PREFIX)

	if ( strncmp (option, PREFIX, PREFIX_LEN) != 0 )
		return false;

	const char* lang = option + PREFIX_LEN;
	if (lang[0] == '\0')
	{
		error (WARNING, "no language given in \"%s\" option", option);
		return true;
	}

	/*
	 * --roles-all.*=
	 * --roles-all=
	 * => Disable all roles of all kinds in all languages.
	 *
	 * --roles-all.*='*'
	 * --roles-all='*'
	 * => Enable all roles of all kinds in all languages.
	 */
	if (strncmp (lang, RSV_LANG_ALL, strlen(RSV_LANG_ALL)) == 0)
	{
		if (lang [strlen (RSV_LANG_ALL)] == '\0'
			|| (strcmp (lang + strlen (RSV_LANG_ALL), ".*") == 0))
		{
			if (*parameter == '\0'
				|| strcmp(parameter, "*") == 0)
			{
				struct langKindRoleDefinitionStruct arg = {
					.kindIndex = KIND_GHOST_INDEX,
					.option = option,
					.parameter = parameter,
				};
				foreachLanguage (processLangKindRoleDefinitionEach, &arg);
				return true;
			}
			else
				error (FATAL, "only '*' or '' (empty string) is acceptable as an argument for --%s: %s",
					   option,
					   parameter);
		}
		else if (lang [strlen(RSV_LANG_ALL)] == '.')
			error (FATAL, "only '*' or '' (empty string) is acceptable as a kind spec for --%sall: --%s",
				   PREFIX,
				   option);
	}

	/* Decide the language. */
	langType language;
	const char *dot = strchr (lang, '.');
	if (dot)
		language = getNamedLanguage (lang, dot - lang);
	else
		language = getNamedLanguage (lang, 0);

	if (language == LANG_IGNORE)
	{
		char *lang0 = dot? eStrndup (lang, dot - lang): NULL;
		error (WARNING, "unknown language \"%s\" in --%s option",
			   (lang0? lang0: lang), option);
		if (lang0)
			eFree (lang0);
		return true;
	}

	/*
	 * --roles-<LANG>.*=
	 * --roles-<LANG>=
	 * => Disable all roles of all kinds.
	 *
	 * --roles-<LANG>.*=*
	 * --roles-<LANG>=*
	 * => Enable all roles of all kinds.
	 */
	if (dot == NULL || (strcmp (dot, ".*") == 0))
	{
		if (*parameter == '\0'
			|| strcmp(parameter, "*") == 0)
		{
			foreachKind (language, resetKindRolesAsCallback,
						 ((*parameter == '*')? (void*)true: (void*)false));
			return true;
		}
		else
			error (FATAL, "only '*' or '' (empty string) is acceptable as an argument for --%s: %s",
				   option,
				   parameter);
	}

	/* Decide the kind of the language. */
	parserObject *parser = LanguageTable + language;
	int kindIndex = KIND_GHOST_INDEX;
	const char *kind = dot + 1;
	if (*kind == '{')
	{
		const char *name_end = strchr (kind, '}');
		if (name_end == NULL)
			error (FATAL, "no '}' representing the end of kind name in --%s option: %s",
				   option, kind);
		char *kindName = eStrndup (kind + 1, name_end - (kind + 1));
		if (strcmp (kindName, KIND_FILE_DEFAULT_NAME) == 0)
		{
			error (WARNING, "don't enable/disable a role in %c/%s kind; it has no role: --%s",
				   KIND_FILE_DEFAULT_LETTER, KIND_FILE_DEFAULT_NAME,
				   option);
			return true;
		}
		kindIndex = getKindIndexForName (parser->kindControlBlock, kindName);
		if (kindIndex == KIND_GHOST_INDEX)
		{
			eFree (kindName);
			error (WARNING, "no such kind name as specified in --%s option", option);
			return true;
		}
		if (*(name_end + 1) != '\0')
			error (FATAL, "garbage after the kind specification {%s} in --%s option",
				   kindName, option);
		eFree (kindName);
	}
	else if (isalpha ((unsigned char)*kind))
	{
		if (*kind == KIND_FILE_DEFAULT_LETTER)
		{
			error (WARNING, "don't enable/disable a role in %c/%s kind; it has no role: --%s",
				   KIND_FILE_DEFAULT_LETTER, KIND_FILE_DEFAULT_NAME,
				   option);
			return true;
		}
		kindIndex = getKindIndexForLetter (parser->kindControlBlock, *kind);
		if (kindIndex == KIND_GHOST_INDEX)
		{
			error (WARNING, "no such kind letter as specified in --%s option", option);
			return true;
		}
		if (*(kind + 1) != '\0')
			error (FATAL, "garbage after the kind specification '%c' in --%s option",
				   *kind, option);
	}
	else
		error (FATAL, "'%c', unexpected character in --%s", *kind, option);


	/*
	 * --roles-<LANG>.{kind}=
	 * --roles-<LANG>.k=
	 * => Disable all roles of the kind specified with a letter.
	 *
	 * --roles-<LANG>.{kind}=*
	 * --roles-<LANG>.k=*
	 * => Enable all roles of the kind specified with a letter.
	 *
	 * --roles-<LANG>.{kind}=[+|-|]{role}
	 * --roles-<LANG>.k=[+|-|]{role}
	 * => Enable/disable the role of the kind specified with a letter.
	 */
	processLangKindRoleDefinition (language, kindIndex, option, parameter);

	return true;
#undef PREFIX
#undef PREFIX_LEN
}

extern void printLanguageRoles (const langType language, const char* kindspecs,
								bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable *table = roleColprintTableNew();
	parserObject *parser;

	initializeParser (language);

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0  ;  i < LanguageCount  ;  ++i)
		{
			if (!isLanguageVisible (i))
				continue;

			parser = LanguageTable + i;
			roleColprintAddRoles (table, parser->kindControlBlock, kindspecs);
		}
	}
	else
	{
		parser = LanguageTable + language;
		roleColprintAddRoles (table, parser->kindControlBlock, kindspecs);
	}

	roleColprintTablePrint (table, (language != LANG_AUTO),
							withListHeader, machinable, fp);
	colprintTableDelete (table);
}

static void printKinds (langType language, bool indent,
						struct colprintTable * table)
{
	const parserObject *parser;
	struct kindControlBlock *kcb;
	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParser (language);
	parser = LanguageTable + language;
	kcb = parser->kindControlBlock;

	if (table)
		kindColprintAddLanguageLines (table, kcb);
	else
	{
		for (unsigned int i = 0  ;  i < countKinds(kcb)  ;  ++i)
			printKind (getKind(kcb, i), indent);
	}
}

extern void printLanguageKinds (const langType language, bool allKindFields,
								bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable * table = NULL;

	if (allKindFields)
		table = kindColprintTableNew ();

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0  ;  i < LanguageCount  ;  ++i)
		{
			const parserDefinition* const lang = LanguageTable [i].def;

			if (lang->invisible)
				continue;

			if (!table)
				printf ("%s%s\n", lang->name, isLanguageEnabled (i) ? "" : " [disabled]");
			printKinds (i, true, table);
		}
	}
	else
		printKinds (language, false, table);

	if (allKindFields)
	{
		kindColprintTablePrint(table, (language == LANG_AUTO)? 0: 1,
							   withListHeader, machinable, fp);
		colprintTableDelete (table);
	}
}

extern bool processParamOption (
			const char *const option, const char *const value)
{
	langType language;
	const char* name;
	const char* sep;

	language = getLanguageComponentInOption (option, "param-");
	if (language == LANG_IGNORE)
		return false;

	sep = option + strlen ("param-") + strlen (getLanguageName (language));
	/* `:' is only for keeping self compatibility */
	if (! (*sep == '.' || *sep == ':' ))
		error (FATAL, "no separator(.) is given for %s=%s", option, value);
	name = sep + 1;

	if (value == NULL || value [0] == '\0')
		error (FATAL, "no value is given for %s", option);

	if (applyLanguageParam (language, name, value))
		propagateParamToOptscript (LanguageTable [language].lregexControlBlock,
								   name, value);
	return true;
}

static void freePdef (paramDefinition *pdef)
{
	eFree ((void *)pdef->name);
	eFree ((void *)pdef->desc);
	eFree (pdef);
}

static bool processLangDefineParam (const langType language,
									const char *const option,
									const char *const parameter)
{
	parserObject *parser;

	paramDefinition *pdef;
	const char * p = parameter;
	const char *name_end;
	const char *desc;
	const char *flags;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no parameter definition specified in \"--%s\" option", option);

	name_end = strchr (p, ',');
	if (!name_end)
		error (FATAL, "no parameter description specified in \"--%s\" option", option);
	else if (name_end == p)
		error (FATAL, "the parameter name in \"--%s\" option is empty", option);

	for (; p < name_end; p++)
	{
		if (!isalnum ((unsigned char) *p) && *p != '_')
			error (FATAL, "unacceptable char as part of parameter name in \"--%s\" option: '%c'",
				   option, *p);
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "parameter description in \"--%s\" option is empty", option);

	desc = extractDescriptionAndFlags (p, &flags);

	pdef = xCalloc (1, paramDefinition);
	pdef->name = eStrndup (parameter, name_end - parameter);
	pdef->desc = desc;

	if (flags)
		flagsEval (flags, NULL, 0, pdef);

	parser = LanguageTable + language;
	defineParam (parser->paramControlBlock, pdef, freePdef);
	return true;
}

extern bool processParamdefOption (const char *const option, const char *const value)
{
	langType language;

	language = getLanguageComponentInOption (option, "_paramdef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineParam (language, option, value);
}

static void printParams (struct colprintTable *table, langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParser (language);
	paramColprintAddParams (table,
							LanguageTable [language].paramControlBlock);
}

extern void printLanguageParams (const langType language,
								 bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable *table =  paramColprintTableNew();

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0; i < LanguageCount ; ++i)
		{
			const parserDefinition* const lang = LanguageTable [i].def;

			if (lang->invisible)
				continue;

			printParams (table, i);
		}
	}
	else
		printParams (table, language);

	paramColprintTablePrint (table, (language != LANG_AUTO),
							 withListHeader, machinable, fp);
	colprintTableDelete (table);
}

static void processLangAliasOption (const langType language,
				    const char *const parameter)
{
	const char* alias;
	const parserObject * parser;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;

	if (parameter[0] == '\0')
	{
		clearLanguageAliases (language);
		verbose ("clear aliases for %s\n", parser->def->name);
	}
	else if (strcmp (parameter, RSV_LANGMAP_DEFAULT) == 0)
	{
		installLanguageAliasesDefault (language);
		verbose ("reset aliases for %s\n", parser->def->name);
	}
	else if (parameter[0] == '+')
	{
		alias = parameter + 1;
		addLanguageAlias(language, alias);
		verbose ("add an alias %s to %s\n", alias, parser->def->name);
	}
	else if (parameter[0] == '-')
	{
		if (parser->currentAliases)
		{
			alias = parameter + 1;
			if (stringListDeleteItemExtension (parser->currentAliases, alias))
			{
				verbose ("remove an alias %s from %s\n", alias, parser->def->name);
			}
		}
	}
	else
	{
		alias = parameter;
		clearLanguageAliases (language);
		addLanguageAlias(language, alias);
		verbose ("set alias %s to %s\n", alias, parser->def->name);
	}

}

extern bool processAliasOption (
		const char *const option, const char *const parameter)
{
	langType language;

	Assert (parameter);

#define PREFIX "alias-"
	if (strcmp (option, "alias-" RSV_LANG_ALL) == 0)
	{
		if ((parameter[0] == '\0')
			|| (strcmp (parameter, RSV_LANGMAP_DEFAULT) == 0))
		{
			for (unsigned int i = 0; i < LanguageCount; i++)
			{
				clearLanguageAliases (i);
				verbose ("clear aliases for %s\n", getLanguageName(i));
			}

			if (parameter[0] != '\0')
			{
				verbose ("  Installing default language aliases:\n");
				installLanguageAliasesDefaults ();
			}
		}
		else
		{
			error (WARNING, "Use \"%s\" option for reset (\"default\") or clearing (\"\")", option);
			return false;
		}
		return true;
	}

	language = getLanguageComponentInOption (option, "alias-");
	if (language == LANG_IGNORE)
		return false;
#undef PREFIX

	processLangAliasOption (language, parameter);
	return true;
}

static void printMaps (const langType language, langmapType type)
{
	const parserObject* parser;
	unsigned int i;

	parser = LanguageTable + language;
	printf ("%-8s", parser->def->name);
	if (parser->currentPatterns != NULL && (type & LMAP_PATTERN))
		for (i = 0  ;  i < stringListCount (parser->currentPatterns)  ;  ++i)
			printf (" %s", vStringValue (
						stringListItem (parser->currentPatterns, i)));
	if (parser->currentExtensions != NULL && (type & LMAP_EXTENSION))
		for (i = 0  ;  i < stringListCount (parser->currentExtensions)  ;  ++i)
			printf (" *.%s", vStringValue (
						stringListItem (parser->currentExtensions, i)));
	putchar ('\n');
}

static struct colprintTable *mapColprintTableNew (langmapType type)
{
	if ((type & LMAP_ALL) == LMAP_ALL)
		return colprintTableNew ("L:LANGUAGE", "L:TYPE", "L:MAP", NULL);
	else if (type & LMAP_PATTERN)
		return colprintTableNew ("L:LANGUAGE", "L:PATTERN", NULL);
	else if (type & LMAP_EXTENSION)
		return colprintTableNew ("L:LANGUAGE", "L:EXTENSION", NULL);
	else
	{
		AssertNotReached ();
		return NULL;
	}
}

static void mapColprintAddLanguage (struct colprintTable * table,
									langmapType type,
									const parserObject* parser)
{
	struct colprintLine * line;
	unsigned int count;
	unsigned int i;

	if ((type & LMAP_PATTERN) && (0 < (count = stringListCount (parser->currentPatterns))))
	{
		for (i = 0; i < count; i++)
		{
			line = colprintTableGetNewLine (table);
			vString *pattern = stringListItem (parser->currentPatterns, i);

			colprintLineAppendColumnCString (line, parser->def->name);
			if (type & LMAP_EXTENSION)
				colprintLineAppendColumnCString (line, "pattern");
			colprintLineAppendColumnVString (line, pattern);
		}
	}

	if ((type & LMAP_EXTENSION) && (0 < (count = stringListCount (parser->currentExtensions))))
	{
		for (i = 0; i < count; i++)
		{
			line = colprintTableGetNewLine (table);
			vString *extension = stringListItem (parser->currentExtensions, i);

			colprintLineAppendColumnCString (line, parser->def->name);
			if (type & LMAP_PATTERN)
				colprintLineAppendColumnCString (line, "extension");
			colprintLineAppendColumnVString (line, extension);
		}
	}
}

extern void printLanguageMaps (const langType language, langmapType type,
							   bool withListHeader, bool machinable, FILE *fp)
{
	/* DON'T SORT THE LIST

	   The order of listing should be equal to the order of matching
	   for the parser selection. */

	struct colprintTable * table = NULL;
	if (type & LMAP_TABLE_OUTPUT)
		table = mapColprintTableNew(type);

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0  ;  i < LanguageCount  ;  ++i)
		{
			if (!isLanguageVisible (i))
				continue;

			if (type & LMAP_TABLE_OUTPUT)
			{
				const parserObject* parser = LanguageTable + i;

				mapColprintAddLanguage (table, type, parser);
			}
			else
				printMaps (i, type);
		}
	}
	else
	{
		Assert (0 <= language  &&  language < (int) LanguageCount);

		if (type & LMAP_TABLE_OUTPUT)
		{
			const parserObject* parser = LanguageTable + language;

			mapColprintAddLanguage (table, type, parser);
		}
		else
			printMaps (language, type);
	}


	if (type & LMAP_TABLE_OUTPUT)
	{
		colprintTablePrint (table, (language == LANG_AUTO)? 0: 1,
							withListHeader, machinable, fp);
		colprintTableDelete (table);
	}
}

static struct colprintTable *aliasColprintTableNew (void)
{
	return colprintTableNew ("L:LANGUAGE", "L:ALIAS", NULL);
}

static void aliasColprintAddLanguage (struct colprintTable * table,
									  const parserObject* parser)
{
	unsigned int count;

	if (parser->currentAliases && (0 < (count = stringListCount (parser->currentAliases))))
	{
		for (unsigned int i = 0; i < count; i++)
		{
			struct colprintLine * line = colprintTableGetNewLine (table);
			vString *alias = stringListItem (parser->currentAliases, i);

			colprintLineAppendColumnCString (line, parser->def->name);
			colprintLineAppendColumnVString (line, alias);
		}
	}
}

extern void printLanguageAliases (const langType language,
								  bool withListHeader, bool machinable, FILE *fp)
{
	/* DON'T SORT THE LIST

	   The order of listing should be equal to the order of matching
	   for the parser selection. */

	struct colprintTable * table = aliasColprintTableNew();
	const parserObject* parser;

	if (language == LANG_AUTO)
	{
		for (unsigned int i = 0; i < LanguageCount; ++i)
		{
			parser = LanguageTable + i;
			if (parser->def->invisible)
				continue;

			aliasColprintAddLanguage (table, parser);
		}
	}
	else
	{
		Assert (0 <= language  &&  language < (int) LanguageCount);
		parser = LanguageTable + language;
		aliasColprintAddLanguage (table, parser);
	}

	colprintTablePrint (table, (language == LANG_AUTO)? 0: 1,
						withListHeader, machinable, fp);
	colprintTableDelete (table);
}

static void printLanguage (const langType language, parserDefinition** ltable)
{
	const parserDefinition* lang;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = ltable [language];

	if (lang->invisible)
		return;

	printf ("%s%s\n", lang->name, isLanguageEnabled (lang->id) ? "" : " [disabled]");
}

extern void printLanguageList (enum parserCategory category)
{
	unsigned int i;
	unsigned int n;
	parserDefinition **ltable;

	ltable = xMalloc (LanguageCount, parserDefinition*);
	for (i = 0, n = 0 ; i < LanguageCount ; ++i)
	{
		parserObject *pobj = LanguageTable + i;
		if (category != PARSER_CATEGORY_NONE)
		{
			if (getCategoryForParserFunc(pobj->parserDefFunc) != category)
				continue;
		}
		ltable[n] = pobj->def;
		++n;
	}
	qsort (ltable, n, sizeof (parserDefinition*), compareParsersByName);

	for (i = 0  ;  i < n  ;  ++i)
		printLanguage (i, ltable);

	eFree (ltable);
}

static void xtagDefinitionDestroy (xtagDefinition *xdef)
{
	eFree ((void *)xdef->name);
	eFree ((void *)xdef->description);
	eFree (xdef);
}

static bool processLangDefineExtra (const langType language,
									const char *const option,
									const char *const parameter)
{
	xtagDefinition *xdef;
	const char * p = parameter;
	const char *name_end;
	const char *desc;
	const char *flags;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no extra definition specified in \"--%s\" option", option);

	name_end = strchr (p, ',');
	if (!name_end)
		error (FATAL, "no extra description specified in \"--%s\" option", option);
	else if (name_end == p)
		error (FATAL, "the extra name in \"--%s\" option is empty", option);

	for (; p < name_end; p++)
	{
		if (!isalnum ((unsigned char) *p))
			error (FATAL, "unacceptable char as part of extra name in \"--%s\" option: '%c'",
				   option, *p);
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "extra description in \"--%s\" option is empty", option);

	desc = extractDescriptionAndFlags (p, &flags);

	xdef = xCalloc (1, xtagDefinition);
	xdef->enabled = false;
	xdef->letter = NUL_XTAG_LETTER;
	xdef->name = eStrndup (parameter, name_end - parameter);
	xdef->description = desc;
	xdef->isEnabled = NULL;
	DEFAULT_TRASH_BOX(xdef, xtagDefinitionDestroy);

	if (flags)
		flagsEval (flags, NULL, 0, xdef);

	defineXtag (xdef, language);

	return true;
}

extern bool processExtradefOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_" "extradef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineExtra (language, option, parameter);
}

static void fieldDefinitionDestroy (fieldDefinition *fdef)
{
	eFree ((void *)fdef->description);
	eFree ((void *)fdef->name);
	eFree (fdef);
}

static void field_def_flag_datatype_long (const char *const optflag CTAGS_ATTR_UNUSED,
										  const char* const param,
										  void *data)
{
	fieldDefinition *fdef = data;

	if (*param == '\0')
		error (FATAL, "no datatype given for field: \"%s\"", fdef->name);

	fdef->dataType = 0;
	if (strcmp (param, "int") == 0)
		fdef->dataType |= FIELDTYPE_INTEGER;
	else if (strcmp (param, "str") == 0)
		fdef->dataType |= FIELDTYPE_STRING;
	else if (strcmp (param, "bool") == 0)
		fdef->dataType |= FIELDTYPE_BOOL;
	else if (strcmp (param, "str+bool") == 0)
		fdef->dataType |= FIELDTYPE_STRING|FIELDTYPE_BOOL;
	else
		error (FATAL, "unknown datatype for field \"%s\": \"%s\"", fdef->name, param);
}

static flagDefinition FieldDefFlagDef [] = {
	{ '\0', "datatype", NULL, field_def_flag_datatype_long,
	  "TYPE", "acceaptable datatype of the field ([str]|bool|int|str+bool)" },
};

static bool processLangDefineField (const langType language,
									const char *const option,
									const char *const parameter)
{
	fieldDefinition *fdef;
	const char * p = parameter;
	const char *name_end;
	const char *desc;
	const char *flags;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	Assert (p);

	if (p[0] == '\0')
		error (FATAL, "no field definition specified in \"--%s\" option", option);

	name_end = strchr (p, ',');
	if (!name_end)
		error (FATAL, "no field description specified in \"--%s\" option", option);
	else if (name_end == p)
		error (FATAL, "the field name in \"--%s\" option is empty", option);

	for (; p < name_end; p++)
	{
		if (!isalpha ((unsigned char) *p))
			error (FATAL, "unacceptable char as part of field name in \"--%s\" option: '%c'",
				   option, *p);
	}

	p++;
	if (p [0] == '\0' || p [0] == LONG_FLAGS_OPEN)
		error (FATAL, "field description in \"--%s\" option is empty", option);

	desc = extractDescriptionAndFlags (p, &flags);

	fdef = xCalloc (1, fieldDefinition);
	fdef->enabled = false;
	fdef->letter = NUL_FIELD_LETTER;
	fdef->name = eStrndup(parameter, name_end - parameter);
	fdef->description = desc;
	fdef->isValueAvailable = NULL;
	fdef->getValueObject = NULL;
	fdef->getterValueType = NULL;
	fdef->setValueObject = NULL;
	fdef->setterValueType = NULL;
	fdef->checkValueForSetter = NULL;
	fdef->dataType = 0;
	if (flags)
		flagsEval (flags, FieldDefFlagDef, ARRAY_SIZE (FieldDefFlagDef), fdef);
	if (!fdef->dataType)
		fdef->dataType = FIELDTYPE_STRING;
	fdef->ftype = FIELD_UNKNOWN;
	DEFAULT_TRASH_BOX(fdef, fieldDefinitionDestroy);

	defineField (fdef, language);

	return true;
}

extern bool processFielddefOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_fielddef-");
	if (language == LANG_IGNORE)
		return false;

	return processLangDefineField (language, option, parameter);
}

/*
*   File parsing
*/

static rescanReason createTagsForFile (const langType language,
				       const unsigned int passCount)
{
	parserDefinition *const lang = LanguageTable [language].def;
	rescanReason rescan = RESCAN_NONE;

	resetInputFile (language, passCount > 1);

	Assert (lang->parser || lang->parser2);

	notifyInputStart ();

	if (lang->parser != NULL)
		lang->parser ();
	else if (lang->parser2 != NULL)
		rescan = lang->parser2 (passCount);

	notifyInputEnd ();

	return rescan;
}

extern void notifyLanguageRegexInputStart (langType language)
{
	parserObject *pobj = LanguageTable + language;

	notifyRegexInputStart(pobj->lregexControlBlock);
}

extern void notifyLanguageRegexInputEnd (langType language)
{
	parserObject *pobj = LanguageTable + language;

	notifyRegexInputEnd(pobj->lregexControlBlock);
}

extern void notifyLanguageRegexMakeTagEntry (langType language, int corkIndex)
{
	notifyRegexMakeTagEntry((LanguageTable + language)->lregexControlBlock, corkIndex);
}

static unsigned int parserCorkFlags (parserDefinition *parser)
{
	subparser *tmp;
	unsigned int r = 0;

	r |= parser->useCork;

	if (doesLanguageExpectCorkInRegex (parser->id)
	    || parser->requestAutomaticFQTag)
		r |= CORK_QUEUE;

	pushLanguage (parser->id);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		r |= parserCorkFlags (LanguageTable[t].def);
	}
	popLanguage ();
	return r;
}

static void setupLanguageSubparsersInUse (const langType language)
{
	subparser *tmp;

	setupSubparsersInUse ((LanguageTable + language)->slaveControlBlock);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		setupLanguageSubparsersInUse(t);
		leaveSubparser ();
	}
}

static subparser* teardownLanguageSubparsersInUse (const langType language)
{
	subparser *tmp;

	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		teardownLanguageSubparsersInUse(t);
		leaveSubparser ();
	}
	return teardownSubparsersInUse ((LanguageTable + language)->slaveControlBlock);
}

static void	initializeParserStats (parserObject *parser)
{
	if (Option.printTotals > 1 && parser->used == 0 && parser->def->initStats)
		parser->def->initStats (parser->def->id);
	parser->used = 1;
}

extern void printParserStatisticsIfUsed (langType language)
{
	parserObject *parser = &(LanguageTable [language]);

	if (parser->used)
	{
		if (parser->def->printStats)
		{
			fprintf(stderr, "\nSTATISTICS of %s\n", getLanguageName (language));
			fputs("==============================================\n", stderr);
			parser->def->printStats (language);
		}
		printLanguageMultitableStatistics (language);
	}
}

static bool createTagsWithFallback1 (const langType language,
									 langType *exclusive_subparser)
{
	bool tagFileResized = false;
	unsigned long numTags;
	MIOPos tagfpos;
	int lastPromise = getLastPromise ();
	unsigned int passCount = 0;
	rescanReason whyRescan;
	parserObject *parser;
	unsigned int corkFlags;
	bool useCork = false;

	initializeParser (language);
	parser = &(LanguageTable [language]);

	setupLanguageSubparsersInUse (language);

	corkFlags = parserCorkFlags (parser->def);
	useCork = corkFlags & CORK_QUEUE;
	if (useCork)
		corkTagFile(corkFlags);

	if (isXtagEnabled (XTAG_PSEUDO_TAGS))
		addParserPseudoTags (language);
	initializeParserStats (parser);
	numTags = numTagsAdded ();
	tagFilePosition (&tagfpos);

	anonResetMaybe (parser);
	parser->justRunForSchedulingBase = 0;

	while ( ( whyRescan =
		  createTagsForFile (language, ++passCount) )
		!= RESCAN_NONE)
	{
		if (useCork)
		{
			uncorkTagFile();
			corkTagFile(corkFlags);
		}


		if (whyRescan == RESCAN_FAILED)
		{
			/*  Restore prior state of tag file.
			*/
			setTagFilePosition (&tagfpos, true);
			setNumTagsAdded (numTags);
			writerRescanFailed (numTags);
			tagFileResized = true;
			breakPromisesAfter(lastPromise);
		}
		else if (whyRescan == RESCAN_APPEND)
		{
			tagFilePosition (&tagfpos);
			numTags = numTagsAdded ();
			lastPromise = getLastPromise ();
		}
	}

	if (!parser->justRunForSchedulingBase
		/* Force applying regex patterns */
		&& hasLanguageAnyRegexPatterns (language))
		while (readLineFromInputFile () != NULL)
			; /* Do nothing */

	if (useCork)
		uncorkTagFile();

	{
		subparser *s = teardownLanguageSubparsersInUse (language);
		if (exclusive_subparser && s)
			*exclusive_subparser = getSubparserLanguage (s);
	}

	return tagFileResized;
}

extern bool runParserInNarrowedInputStream (const langType language,
					       unsigned long startLine, long startCharOffset,
					       unsigned long endLine, long endCharOffset,
					       unsigned long sourceLineOffset,
					       int promise)
{
	bool tagFileResized;

	verbose ("runParserInNarrowedInputStream: %s; "
			 "file: %s, "
			 "start(line: %lu, offset: %ld, srcline: %lu)"
			 " - "
			 "end(line: %lu, offset: %ld)\n",
			 getLanguageName (language),
			 getInputFileName (),
			 startLine, startCharOffset, sourceLineOffset,
			 endLine, endCharOffset);

	pushNarrowedInputStream (
				 doesParserRequireMemoryStream (language),
				 startLine, startCharOffset,
				 endLine, endCharOffset,
				 sourceLineOffset,
				 promise);
	tagFileResized = createTagsWithFallback1 (language, NULL);
	popNarrowedInputStream  ();
	return tagFileResized;

}

static bool createTagsWithFallback (
	const char *const fileName, const langType language,
	MIO *mio, time_t mtime, bool *failureInOpenning)
{
	langType exclusive_subparser = LANG_IGNORE;
	bool tagFileResized = false;

	Assert (0 <= language  &&  language < (int) LanguageCount);

	if (!openInputFile (fileName, language, mio, mtime))
	{
		*failureInOpenning = true;
		return false;
	}
	*failureInOpenning = false;

	tagFileResized = createTagsWithFallback1 (language,
											  &exclusive_subparser);
	tagFileResized = forcePromises()? true: tagFileResized;

	pushLanguage ((exclusive_subparser == LANG_IGNORE)
				  ? language
				  : exclusive_subparser);
	makeFileTag (fileName);
	popLanguage ();
	closeInputFile ();

	return tagFileResized;
}

static void printGuessedParser (const char* const fileName, langType language)
{
	const char *parserName;

	if (language == LANG_IGNORE)
	{
		Option.printLanguage = ((int)true) + 1;
		parserName = RSV_NONE;
	}
	else
	{
		parserName = getLanguageName (language);
	}

	printf("%s: %s\n", fileName, parserName);
}

#ifdef HAVE_ICONV
static char **EncodingMap;
static unsigned int EncodingMapMax;

static void addLanguageEncoding (const langType language,
									const char *const encoding)
{
	if (language > EncodingMapMax || EncodingMapMax == 0)
	{
		int i;
		int istart = (EncodingMapMax == 0)? 0: EncodingMapMax + 1;
		EncodingMap = xRealloc (EncodingMap, (language + 1), char*);
		for (i = istart;  i <= language  ;  ++i)
		{
			EncodingMap [i] = NULL;
		}
		EncodingMapMax = language;
	}
	if (EncodingMap [language])
		eFree (EncodingMap [language]);
	EncodingMap [language] = eStrdup(encoding);
	if (!Option.outputEncoding)
		Option.outputEncoding = eStrdup("UTF-8");
}

extern bool processLanguageEncodingOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "input-encoding-");
	if (language == LANG_IGNORE)
		return false;

	addLanguageEncoding (language, parameter);
	return true;
}

extern void freeEncodingResources (void)
{
	if (EncodingMap)
	{
		unsigned int i;
		for (i = 0  ;  i <= EncodingMapMax  ; ++i)
		{
			if (EncodingMap [i])
				eFree (EncodingMap [i]);
		}
		eFree (EncodingMap);
	}
	if (Option.inputEncoding)
		eFree (Option.inputEncoding);
	if (Option.outputEncoding)
		eFree (Option.outputEncoding);
}

extern const char *getLanguageEncoding (const langType language)
{
	if (EncodingMap && language <= EncodingMapMax && EncodingMap [language])
		return EncodingMap[language];
	else
		return Option.inputEncoding;
}
#endif

static void addParserPseudoTags (langType language)
{
	parserObject *parser = LanguageTable + language;
	if (!parser->pseudoTagPrinted)
	{
		for (int i = 0; i < PTAG_COUNT; i++)
		{
			if (isPtagParserSpecific (i))
				makePtagIfEnabled (i, language, parser);
		}
		parser->pseudoTagPrinted = 1;
	}
}

extern bool doesParserRequireMemoryStream (const langType language)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parserDefinition *const lang = LanguageTable [language].def;
	unsigned int i;

	if (lang->tagXpathTableCount > 0
		|| lang->useMemoryStreamInput)
	{
		verbose ("%s requires a memory stream for input\n", lang->name);
		return true;
	}

	for (i = 0; i < lang->dependencyCount; i++)
	{
		parserDependency *d = lang->dependencies + i;
		if (d->type == DEPTYPE_SUBPARSER &&
			((subparser *)(d->data))->direction & SUBPARSER_SUB_RUNS_BASE)
		{
			langType baseParser;
			baseParser = getNamedLanguage (d->upperParser, 0);
			if (doesParserRequireMemoryStream(baseParser))
			{
				verbose ("%s/%s requires a memory stream for input\n", lang->name,
						 LanguageTable[baseParser].def->name);
				return true;
			}
		}
	}

	return false;
}

extern bool parseFile (const char *const fileName)
{
	TRACE_ENTER_TEXT("Parsing file %s",fileName);
	bool bRet = parseFileWithMio (fileName, NULL, NULL);
	TRACE_LEAVE();
	return bRet;
}

static bool parseMio (const char *const fileName, langType language, MIO* mio, time_t mtime, bool useSourceFileTagPath,
					  void *clientData)
{
	bool tagFileResized = false;
	bool failureInOpenning = false;

	setupWriter (clientData);

	setupAnon ();

	initParserTrashBox ();

	tagFileResized = createTagsWithFallback (fileName, language, mio, mtime, &failureInOpenning);

	finiParserTrashBox ();

	teardownAnon ();

	if (useSourceFileTagPath && (!failureInOpenning))
		return teardownWriter (getSourceFileTagPath())? true: tagFileResized;
	else
		return teardownWriter(fileName);
}

extern bool parseFileWithMio (const char *const fileName, MIO *mio,
							  void *clientData)
{
	bool tagFileResized = false;
	langType language;
	struct GetLanguageRequest req = {
		.type = mio? GLR_REUSE: GLR_OPEN,
		.fileName = fileName,
		.mio = mio,
	};
	memset (&req.mtime, 0, sizeof (req.mtime));

	language = getFileLanguageForRequest (&req);
	Assert (language != LANG_AUTO);

	if (Option.printLanguage)
	{
		printGuessedParser (fileName, language);
		return tagFileResized;
	}

	if (language == LANG_IGNORE)
		verbose ("ignoring %s (unknown language/language disabled)\n",
			 fileName);
	else
	{
		Assert(isLanguageEnabled (language));

		if (Option.filter && ! Option.interactive)
			openTagFile ();

#ifdef HAVE_ICONV
		/* TODO: checkUTF8BOM can be used to update the encodings. */
		openConverter (getLanguageEncoding (language), Option.outputEncoding);
#endif
		tagFileResized = parseMio (fileName, language, req.mio, req.mtime, true, clientData);
		if (Option.filter && ! Option.interactive)
			closeTagFile (tagFileResized);
		addTotals (1, 0L, 0L);

#ifdef HAVE_ICONV
		closeConverter ();
#endif
	}

	if (req.type == GLR_OPEN && req.mio)
		mio_unref (req.mio);

	return tagFileResized;
}

extern bool parseRawBuffer(const char *fileName, unsigned char *buffer,
			 size_t bufferSize, const langType language, void *clientData)
{
	MIO *mio = NULL;
	bool r;

	if (buffer)
		mio = mio_new_memory (buffer, bufferSize, NULL, NULL);

	r = parseMio (fileName, language, mio, (time_t)0, false, clientData);

	if (buffer)
		mio_unref (mio);

	return r;
}

static void matchLanguageMultilineRegexCommon (const langType language,
											   bool (* func) (struct lregexControlBlock *, const vString* const),
											   const vString* const allLines)
{
	subparser *tmp;

	func ((LanguageTable + language)->lregexControlBlock, allLines);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		matchLanguageMultilineRegexCommon (t, func, allLines);
		leaveSubparser ();
	}
}

extern void matchLanguageMultilineRegex (const langType language,
										 const vString* const allLines)
{
	matchLanguageMultilineRegexCommon(language, matchMultilineRegex, allLines);
}

extern void matchLanguageMultitableRegex (const langType language,
										  const vString* const allLines)
{
	matchLanguageMultilineRegexCommon(language, matchMultitableRegex, allLines);
}

extern void processLanguageMultitableExtendingOption (langType language, const char *const parameter)
{
	const char* src;
	char* dist;
	const char *tmp;

	tmp = strchr(parameter, '+');

	if (!tmp)
		error (FATAL, "no separator(+) found: %s", parameter);

	if (tmp == parameter)
		error (FATAL, "the name of source table is empty in table extending: %s", parameter);

	src = tmp + 1;
	if (!*src)
		error (FATAL, "the name of dist table is empty in table extending: %s", parameter);

	dist = eStrndup(parameter, tmp  - parameter);
	extendRegexTable(((LanguageTable + language)->lregexControlBlock), src, dist);
	eFree (dist);
}

static bool lregexQueryParserAndSubparsers (const langType language, bool (* predicate) (struct lregexControlBlock *))
{
	bool r;
	subparser *tmp;

	r = predicate ((LanguageTable + language)->lregexControlBlock);
	if (!r)
	{
		foreachSubparser(tmp, true)
		{
			langType t = getSubparserLanguage (tmp);
			enterSubparser (tmp);
			r = lregexQueryParserAndSubparsers (t, predicate);
			leaveSubparser ();

			if (r)
				break;
		}
	}

	return r;
}

extern bool hasLanguagePostRunRegexPatterns (const langType language)
{
	return lregexQueryParserAndSubparsers (language, regexIsPostRun);
}

extern bool hasLanguageMultilineRegexPatterns (const langType language)
{
	return lregexQueryParserAndSubparsers (language, regexNeedsMultilineBuffer);
}

static bool hasLanguageAnyRegexPatterns (const langType language)
{
	return lregexQueryParserAndSubparsers (language, lregexControlBlockHasAny);
}

extern void addLanguageCallbackRegex (const langType language, const char *const regex, const char *const flags,
									  const regexCallback callback, bool *disabled, void *userData)
{
	addCallbackRegex ((LanguageTable +language)->lregexControlBlock, regex, flags, callback, disabled, userData);
}

extern bool doesLanguageExpectCorkInRegex (const langType language)
{
	bool hasScopeAction;

	pushLanguage (language);
	hasScopeAction = lregexQueryParserAndSubparsers (language, doesExpectCorkInRegex);
	popLanguage ();

	return hasScopeAction;
}

extern void matchLanguageRegex (const langType language, const vString* const line, bool postrun)
{
	subparser *tmp;

	matchRegex ((LanguageTable + language)->lregexControlBlock, line, postrun);
	foreachSubparser(tmp, true)
	{
		langType t = getSubparserLanguage (tmp);
		enterSubparser (tmp);
		matchLanguageRegex (t, line, postrun);
		leaveSubparser ();
	}
}

extern bool processLanguageRegexOption (langType language,
										enum regexParserType regptype,
										const char *const parameter)
{
	processTagRegexOption ((LanguageTable +language)->lregexControlBlock,
						   regptype, parameter);

	return true;
}

extern bool processTabledefOption (const char *const option, const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_tabledef-");
	if (language == LANG_IGNORE)
		return false;

	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "A parameter is needed after \"%s\" option", option);

	addRegexTable((LanguageTable +language)->lregexControlBlock, parameter);
	return true;
}

extern void useRegexMethod (const langType language)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;
	lang->method |= METHOD_REGEX;
}

static void useXpathMethod (const langType language)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;
	lang->method |= METHOD_XPATH;
}

static void installTagRegexTable (const langType language)
{
	parserObject* parser;
	parserDefinition* lang;
	unsigned int i;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	lang = parser->def;


	if (lang->tagRegexTable != NULL)
	{
		/* ctags_cli_main() calls initRegexOptscript ().
		 * However, mini-geany deasn't call ctags_cli_main().
		 * So we call initRegexOptscript () here.
		 */
		initRegexOptscript ();

	    for (i = 0; i < lang->tagRegexCount; ++i)
		{
			if (lang->tagRegexTable [i].mline)
				addTagMultiLineRegex (parser->lregexControlBlock,
									  lang->tagRegexTable [i].regex,
									  lang->tagRegexTable [i].name,
									  lang->tagRegexTable [i].kinds,
									  lang->tagRegexTable [i].flags,
									  (lang->tagRegexTable [i].disabled));
			else
				addTagRegex (parser->lregexControlBlock,
							 lang->tagRegexTable [i].regex,
							 lang->tagRegexTable [i].name,
							 lang->tagRegexTable [i].kinds,
							 lang->tagRegexTable [i].flags,
							 (lang->tagRegexTable [i].disabled));
		}
	}
}

static void installKeywordTable (const langType language)
{
	parserDefinition* lang;
	unsigned int i;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	if (lang->keywordTable != NULL)
	{
		for (i = 0; i < lang->keywordCount; ++i)
			addKeyword (lang->keywordTable [i].name,
				    language,
				    lang->keywordTable [i].id);
	}
}

static void installTagXpathTable (const langType language)
{
	parserDefinition* lang;
	unsigned int i, j;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	if (lang->tagXpathTableTable != NULL)
	{
		for (i = 0; i < lang->tagXpathTableCount; ++i)
			for (j = 0; j < lang->tagXpathTableTable[i].count; ++j)
				addTagXpath (language, lang->tagXpathTableTable[i].table + j);
		useXpathMethod (language);
	}
}

static void uninstallTagXpathTable (const langType language)
{
	parserDefinition* lang;
	unsigned int i, j;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	if (lang->tagXpathTableTable != NULL)
	{
		for (i = 0; i < lang->tagXpathTableCount; ++i)
			for (j = 0; j < lang->tagXpathTableTable[i].count; ++j)
				removeTagXpath (language, lang->tagXpathTableTable[i].table + j);
	}
}

const tagXpathTableTable *getXpathTableTable (const langType language, unsigned int nth)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	Assert (nth < lang->tagXpathTableCount);
	return lang->tagXpathTableTable + nth;
}

extern unsigned int getXpathFileSpecCount (const langType language)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	return lang->xpathFileSpecCount;
}

extern xpathFileSpec* getXpathFileSpec (const langType language, unsigned int nth)
{
	parserDefinition* lang;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	lang = LanguageTable [language].def;

	Assert (nth < lang->xpathFileSpecCount);
	return lang->xpathFileSpecs + nth;
}

extern bool makeKindSeparatorsPseudoTags (const langType language,
					     const ptagDesc *pdesc)
{
	parserObject* parser;
	parserDefinition* lang;
	struct kindControlBlock *kcb;
	kindDefinition *kind;
	unsigned int kindCount;
	unsigned int i, j;

	bool r = false;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	lang = parser->def;
	kcb = parser->kindControlBlock;
	kindCount = countKinds(kcb);

	if (kindCount == 0)
		return r;

	for (i = 0; i < kindCount; ++i)
	{
		kind = getKind (kcb, i);
		for (j = 0; j < kind->separatorCount; ++j)
		{
			char name[3] = {[1] = '\0', [2] = '\0'};
			const kindDefinition *upperKind;
			const scopeSeparator *sep;

			sep = kind->separators + j;

			if (sep->parentKindIndex == KIND_WILDCARD_INDEX)
			{
				name[0] = KIND_WILDCARD_LETTER;
				name[1] = kind->letter;
			}
			else if (sep->parentKindIndex == KIND_GHOST_INDEX)
			{
				/* This is root separator: no upper item is here. */
				name[0] = kind->letter;
			}
			else
			{
				upperKind = getLanguageKind (language,
							    sep->parentKindIndex);
				if (!upperKind)
					continue;

				name[0] = upperKind->letter;
				name[1] = kind->letter;
			}


			r = writePseudoTag (pdesc, sep->separator? sep->separator: "",
					    name, lang->name) || r;
		}
	}

	return r;
}

struct makeKindDescriptionPseudoTagData {
	const char* langName;
	const ptagDesc *pdesc;
	bool written;
};

static bool makeKindDescriptionPseudoTag (kindDefinition *kind,
					     void *user_data)
{
	struct makeKindDescriptionPseudoTagData *data = user_data;
	vString *letter_and_name;

	letter_and_name = vStringNew ();

	vStringPut (letter_and_name, kind -> letter);
	vStringPut (letter_and_name, ',');
	vStringCatS (letter_and_name, kind -> name);

	data->written |=  writePseudoTag (data->pdesc, vStringValue (letter_and_name),
									  kind->description? kind->description: kind->name,
									  data->langName);

	vStringDelete (letter_and_name);

	return false;
}

static bool makeRoleDescriptionPseudoTag (kindDefinition *kind,
										  roleDefinition *role,
										  void *user_data)
{
	struct makeKindDescriptionPseudoTagData *data = user_data;

	vString *parser_and_kind_name = vStringNewInit (data->langName);
	vStringCatS (parser_and_kind_name, PSEUDO_TAG_SEPARATOR);
	vStringCatS (parser_and_kind_name, kind->name);

	data->written |=  writePseudoTag (data->pdesc, role->name,
									  role->description? role->description: role->name,
									  vStringValue (parser_and_kind_name));

	vStringDelete (parser_and_kind_name);

	return false;
}

extern bool makeKindDescriptionsPseudoTags (const langType language,
					    const ptagDesc *pdesc)
{
	parserObject *parser;
	struct kindControlBlock *kcb;
	parserDefinition* lang;
	kindDefinition *kind;
	unsigned int kindCount, i;
	struct makeKindDescriptionPseudoTagData data;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	kcb = parser->kindControlBlock;
	lang = parser->def;

	kindCount = countKinds(kcb);

	data.langName = lang->name;
	data.pdesc = pdesc;
	data.written = false;

	for (i = 0; i < kindCount; ++i)
	{
		if (!isLanguageKindEnabled (language, i))
			continue;

		kind = getKind (kcb, i);
		if (language == ctagsSelfTestLang
			&& (kind == NULL || kind->name == NULL))
		{
			/* The Self test parser may have broken kinds.
			 * Let's skip it. */
			continue;
		}
		makeKindDescriptionPseudoTag (kind, &data);
	}

	return data.written;
}

static bool makeFieldDescriptionPseudoTag (const langType language,
										   fieldType f,
										   const ptagDesc *pdesc)
{
	const char *name = getFieldName (f);

	if (name == NULL || name [0] == '\0')
		return false;

	return writePseudoTag (pdesc, name,
						   getFieldDescription (f),
						   language == LANG_IGNORE? NULL: getLanguageName (language));
}

extern bool makeFieldDescriptionsPseudoTags (const langType language,
											 const ptagDesc *pdesc)
{
	bool written = false;
	for (unsigned int i = 0; i < countFields (); i++)
	{
		if (getFieldLanguage (i) == language
			&& isFieldEnabled (i))
		{
			if (makeFieldDescriptionPseudoTag (language, i, pdesc))
				written = true;
		}
	}
	return written;
}

static bool makeExtraDescriptionPseudoTag (const langType language,
										   xtagType x,
										   const ptagDesc *pdesc)
{
	const char *name = getXtagName (x);

	if (name == NULL || name [0] == '\0')
		return false;

	return writePseudoTag (pdesc, name,
						   getXtagDescription (x),
						   language == LANG_IGNORE? NULL: getLanguageName (language));
}

extern bool makeExtraDescriptionsPseudoTags (const langType language,
											 const ptagDesc *pdesc)
{
	bool written = false;
	for (unsigned int i = 0; i < countXtags (); i++)
	{
		if (getXtagLanguage (i) == language
			&& isXtagEnabled (i))
		{
			if (makeExtraDescriptionPseudoTag (language, i, pdesc))
				written = true;
		}
	}
	return written;
}

extern bool makeRoleDescriptionsPseudoTags (const langType language,
											const ptagDesc *pdesc)
{
	parserObject *parser;
	struct kindControlBlock *kcb;
	parserDefinition* lang;
	kindDefinition *kind;
	struct makeKindDescriptionPseudoTagData data;

	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	kcb = parser->kindControlBlock;
	lang = parser->def;

	unsigned int kindCount = countKinds(kcb);

	data.langName = lang->name;
	data.pdesc = pdesc;
	data.written = false;

	for (unsigned int i = 0; i < kindCount; ++i)
	{
		if (!isLanguageKindEnabled (language, i))
			continue;

		kind = getKind (kcb, i);

		unsigned int roleCount = countRoles (kcb, i);
		for (unsigned int j = 0; j < roleCount; ++j)
		{
			if (isRoleEnabled (kcb, i, j))
			{
				roleDefinition *role = getRole (kcb, i, j);
				makeRoleDescriptionPseudoTag (kind, role, &data);
			}
		}
	}

	return data.written;
}

extern unsigned int getLanguageVersionCurrent (const langType language)
{
	parserObject *parser;
	parserDefinition* lang;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	lang = parser->def;
	return lang->versionCurrent;
}

extern unsigned int getLanguageVersionAge (const langType language)
{
	parserObject *parser;
	parserDefinition* lang;
	Assert (0 <= language  &&  language < (int) LanguageCount);
	parser = LanguageTable + language;
	lang = parser->def;
	return lang->versionAge;
}

/*
*   Copyright (c) 2016, Szymon Tomasz Stefanek
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   Anonymous name generator
*/
static ptrArray *parsersUsedInCurrentInput;

static void setupAnon (void)
{
	parsersUsedInCurrentInput = ptrArrayNew (NULL);
}

static void teardownAnon (void)
{
	ptrArrayDelete (parsersUsedInCurrentInput);
}

static void anonResetMaybe (parserObject *parser)
{
	if (ptrArrayHas (parsersUsedInCurrentInput, parser))
		return;

	parser -> anonymousIdentiferId = 0;
	ptrArrayAdd (parsersUsedInCurrentInput, parser);
}

static unsigned int anonHash(const unsigned char *str)
{
	unsigned int hash = 5381;
	int c;

	while((c = *str++))
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

	return hash ;
}

extern void anonHashString (const char *filename, char buf[9])
{
	sprintf(buf, "%08x", anonHash((const unsigned char *)filename));
}

extern void anonConcatFull (vString *buffer, langType lang, int kind)
{
	anonGenerateFull (buffer, NULL, lang, kind);
}

extern void anonGenerateFull (vString *buffer, const char *prefix, langType lang, int kind)
{
	Assert(lang != LANG_IGNORE);
	parserObject* parser = LanguageTable + ((lang == LANG_AUTO)? getInputLanguage (): lang);
	parser -> anonymousIdentiferId ++;

	char szNum[32];
	char buf [9];

	if (prefix)
		vStringCopyS(buffer, prefix);

	anonHashString (getInputFileName(), buf);
	sprintf(szNum,"%s%02x%02x",buf,parser -> anonymousIdentiferId, kind);
	vStringCatS(buffer,szNum);
}

extern vString *anonGenerateNewFull (const char *prefix, langType lang, int kind)
{
	vString *buffer = vStringNew ();

	anonGenerateFull (buffer, prefix, lang, kind);
	return buffer;
}

extern bool applyLanguageParam (const langType language, const char *name, const char *args)
{
	Assert (0 <= language  &&  language < (int) LanguageCount);

	initializeParserOne (language);
	return applyParam (LanguageTable [language].paramControlBlock, name, args);
}

extern subparser *getNextSubparser(subparser *last,
								   bool includingNoneCraftedParser)
{
	langType lang = getInputLanguage ();
	parserObject *parser = LanguageTable + lang;
	subparser *r;
	langType t;

	if (last == NULL)
		r = getFirstSubparser(parser->slaveControlBlock);
	else
		r = last->next;

	if (r == NULL)
		return r;

	t = getSubparserLanguage(r);
	if (isLanguageEnabled (t) &&
		(includingNoneCraftedParser
		 || ((((LanguageTable + t)->def->method) & METHOD_NOT_CRAFTED) == 0)))
		return r;
	else
		return getNextSubparser (r, includingNoneCraftedParser);
}

extern slaveParser *getNextSlaveParser(slaveParser *last)
{
	langType lang = getInputLanguage ();
	parserObject *parser = LanguageTable + lang;
	slaveParser *r;

	if (last == NULL)
		r = getFirstSlaveParser(parser->slaveControlBlock);
	else
		r = last->next;

	return r;
}

extern void scheduleRunningBaseparser (int dependencyIndex)
{
	langType current = getInputLanguage ();
	parserObject *current_pobj = LanguageTable + current;
	parserDefinition *current_parser = current_pobj->def;
	parserDependency *dep = NULL;

	current_pobj->justRunForSchedulingBase = 1;

	if (dependencyIndex == RUN_DEFAULT_SUBPARSERS)
	{
		for (unsigned int i = 0; i < current_parser->dependencyCount; ++i)
			if (current_parser->dependencies[i].type == DEPTYPE_SUBPARSER)
			{
				dep = current_parser->dependencies + i;
				break;
			}
	}
	else
		dep = current_parser->dependencies + dependencyIndex;

	if (dep == NULL)
		return;

	const char *base_name = dep->upperParser;
	langType base = getNamedLanguage (base_name, 0);
	parserObject *base_parser = LanguageTable + base;

	if (dependencyIndex == RUN_DEFAULT_SUBPARSERS)
		useDefaultSubparsers(base_parser->slaveControlBlock);
	else
		useSpecifiedSubparser (base_parser->slaveControlBlock,
							   dep->data);

	if (!isLanguageEnabled (base))
	{
		enableLanguage (base, true);
		base_parser->dontEmit = true;
		verbose ("force enable \"%s\" as base parser\n", base_parser->def->name);
	}

	{
		subparser *tmp;

		verbose ("scheduleRunningBaseparser %s with subparsers: ", base_name);
		pushLanguage (base);
		foreachSubparser(tmp, true)
		{
			langType t = getSubparserLanguage (tmp);
			verbose ("%s ", getLanguageName (t));
		}
		popLanguage ();
		verbose ("\n");
	}


	makePromise(base_name, THIN_STREAM_SPEC);
}

extern bool isParserMarkedNoEmission (void)
{
	langType lang = getInputLanguage();
	parserObject *parser = LanguageTable + lang;

	return parser->dontEmit;
}


extern subparser* getSubparserRunningBaseparser (void)
{
	langType current = getInputLanguage ();
	parserObject *current_parser = LanguageTable + current;
	subparser *s = getFirstSubparser (current_parser->slaveControlBlock);

	if (s && s->schedulingBaseparserExplicitly)
		return s;
	else
		return NULL;
}

extern void printLanguageSubparsers (const langType language,
									 bool withListHeader, bool machinable, FILE *fp)
{
	for (int i = 0; i < (int) LanguageCount; i++)
		initializeParserOne (i);

	struct colprintTable * table = subparserColprintTableNew();
	parserObject *parser;

	if (language == LANG_AUTO)
	{
		for (int i = 0; i < (int) LanguageCount; i++)
		{
			parser = LanguageTable + i;
			if (parser->def->invisible)
				continue;

			subparserColprintAddSubparsers (table,
											parser->slaveControlBlock);
		}
	}
	else
	{
		parser = (LanguageTable + language);
		subparserColprintAddSubparsers (table,
										parser->slaveControlBlock);
	}

	subparserColprintTablePrint (table,
								 withListHeader, machinable,
								 fp);
	colprintTableDelete (table);
}

extern void printLangdefFlags (bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable * table;

	table = flagsColprintTableNew ();

	flagsColprintAddDefinitions (table, PreLangDefFlagDef, ARRAY_SIZE (PreLangDefFlagDef));

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void printKinddefFlags (bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable * table;

	table = flagsColprintTableNew ();

	flagsColprintAddDefinitions (table, PreKindDefFlagDef, ARRAY_SIZE (PreKindDefFlagDef));

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void printFielddefFlags (bool withListHeader, bool machinable, FILE *fp)
{
	struct colprintTable * table;

	table = flagsColprintTableNew ();

	flagsColprintAddDefinitions (table, FieldDefFlagDef, ARRAY_SIZE (FieldDefFlagDef));

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void printLanguageMultitableStatistics (langType language)
{
	parserObject* const parser = LanguageTable + language;
	printMultitableStatistics (parser->lregexControlBlock);
}

extern void addLanguageRegexTable (const langType language, const char *name)
{
	parserObject* const parser = LanguageTable + language;
	addRegexTable (parser->lregexControlBlock, name);
}

extern void addLanguageTagMultiTableRegex(const langType language,
										  const char* const table_name,
										  const char* const regex,
										  const char* const name, const char* const kinds, const char* const flags,
										  bool *disabled)
{
	parserObject* const parser = LanguageTable + language;
	addTagMultiTableRegex (parser->lregexControlBlock, table_name, regex,
						   name, kinds, flags, disabled);
}

extern void addLanguageOptscriptToHook (langType language, enum scriptHook hook, const char *const src)
{
	addOptscriptToHook (LanguageTable [language].lregexControlBlock, hook, src);
}

static bool processHookOption (const char *const option, const char *const parameter, const char *prefix,
							   enum scriptHook hook)
{
	langType language = getLanguageComponentInOption (option, prefix);
	if (language == LANG_IGNORE)
		return false;

	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "A parameter is needed after \"%s\" option", option);

	const char * code = flagsEval (parameter, NULL, 0, NULL);
	if (code == NULL)
		error (FATAL, "Cannot recognized a code block surrounded by `{{' and `}}' after \"%s\" option", option);
	addLanguageOptscriptToHook (language, hook, code);

	return true;
}

extern bool processPreludeOption (const char *const option, const char *const parameter)
{
	return processHookOption (option, parameter, "_prelude-", SCRIPT_HOOK_PRELUDE);
}

extern bool processSequelOption (const char *const option, const char *const parameter)
{
	return processHookOption (option, parameter, "_sequel-", SCRIPT_HOOK_SEQUEL);
}

extern bool processMakeTagEntryReflectionOption (const char *const option, const char *const parameter)
{
	return processHookOption (option, parameter, "_makeTagEntryReflection-", SCRIPT_HOOK_MAKE_TAG_ENTRY_REFLECTION);
}

extern bool processPretendOption (const char *const option, const char *const parameter)
{
	langType new_language, old_language;

#define pretendOptionPrefix "_pretend-"
	new_language = getLanguageComponentInOptionFull (option, pretendOptionPrefix, true);
	if (new_language == LANG_IGNORE)
		return false;

	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "A parameter is needed after \"%s\" option", option);

	old_language = getNamedLanguageFull (parameter, 0, true, false);
	if (old_language == LANG_IGNORE)
		error (FATAL, "Unknown language \"%s\" in option \"--%s=%s\"",
			   parameter, option, parameter);

	if (LanguageTable [new_language].pretendingAsLanguage != LANG_IGNORE)
	{
		error (FATAL, "%s parser pretends as %s already\n",
			   getLanguageNameFull (new_language, true),
			   getLanguageNameFull (LanguageTable [new_language].pretendingAsLanguage, true));
	}
	if (LanguageTable [old_language].pretendedAsLanguage != LANG_IGNORE)
	{
		error (FATAL, "%s parser is pretended as %s already\n",
			   getLanguageNameFull (old_language, true),
			   getLanguageNameFull (LanguageTable [old_language].pretendedAsLanguage, true));
	}

	verbose ("%s pretends %s\n",
			 getLanguageNameFull (new_language, true),
			 getLanguageNameFull (old_language, true));

	LanguageTable [new_language].pretendingAsLanguage = old_language;
	LanguageTable [old_language].pretendedAsLanguage  = new_language;

	verbose ("force enabling %s\n",
			 getLanguageNameFull (new_language, true));
	enableLanguage (new_language, true);

	verbose ("force disabling %s\n",
			 getLanguageNameFull (old_language, true));
	enableLanguage (old_language, false);

	return true;
}

extern unsigned int getLanguageCorkUsage (langType lang)
{
	parserObject* const parser = LanguageTable + lang;
	return parserCorkFlags (parser->def);
}

/*
 * The universal fallback parser.
 * If any parser doesn't handle the input, this parser is
 * used for the input when --languages=+Unknown is given.
 * writer-etags enables this parser implicitly.
 */
static parserDefinition *FallbackParser (void)
{
	parserDefinition *const def = parserNew ("Unknown");
	def->extensions = NULL;
	def->kindTable = NULL;
	def->kindCount = 0;

	/* A user can extend this parser with --regex-Unknown=...
	 * or --langdef=MyParser{base=Unknown}.
	 *
	 * TODO: if following conditions are met, dontFindTags()
	 * defined below can be used.
	 * - any regex pattern is not defined,
	 * - any sub parser is not defined, and
	 * - end: field is not enabled.
	 */
	def->parser = findRegexTags;
	def->enabled = 0;
	def->method = METHOD_REGEX;
	return def;
}

/*
 * A dummy parser for printing pseudo tags in xref output
 */
static void dontFindTags (void)
{
}

static kindDefinition CtagsKinds[] = {
	{true, 'p', "ptag", "pseudo tags"},
};

static parserDefinition *CTagsParser (void)
{
	parserDefinition *const def = parserNew ("UniversalCtags");
	def->extensions = NULL;
	def->kindTable = CtagsKinds;
	def->kindCount = ARRAY_SIZE(CtagsKinds);
	def->parser = dontFindTags;
	def->invisible = true;
	return def;
}

/*
 * A parser for CTagsSelfTest (CTST)
 */
#define SELF_TEST_PARSER "CTagsSelfTest"
#if defined(DEBUG) && defined(HAVE_SECCOMP)
extern void getppid(void);
#endif

static bool CTST_GatherStats;
static int CTST_num_handled_char;

typedef enum {
	K_BROKEN,
	K_NO_LETTER,
	K_NO_LONG_NAME,
	K_NOTHING_SPECIAL,
	K_GUEST_BEGINNING,
	K_GUEST_END,
#if defined(DEBUG) && defined(HAVE_SECCOMP)
	K_CALL_GETPPID,
#endif
	K_QUIT,
	K_DISABLED,
	K_ENABLED,
	K_ROLES,
	K_ROLES_DISABLED,
	K_FIELD_TESTING,
	K_TRIGGER_NOTICE,
	K_EMIT_NULL_TAG,
	K_DONT_EMIT_NULL_TAG,
	KIND_COUNT
} CTST_Kind;

typedef enum {
	R_BROKEN_REF,
} CTST_BrokenRole;

static roleDefinition CTST_BrokenRoles [] = {
	{true, "broken", "broken" },
};

typedef enum {
	R_DISABLED_KIND_DISABLED_ROLE,
	R_DISABLED_KIND_ENABLED_ROLE,
} CTST_DisabledKindRole;

static roleDefinition CTST_DisabledKindRoles [] = {
	{ false, "disabled", "disabled role attached to disabled kind" },
	{ true,  "enabled",  "enabled role attached to disabled kind"  },
};

typedef enum {
	R_ENABLED_KIND_DISABLED_ROLE,
	R_ENABLED_KIND_ENABLED_ROLE,
} CTST_EnabledKindRole;

static roleDefinition CTST_EnabledKindRoles [] = {
	{ false, "disabled", "disabled role attached to enabled kind" },
	{ true,  "enabled",  "enabled role attached to enabled kind"  },
};

typedef enum {
	R_ROLES_KIND_A_ROLE,
	R_ROLES_KIND_B_ROLE,
	R_ROLES_KIND_C_ROLE,
	R_ROLES_KIND_D_ROLE,
} CTST_RolesKindRole;

static roleDefinition CTST_RolesKindRoles [] = {
	{ true,  "a", "A role" },
	{ true,  "b", "B role" },
	{ false, "c", "C role" },
	{ true,  "d", "D role"  },
};

typedef enum {
	R_ROLES_DISABLED_KIND_A_ROLE,
	R_ROLES_DISABLED_KIND_B_ROLE,
} CTST_RolesDisableKindRole;


static roleDefinition CTST_RolesDisabledKindRoles [] = {
	{ true,  "A", "A role" },
	{ true,  "B", "B role" },
};

static kindDefinition CTST_Kinds[KIND_COUNT] = {
	/* `a' is reserved for kinddef testing */
	{true, 'b', "broken tag", "name with unwanted characters",
	 .referenceOnly = false, ATTACH_ROLES (CTST_BrokenRoles) },
	{true, KIND_NULL_LETTER, "no letter", "kind with no letter"
	 /* use '@' when testing. */
	},
	{true, 'L', NULL, "kind with no long name" },
	{true, 'N', "nothingSpecial", "emit a normal tag" },
	{true, 'B', NULL, "beginning of an area for a guest" },
	{true, 'E', NULL, "end of an area for a guest" },
#if defined(DEBUG) && defined(HAVE_SECCOMP)
	{true, 'P', "callGetPPid", "trigger calling getppid(2) that seccomp sandbox disallows"},
#endif
	{true, 'Q', "quit", "stop the parsing"},
	{false,'d', "disabled", "a kind disabled by default",
	 .referenceOnly = false, ATTACH_ROLES (CTST_DisabledKindRoles)},
	{true, 'e', "enabled", "a kind enabled by default",
	 .referenceOnly = false, ATTACH_ROLES (CTST_EnabledKindRoles)},
	{true, 'r', "roles", "emit a tag with multi roles",
	 .referenceOnly = true, ATTACH_ROLES (CTST_RolesKindRoles)},
	{false, 'R', "rolesDisabled", "emit a tag with multi roles(disabled by default)",
	 .referenceOnly = true, ATTACH_ROLES (CTST_RolesDisabledKindRoles)},
	{true,  'f', "fieldMaker", "tag for testing field:" },
	{true,  'n', "triggerNotice", "trigger notice output"},
	{true,  'z', "emitNullTag", "emit a tag having an empty string"},
	{true,  'Z', "dontEmitNullTag", "don't emit a tag having an empty string"},
};

typedef enum {
	F_BOOLEAN_FIELD,
	F_BOOLEAN_AND_STRING_FIELD,
	F_STRING_FIELD,
	F_INTEGER_FIELD,
	COUNT_FIELD
} CTSTField;

static fieldDefinition CTSTFields[COUNT_FIELD] = {
	{ .name = "bField",
	  .description = "field for testing boolean type",
	  .dataType = FIELDTYPE_BOOL,
	  .enabled = true,
	},
	{ .name = "sbField",
	  .description = "field for testing string|boolean type",
	  .dataType = FIELDTYPE_STRING|FIELDTYPE_BOOL,
	  .enabled = true,
	},
	{ .name = "sField",
	  .description = "field for testing string type",
	  .dataType = FIELDTYPE_STRING,
	  .enabled = true,
	},
	{ .name = "iField",
	  .description = "field for testing integer type",
	  .dataType = FIELDTYPE_INTEGER,
	  .enabled = true,
	},
};

static void createCTSTTags (void)
{
	int i;
	const unsigned char *line;
	tagEntryInfo e;

	unsigned long lb = 0;
	unsigned long le = 0;

	int found_enabled_disabled[2] = {0, 0};

	bool quit = false;

	TRACE_ENTER_TEXT("Parsing starts");

	while (!quit && (line = readLineFromInputFile ()) != NULL)
	{
		int c = line[0];

		for (i = 0; i < KIND_COUNT; i++)
			if ((c == CTST_Kinds[i].letter && i != K_NO_LETTER)
				|| (c == '@' && i == K_NO_LETTER))
			{
				if (CTST_GatherStats)
					CTST_num_handled_char++;

				switch (i)
				{
					case K_BROKEN:
						initTagEntry (&e, "one\nof\rbroken\tname", i);
						e.extensionFields.scopeKindIndex = K_BROKEN;
						e.extensionFields.scopeName = "\\Broken\tContext";
						makeTagEntry (&e);
						initTagEntry (&e, "only\nnewline", i);
						makeTagEntry (&e);
						initTagEntry (&e, "only\ttab", i);
						makeTagEntry (&e);
						initTagEntry (&e, "newline-in-scope", i);
						e.extensionFields.scopeKindIndex = K_BROKEN;
						e.extensionFields.scopeName = "parent\nscope";
						makeTagEntry (&e);
						initTagEntry (&e, "tab-in-scope", i);
						e.extensionFields.scopeKindIndex = K_BROKEN;
						e.extensionFields.scopeName = "parent\tscope";
						makeTagEntry (&e);
						break;
					case K_NO_LETTER:
						initTagEntry (&e, "abnormal kindDefinition testing (no letter)", i);
						makeTagEntry (&e);
						break;
					case K_NO_LONG_NAME:
						initTagEntry (&e, "abnormal kindDefinition testing (no long name)", i);
						makeTagEntry (&e);
						break;
					case K_NOTHING_SPECIAL:
						if (!lb)
						{
							initTagEntry (&e, "NOTHING_SPECIAL", i);
							makeTagEntry (&e);
						}
						break;
					case K_GUEST_BEGINNING:
						lb = getInputLineNumber ();
						break;
					case K_GUEST_END:
						le = getInputLineNumber ();
						makePromise (SELF_TEST_PARSER, lb + 1, 0, le, 0, lb + 1);
						break;
#if defined(DEBUG) && defined(HAVE_SECCOMP)
				    case K_CALL_GETPPID:
						getppid();
						break;
#endif
					case K_QUIT:
						quit = true;
						break;
				    case K_DISABLED:
				    case K_ENABLED:
						{
							int role;
							char *name;
							if (found_enabled_disabled[i == K_DISABLED]++ == 0)
							{
								role = ROLE_DEFINITION_INDEX;
								name = (i == K_DISABLED)
									? "disable-kind-no-role"
									: "enabled-kind-no-role";
							}
							else if (found_enabled_disabled[i == K_DISABLED]++ == 1)
							{
								role = (i == K_DISABLED)
									? R_DISABLED_KIND_DISABLED_ROLE
									: R_ENABLED_KIND_DISABLED_ROLE;
								name = (i == K_DISABLED)
									? "disable-kind-disabled-role"
									: "enabled-kind-disabled-role";
							}
							else
							{
								role = (i == K_DISABLED)
									? R_DISABLED_KIND_ENABLED_ROLE
									: R_ENABLED_KIND_ENABLED_ROLE;
								name = (i == K_DISABLED)
									? "disable-kind-enabled-role"
									: "enabled-kind-enabled-role";
							}
							initRefTagEntry (&e, name, i, role);
							makeTagEntry (&e);
							break;
						}
					case K_ROLES:
					{
						char *name = "multiRolesTarget";
						int qindex;
						tagEntryInfo *qe;

						initTagEntry (&e, name, i);
						assignRole(&e, R_ROLES_KIND_A_ROLE);
						assignRole(&e, R_ROLES_KIND_C_ROLE);
						assignRole(&e, R_ROLES_KIND_D_ROLE);
						qindex = makeTagEntry (&e);
						qe = getEntryInCorkQueue (qindex);
						if (qe)
							assignRole(qe, R_ROLES_KIND_B_ROLE);
						break;
					}
					case K_ROLES_DISABLED:
					{
						char *name = "multiRolesDisabledTarget";

						initRefTagEntry (&e, name, i, R_ROLES_DISABLED_KIND_A_ROLE);
						makeTagEntry (&e);
						initRefTagEntry (&e, name, i, R_ROLES_DISABLED_KIND_B_ROLE);
						makeTagEntry (&e);
						break;
					}
					case K_FIELD_TESTING:
					{
						char c = 'a';
						char name []= {'\0', 't', 'a', 'g', '\0' };

						name [0] = c++;
						initTagEntry (&e, name, i);
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_BOOLEAN_FIELD].ftype, "");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_BOOLEAN_FIELD].ftype, "any-C-string-is-interpreted-as-true");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_BOOLEAN_AND_STRING_FIELD].ftype, "val");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_BOOLEAN_AND_STRING_FIELD].ftype, "");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_STRING_FIELD].ftype, "val");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_STRING_FIELD].ftype, "");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_INTEGER_FIELD].ftype, "23");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_INTEGER_FIELD].ftype, "-3");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_INTEGER_FIELD].ftype, "poison");
						makeTagEntry (&e);

						name [0] = c++;
						initTagEntry (&e, name, i);
						attachParserField (&e,
										   CTSTFields[F_INTEGER_FIELD].ftype, "");
						makeTagEntry (&e);

						break;
					}
					case K_TRIGGER_NOTICE:
						notice ("notice output for testing: %s", CTST_Kinds [i].name);
						break;
					case K_EMIT_NULL_TAG:
						initTagEntry (&e, "", i);
						e.allowNullTag = 1;
						makeTagEntry (&e);
						break;
					case K_DONT_EMIT_NULL_TAG:
						initTagEntry (&e, "", i);
						e.allowNullTag = 0;
						makeTagEntry (&e);
						break;
				}

				if (quit)
					break;
			}
	}

	TRACE_LEAVE();
}

static void initStatsCTST (langType lang CTAGS_ATTR_UNUSED)
{
	CTST_GatherStats = true;
}

static void printStatsCTST (langType lang CTAGS_ATTR_UNUSED)
{
	fprintf (stderr, "The number of handled chars: %d\n",
			 CTST_num_handled_char);
}

static void initCTST (langType language)
{
	ctagsSelfTestLang = language;
}

static parserDefinition *CTagsSelfTestParser (void)
{
	static const char *const extensions[] = { NULL };
	parserDefinition *const def = parserNew (SELF_TEST_PARSER);
	def->extensions = extensions;
	def->kindTable = CTST_Kinds;
	def->kindCount = KIND_COUNT;
	def->parser = createCTSTTags;
	def->initialize = initCTST;
	def->invisible = true;
	def->useMemoryStreamInput = true;
	def->useCork = CORK_QUEUE;
	def->initStats = initStatsCTST;
	def->printStats = printStatsCTST;
	def->fieldTable = CTSTFields;
	def->fieldCount = ARRAY_SIZE (CTSTFields);

	return def;
}
