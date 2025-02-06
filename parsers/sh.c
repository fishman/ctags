/*
*   Copyright (c) 2000-2002, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for generating tags for scripts for the
*   Bourne shell (and its derivatives, the Korn and Z shells).
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include <string.h>

#include "debug.h"
#include "entry.h"
#include "keyword.h"
#include "kind.h"
#include "parse.h"
#include "read.h"
#include "promise.h"
#include "routines.h"
#include "vstring.h"
#include "xtag.h"

#include "x-sh.h"

/*
*   DATA DEFINITIONS
*/
typedef enum {
	K_SUBPARSER = -2,
	K_NOTHING = -1,		/* place holder. Never appears on tags file. */
	K_ALIAS,
	K_FUNCTION,
	K_SCRIPT,
	K_HEREDOCLABEL,
	K_VARIABLE,
} shKind;

typedef enum {
	R_SCRIPT_LOADED,
	LAST_R_SCRIPT,
} shScriptRole;

typedef enum {
	R_ZSH_SCRIPT_AUTOLOADED = LAST_R_SCRIPT,
} zshScriptRole;

#define SH_SCRIPT_ROLES_COMMON 	{ true, "loaded", "loaded" }
static roleDefinition ShScriptRoles [] = {
	SH_SCRIPT_ROLES_COMMON,
};

static roleDefinition ZshScriptRoles [] = {
	SH_SCRIPT_ROLES_COMMON,
	{ true, "autoloaded", "autoloaded" },
};

typedef enum {
	R_HEREDOC_ENDMARKER,
} shHeredocRole;

#define SH_HEREDOC_ROLES_COMMON { true, "endmarker", "end marker" }
static roleDefinition ShHeredocRoles [] = {
	SH_HEREDOC_ROLES_COMMON,
};

static roleDefinition ZshHeredocRoles [] = {
	SH_HEREDOC_ROLES_COMMON,
};

typedef enum {
	R_ZSH_FUNCTION_AUTOLOADED,
} zshFunctionRole;

static roleDefinition ZshFunctionRoles [] = {
	{ true, "autoloaded", "function name passed to autoload built-in command" },
};

#define SH_KINDS_COMMON(SCRIPT_ROLES,HEREDOC_ROLES, FUNCTION_ROLES_SPEC) \
	{ true, 'a', "alias", "aliases"},							\
	{ true, 'f', "function", "functions",						\
	  .referenceOnly = false, FUNCTION_ROLES_SPEC },			\
	{ true, 's', "script", "script files",						\
	  .referenceOnly = true, ATTACH_ROLES (SCRIPT_ROLES) },		\
	{ true, 'h', "heredoc", "labels for here document",			\
	  .referenceOnly = false, ATTACH_ROLES (HEREDOC_ROLES) },   \
	{ true, 'v', "variable", "variables assigment (experimental)" }


static kindDefinition ShKinds [] = {
	SH_KINDS_COMMON(ShScriptRoles, ShHeredocRoles,),
};

static kindDefinition ZshKinds [] = {
	SH_KINDS_COMMON(ZshScriptRoles, ZshHeredocRoles,
					ATTACH_ROLES(ZshFunctionRoles)),
};

enum eShKeywordId {
	KEYWORD_function,
	KEYWORD_alias,
	KEYWORD_source,
	KEYWORD_autoload,
};

const char *dialectMap [] = {
	"Sh", "Zsh",
};

static const struct dialectalKeyword KeywordTable [] = {
	{ "function",  KEYWORD_function, { 1, 1 } },
	{ "alias",     KEYWORD_alias,    { 1, 1 } },
	{ "source",    KEYWORD_source,   { 1, 1 } },
	{ "autoload",  KEYWORD_autoload, { 0, 1 } },
};

/*
* FUNCTION DECLARATIONS
*/
static subparser *notifyLineToSubparsers (const unsigned char *cp,
										  int *n);
static int extractNameToSubparser (subparser *sub, const unsigned char *cp,
								   vString *name);
static int makeTagInSubparser (subparser *sub, vString *name);

/*
*   FUNCTION DEFINITIONS
*/

static bool isFileChar  (int c)
{
	return (isalnum (c)
		|| c == '_' || c == '-'
		|| c == '/' || c == '.'
		|| c == '+' || c == '^'
		|| c == '%' || c == '@'
		|| c == '~');
}

static bool isIdentChar0 (int c)
{
	return (isalpha (c) || c == '_' || c == '-');
}

static bool isIdentChar (int c)
{
	return (isalnum (c) || c == '_' || c == '-');
}

/* bash allows all kinds of crazy stuff as the identifier after 'function' */
static bool isBashFunctionChar (int c)
{
	return (c > 1 /* NUL and SOH are disallowed */ && c != 0x7f &&
	        /* blanks are disallowed, but VT and FF (and CR to some extent, but
	         * let's not fall into the pit of craziness) */
	        c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
	        c != '"' && c != '\'' && c != '$' && c != '`' && c != '\\' &&
	        c != '&' && c != ';' &&
	        c != '(' && c != ')' &&
	        c != '<' && c != '>');
}

static const unsigned char *skipDoubleString (const unsigned char *cp)
{
	const unsigned char* prev = cp;
	cp++;
	while ((*cp != '"' || *prev == '\\') && *cp != '\0')
	{
		prev = cp;
		cp++;
	}
	return cp;
}

static const unsigned char *skipSingleString (const unsigned char *cp)
{
	cp++;
	while (*cp != '\'' && *cp != '\0')
		cp++;
	return cp;
}

static bool isEnvCommand (const vString *cmd)
{
	const char *lc = vStringValue(cmd);
	const char * tmp = baseFilename (lc);

	return (strcmp(tmp, "env") == 0);
}

static int readDestfileName (const unsigned char *cp, vString *destfile)
{
	const unsigned char *origin = cp;

	while (isspace (*cp))
		++cp;

	/* >... */
	if (*cp != '>')
		return 0;

	/* >>... */
	if (*cp == '>')
		++cp;

	while (isspace (*cp))
		++cp;

	if (!isFileChar (*cp))
		return 0;

	vStringClear(destfile);
	do {
		vStringPut (destfile, *cp);
		++cp;
	} while (isFileChar (*cp));

	if (vStringLength(destfile) > 0)
		return cp - origin;

	return 0;
}

struct hereDocParsingState {
	vString *args[2];
	vString *destfile;
	langType sublang;
	unsigned long startLine;

	int corkIndex;
};

static void hdocStateInit (struct hereDocParsingState *hstate)
{
	hstate->args[0] = vStringNew ();
	hstate->args[1] = vStringNew ();
	hstate->destfile = vStringNew ();

	hstate->corkIndex = CORK_NIL;
	hstate->sublang = LANG_IGNORE;
}

static void hdocStateClear (struct hereDocParsingState *hstate)
{
	vStringClear (hstate->args[0]);
	vStringClear (hstate->args[1]);
	vStringClear (hstate->destfile);
}

static void hdocStateFini (struct hereDocParsingState *hstate)
{
	vStringDelete (hstate->args[0]);
	vStringDelete (hstate->args[1]);
	vStringDelete (hstate->destfile);
}

static void hdocStateUpdateArgs (struct hereDocParsingState *hstate,
										   vString *name)
{
	if (vStringIsEmpty(hstate->args[0]))
		vStringCopy(hstate->args[0], name);
	else if (vStringIsEmpty(hstate->args[1]))
		vStringCopy(hstate->args[1], name);
}

static void hdocStateMakePromiseMaybe (struct hereDocParsingState *hstate)
{
	if (hstate->sublang != LANG_IGNORE)
		makePromise (getLanguageName(hstate->sublang),
					 hstate->startLine, 0,
					 getInputLineNumber(), 0,
					 0);
	hstate->sublang = LANG_IGNORE;
}

static void hdocStateRecordStartlineFromDestfileMaybe (struct hereDocParsingState *hstate)
{
	const char *f = vStringValue(hstate->destfile);

	if (hstate->sublang != LANG_IGNORE)
		return;

	hstate->sublang = getLanguageForFilename (f, 0);
	if (hstate->sublang != LANG_IGNORE)
		hstate->startLine = getInputLineNumber () + 1;
	vStringClear (hstate->destfile);
}

static void hdocStateRecordStatelineMaybe (struct hereDocParsingState *hstate)
{
	if (!vStringIsEmpty(hstate->args[0]))
	{
		const char *cmd;

		cmd = vStringValue(hstate->args[0]);
		if (isEnvCommand (hstate->args[0]))
		{
			cmd = NULL;
			if (!vStringIsEmpty(hstate->args[1]))
				cmd = vStringValue(hstate->args[1]);
		}

		if (cmd)
		{
			hstate->sublang = getLanguageForCommand (cmd, 0);
			if (hstate->sublang != LANG_IGNORE)
				hstate->startLine = getInputLineNumber () + 1;
		}
	}

	if (vStringLength(hstate->destfile) > 0)
		hdocStateRecordStartlineFromDestfileMaybe (hstate);
}

static int hdocStateReadDestfileName (struct hereDocParsingState *hstate,
									  const unsigned char* cp,
									  const vString *const hereDocDelimiter)
{
	int d = readDestfileName (cp, hstate->destfile);

	if (d > 0 && hereDocDelimiter)
		hdocStateRecordStartlineFromDestfileMaybe (hstate);

	return d;
}

static void hdocStateUpdateTag (struct hereDocParsingState *hstate, unsigned long endLine)
{
	setTagEndLineToCorkEntry (hstate->corkIndex, endLine);
	hstate->corkIndex = CORK_NIL;
}

static size_t handleShKeyword (int keyword,
							   vString *token,
							   int *kind,
							   int *role,
							   bool (** check_char)(int))
{
	switch (keyword)
	{
	case KEYWORD_function:
		*kind = K_FUNCTION;
		break;
	case KEYWORD_alias:
		*kind = K_ALIAS;
		*check_char = isIdentChar;
		break;
	case KEYWORD_source:
		*kind = K_SCRIPT;
		*role = R_SCRIPT_LOADED;
		*check_char = isFileChar;
		break;
	default:
		AssertNotReached();
		break;
	}

	return vStringLength(token);
}

static int makeShAliasTag(vString *name, const unsigned char ** cp, const char *options)
{
	const unsigned char *p = *cp;
	int r = CORK_NIL;

	const char *opt = vStringValue (name);
	if (opt[0] == '-')
	{
		if  (strpbrk(opt, options))
			return CORK_NIL;

		vStringClear(name);

		while (isspace (*p))
			p++;

		while (*p && isIdentChar (*p))
			vStringPut (name, *p++);
	}

	if (!vStringIsEmpty(name) && *p == '=')
		r = makeSimpleTag (name, K_ALIAS);
	*cp = p;
	return r;
}

static int makeShTag (vString *name, const unsigned char ** cp,
					  int found_kind, int found_role)
{
	if (found_kind == K_ALIAS && found_role == ROLE_DEFINITION_INDEX)
		/* -p is for printing defined aliases in bash. */
		return makeShAliasTag (name, cp, "p");

	return makeSimpleRefTag (name, found_kind, found_role);
}

static int makeZshAutoloadTag(vString *name, const unsigned char ** cp)
{
	const unsigned char *p = *cp;

	int r = CORK_NIL;
	do
	{
		if (vStringChar(name, 0) != '-' && vStringChar(name, 0) != '+')
		{
			do
			{
				r = makeSimpleRefTag (name, K_SCRIPT, R_ZSH_SCRIPT_AUTOLOADED);

				tagEntryInfo e;
				const char *func = strrchr(vStringValue (name), '/');
				func = func? func + 1: vStringValue (name);
				if (*func != '\0')
				{
					initRefTagEntry (&e, func, K_FUNCTION, R_ZSH_FUNCTION_AUTOLOADED);
					r = makeTagEntry(&e);
				}

				while (isspace (*p))
					p++;

				if (*p == '\0')
					break;

				vStringClear (name);
				while (*p != '\0' && !isspace (*p))
					vStringPut (name, *p++);
			}
			while (1);
			break;
		}

		if (*p == '\0')
			break;

		vStringClear(name);
		while (*p != '\0' && !isspace(*p))
			vStringPut (name, *p++);

		while (isspace (*p))
			++p;
	}
	while (1);

	*cp = p;
	return r;
}

static int makeZshFunctionTag(vString *name, const unsigned char ** cp)
{
	const unsigned char *p = *cp;

	int r = CORK_NIL;

	if (strcmp(vStringValue(name), "-T") == 0)
	{
		vStringClear(name);

		while (isspace (*p))
			p++;

		while (isBashFunctionChar (*p))
		{
			vStringPut (name, *p);
			++p;
		}
	}

	if (!vStringIsEmpty(name))
		r = makeSimpleTag (name, K_FUNCTION);
	*cp = p;
	return r;
}

static int makeZshTag (vString *name, const unsigned char ** cp,
					  int found_kind, int found_role)
{
	if (found_kind == K_SCRIPT && found_role == R_ZSH_SCRIPT_AUTOLOADED)
		return makeZshAutoloadTag(name, cp);

	if (found_kind == K_FUNCTION && found_role == ROLE_DEFINITION_INDEX)
		return makeZshFunctionTag(name, cp);

	if (found_kind == K_ALIAS && found_role == ROLE_DEFINITION_INDEX)
		/* -m, -r, and -L are for printing defined aliases. */
		return makeShAliasTag (name, cp, "mrL");

	return makeShTag (name, cp, found_kind, found_role);
}

static size_t handleZshKeyword (int keyword,
								vString *token,
								int *kind,
								int *role,
								bool (** check_char)(int))
{
	switch (keyword)
	{
	case KEYWORD_autoload:
		*kind = K_SCRIPT;
		*role = R_ZSH_SCRIPT_AUTOLOADED;
		break;
	default:
		return handleShKeyword (keyword, token, kind, role, check_char);
	}

	return vStringLength(token);
}

static bool doesLineCotinue(const unsigned char *start, const unsigned char *cp)
{
	cp--;
	if (start >= cp)
		return false;

	if (*cp != '\\')
		return false;

	while (start < cp)
	{
		if (*cp == ';' || *cp == '|' || *cp == '&')
			return false;
		else if (isspace(*cp))
			cp--;
		else
			return true;
	}
	return false;
}

static bool handleVariableAssignment (vString *input)
{
	const char *base = vStringValue (input);
	const char *cp = base;

	while (*cp != '\0')
	{
		if (*cp == '=')
		{
			size_t len = cp - base;
			if (len > 0)
			{
				vStringTruncate (input, len);
				return true;
			}
			break;
		}
		else if ( ((cp == base)?
				   isIdentChar0: isIdentChar) ((unsigned char)*cp) )
			cp++;
		else
			break;
	}
	return false;
}

typedef bool (* checkCharFunc) (int);
static void findShTagsCommon (size_t (* keyword_handler) (int,
														  vString *,
														  int *,
														  int *,
														  checkCharFunc *check_char),
							  int (* make_tag_handler) (vString *, const unsigned char **, int, int))

{
	vString *name = vStringNew ();
	const unsigned char *line;
	vString *hereDocDelimiter = NULL;
	bool hereDocIndented = false;
	checkCharFunc check_char;

	struct hereDocParsingState hstate;
	hdocStateInit (&hstate);

	bool cont_line = false;
	while ((line = readLineFromInputFile ()) != NULL)
	{
		const unsigned char* cp = line;
		int found_kind = K_NOTHING;
		int found_role = ROLE_DEFINITION_INDEX;

		if (hereDocDelimiter)
		{
			if (hereDocIndented)
			{
				while (*cp == '\t')
					cp++;
			}
			if ((strncmp ((const char *) cp, vStringValue (hereDocDelimiter), vStringLength (hereDocDelimiter)) == 0)
				&& ((*(cp + vStringLength (hereDocDelimiter)) == '\0')
					|| isspace (*(cp + vStringLength (hereDocDelimiter)) )))
			{
				hdocStateUpdateTag (&hstate, getInputLineNumber ());
				hdocStateMakePromiseMaybe (&hstate);

				if (!vStringIsEmpty(hereDocDelimiter))
					makeSimpleRefTag(hereDocDelimiter, K_HEREDOCLABEL, R_HEREDOC_ENDMARKER);
				vStringDelete (hereDocDelimiter);
				hereDocDelimiter = NULL;
			}
			cont_line = false;
			continue;
		}

		hdocStateClear (&hstate);
		bool beginning_of_line = !cont_line;
		while (*cp != '\0')
		{
			subparser *sub = NULL;
			int sub_n = 0;

			/* jump over whitespace */
			while (isspace (*cp))
				cp++;

			/* jump over strings */
			if (*cp == '"')
				cp = skipDoubleString (cp);
			else if (*cp == '\'')
				cp = skipSingleString (cp);
			/* jump over comments */
			else if (*cp == '#')
				break;
			/* jump over here-documents */
			else if (cp[0] == '<' && cp[1] == '<')
			{
				const unsigned char *start, *end;
				bool trimEscapeSequences = false;
				bool quoted = false;
				cp += 2;
				/* an optional "-" strips leading tabulations from the heredoc lines */
				if (*cp != '-')
					hereDocIndented = false;
				else
				{
					hereDocIndented = true;
					cp++;
				}
				while (isspace (*cp))
					cp++;
				start = end = cp;
				/* the delimiter can be surrounded by quotes */
				if (*cp == '"')
				{
					start++;
					end = cp = skipDoubleString (cp);
					/* we need not to worry about variable substitution, they
					 * don't happen in heredoc delimiter definition */
					trimEscapeSequences = true;
					quoted = true;
				}
				else if (*cp == '\'')
				{
					start++;
					end = cp = skipSingleString (cp);
					quoted = true;
				}
				else
				{
					while (isIdentChar (*cp))
						cp++;
					end = cp;
				}
				if (end > start || quoted)
				{
					/* The input may be broken as a shell script but we need to avoid
					   memory leaking. */
					if (hereDocDelimiter)
						vStringClear(hereDocDelimiter);
					else
						hereDocDelimiter = vStringNew ();
					for (; end > start; start++)
					{
						if (trimEscapeSequences && *start == '\\')
							start++;
						vStringPut (hereDocDelimiter, *start);
					}
					if (vStringLength(hereDocDelimiter) > 0)
						hstate.corkIndex = makeSimpleTag(hereDocDelimiter, K_HEREDOCLABEL);

					hdocStateRecordStatelineMaybe(&hstate);
				}
			}

			check_char = isBashFunctionChar;

			if (cp [0] == '.'
				    && isspace(cp [1]))
			{
				found_kind = K_SCRIPT;
				found_role = R_SCRIPT_LOADED;
				++cp;
				check_char = isFileChar;
			}
			else if ((sub = notifyLineToSubparsers (cp, &sub_n)))
			{
				found_kind = K_SUBPARSER;
				cp += sub_n;
			}
			else
			{
				vString *token = vStringNew ();
				const unsigned char *tmp = cp;
				while (*tmp && (tmp == cp
								? (isIdentChar0 (*tmp))
								: (isIdentChar (*tmp))))
				{
					vStringPut (token, *tmp);
					tmp++;
				}

				int keyword = lookupKeyword (vStringValue (token),
											 getInputLanguage ());
				if (keyword != KEYWORD_NONE)
					cp += (* keyword_handler) (keyword, token,
											   &found_kind, &found_role, &check_char);
				vStringDelete (token);
			}

			if (found_kind != K_NOTHING)
				while (isspace (*cp))
					++cp;

			if (found_kind == K_SUBPARSER)
			{
				sub_n = extractNameToSubparser (sub, cp, name);
				if (!vStringIsEmpty (name))
					cp += sub_n;
			}
			else
			{
				// Get the name of the function, alias or file to be read by source
				if (! check_char (*cp))
				{
					found_kind = K_NOTHING;
					found_role = ROLE_DEFINITION_INDEX;

					int d = hdocStateReadDestfileName (&hstate, cp,
													   hereDocDelimiter);
					if (d > 0)
						cp += d;
					else if (*cp != '\0')
						++cp;
					beginning_of_line = false;
					continue;
				}

				while (check_char (*cp))
				{
					vStringPut (name, *cp);
					++cp;
				}
			}

			while (isspace (*cp))
				++cp;

			if (found_kind == K_SCRIPT)
				;				/* Do NOTHING */
			else if (*cp == '(')
			{
				++cp;
				while (isspace (*cp))
					++cp;
				if (*cp == ')')
				{
					/* A function definiton can look like an array initialization:
					 *
					 *   ... f=()
					 *
					 * We use followng heuristics to distinguish a function definition
					 * from an array initialization:
					 *
					 *   preceding "function" keyword: function f=()
					 *   followed by '{': f=() {
					 *
					 */
					if (! ((vStringLast(name) == '=')
						   /* Have we found "function" yet? */
						   && (found_kind != K_FUNCTION)
						   && (strchr ((char *)(cp + 1), '{') == NULL)))
						found_kind = K_FUNCTION;
					++cp;
				}
			}
			else if (beginning_of_line
					 && found_kind == K_NOTHING
					 && handleVariableAssignment (name))
				found_kind = K_VARIABLE;

			if (found_kind != K_NOTHING)
			{
				if (found_kind == K_SUBPARSER)
				{
					if (!vStringIsEmpty (name))
						makeTagInSubparser (sub, name);
				}
				else
					make_tag_handler (name, &cp, found_kind, found_role);
				found_kind = K_NOTHING;
				found_role = ROLE_DEFINITION_INDEX;
			}
			else if (!hereDocDelimiter)
				hdocStateUpdateArgs (&hstate, name);
			vStringClear (name);
			beginning_of_line = false;
		}
		if (*cp == '#')
			cont_line = false;
		cont_line = doesLineCotinue (line, cp);
	}
	hdocStateFini (&hstate);
	vStringDelete (name);
	if (hereDocDelimiter)
		vStringDelete (hereDocDelimiter);
}

static void findShTags (void)
{
	findShTagsCommon (handleShKeyword, makeShTag);
}

static void findZshTags (void)
{
	findShTagsCommon (handleZshKeyword, makeZshTag);
}

static subparser *notifyLineToSubparsers (const unsigned char *cp,
										  int *n)
{
	int r = 0;
	subparser *sub;

	foreachSubparser (sub, false)
	{
		shSubparser *shsub = (shSubparser *)sub;

		if(shsub->lineNotify)
		{
			enterSubparser(sub);
			r = shsub->lineNotify (shsub, cp);
			leaveSubparser();
			if (r > 0)
				break;
		}
	}

	if (r > 0)
	{
		*n = r;
		return sub;
	}
	return NULL;
}

static int extractNameToSubparser(subparser *sub, const unsigned char *cp,
								  vString *name)
{
	int n;
	shSubparser *shsub = (shSubparser *)sub;

	enterSubparser(sub);
	n = shsub->extractName(shsub, cp, name);
	leaveSubparser();

	return n;
}

static int makeTagInSubparser (subparser *sub, vString *name)
{
	int r;
	shSubparser *shsub = (shSubparser *)sub;

	enterSubparser(sub);
	r = shsub->makeTag(shsub, name);
	leaveSubparser();

	return r;
}

static void initializeSh (const langType language)
{
	addDialectalKeywords (KeywordTable, ARRAY_SIZE (KeywordTable),
						  language,
						  dialectMap, ARRAY_SIZE (dialectMap));
}

extern parserDefinition* ShParser (void)
{
	static const char *const extensions [] = {
		"sh", "SH", "bsh", "bash", "ksh", "ash", NULL
	};
	static const char *const aliases [] = {
		"sh", "bash", "ksh", "ash", "dash",
		/* major mode name in emacs */
		"shell-script",
		NULL
	};
	parserDefinition* def = parserNew ("Sh");
	def->kindTable      = ShKinds;
	def->kindCount  = ARRAY_SIZE (ShKinds);
	def->extensions = extensions;
	def->aliases = aliases;
	def->parser     = findShTags;
	def->initialize = initializeSh;
	def->useCork    = CORK_QUEUE;
	return def;
}

extern parserDefinition* ZshParser (void)
{
	static const char *const extensions [] = {
		"zsh", NULL
	};
	static const char *const aliases [] = {
		"zsh", NULL,
	};
	parserDefinition* def = parserNew ("Zsh");
	def->kindTable      = ZshKinds;
	def->kindCount  = ARRAY_SIZE (ZshKinds);
	def->extensions = extensions;
	def->aliases = aliases;
	def->parser     = findZshTags;
	def->initialize = initializeSh;
	def->useCork    = CORK_QUEUE;
	return def;
}
