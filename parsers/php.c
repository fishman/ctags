/*
*   Copyright (c) 2013, Colomban Wendling <ban@herbesfolles.org>
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains code for generating tags for the PHP scripting
*   language.
*
*   The language reference: http://php.net/manual/en/langref.php
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include <string.h>

#include "parse.h"
#include "read.h"
#include "vstring.h"
#include "keyword.h"
#include "entry.h"
#include "routines.h"
#include "debug.h"
#include "objpool.h"
#include "promise.h"
#include "trace.h"

#define isIdentChar(c) (isalnum (c) || (c) == '_' || (c) >= 0x80)
#define newToken() (objPoolGet (TokenPool))
#define deleteToken(t) (objPoolPut (TokenPool, (t)))

enum {
	KEYWORD_abstract,
	KEYWORD_and,
	KEYWORD_as,
	KEYWORD_break,
	KEYWORD_callable,
	KEYWORD_case,
	KEYWORD_catch,
	KEYWORD_class,
	KEYWORD_clone,
	KEYWORD_const,
	KEYWORD_continue,
	KEYWORD_declare,
	KEYWORD_define,
	KEYWORD_default,
	KEYWORD_do,
	KEYWORD_echo,
	KEYWORD_else,
	KEYWORD_elif,
	KEYWORD_enddeclare,
	KEYWORD_endfor,
	KEYWORD_endforeach,
	KEYWORD_endif,
	KEYWORD_endswitch,
	KEYWORD_endwhile,
	KEYWORD_extends,
	KEYWORD_final,
	KEYWORD_finally,
	KEYWORD_for,
	KEYWORD_foreach,
	KEYWORD_function,
	KEYWORD_global,
	KEYWORD_goto,
	KEYWORD_if,
	KEYWORD_implements,
	KEYWORD_include,
	KEYWORD_include_once,
	KEYWORD_instanceof,
	KEYWORD_insteadof,
	KEYWORD_interface,
	KEYWORD_namespace,
	KEYWORD_new,
	KEYWORD_or,
	KEYWORD_print,
	KEYWORD_private,
	KEYWORD_protected,
	KEYWORD_public,
	KEYWORD_require,
	KEYWORD_require_once,
	KEYWORD_return,
	KEYWORD_static,
	KEYWORD_switch,
	KEYWORD_throw,
	KEYWORD_trait,
	KEYWORD_try,
	KEYWORD_use,
	KEYWORD_var,
	KEYWORD_while,
	KEYWORD_xor,
	KEYWORD_yield
};
typedef int keywordId; /* to allow KEYWORD_NONE */

typedef enum {
	ACCESS_UNDEFINED,
	ACCESS_PRIVATE,
	ACCESS_PROTECTED,
	ACCESS_PUBLIC,
	COUNT_ACCESS
} accessType;

typedef enum {
	IMPL_UNDEFINED,
	IMPL_ABSTRACT,
	COUNT_IMPL
} implType;

typedef enum {
	K_CLASS,
	K_DEFINE,
	K_FUNCTION,
	K_INTERFACE,
	K_LOCAL_VARIABLE,
	K_NAMESPACE,
	K_TRAIT,
	K_VARIABLE,
	K_ALIAS,
	COUNT_KIND
} phpKind;

#define NAMESPACE_SEPARATOR "\\"
static scopeSeparator PhpGenericSeparators [] = {
	{ K_NAMESPACE        , NAMESPACE_SEPARATOR },
	{ KIND_WILDCARD_INDEX, "::" },
};

static kindDefinition PhpKinds[COUNT_KIND] = {
	{ true, 'c', "class",		"classes",
	  ATTACH_SEPARATORS(PhpGenericSeparators) },
	{ true, 'd', "define",		"constant definitions",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
	{ true, 'f', "function",	"functions",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
	{ true, 'i', "interface",	"interfaces",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
	{ false, 'l', "local",		"local variables",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
	{ true, 'n', "namespace",	"namespaces",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
	{ true, 't', "trait",		"traits",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
	{ true, 'v', "variable",	"variables",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
	{ true, 'a', "alias",		"aliases",
	  ATTACH_SEPARATORS(PhpGenericSeparators)},
};

static const keywordTable PhpKeywordTable[] = {
	/* keyword			keyword ID */
	{ "abstract",		KEYWORD_abstract		},
	{ "and",			KEYWORD_and				},
	{ "as",				KEYWORD_as				},
	{ "break",			KEYWORD_break			},
	{ "callable",		KEYWORD_callable		},
	{ "case",			KEYWORD_case			},
	{ "catch",			KEYWORD_catch			},
	{ "cfunction",		KEYWORD_function		}, /* nobody knows what the hell this is, but it seems to behave much like "function" so bind it to it */
	{ "class",			KEYWORD_class			},
	{ "clone",			KEYWORD_clone			},
	{ "const",			KEYWORD_const			},
	{ "continue",		KEYWORD_continue		},
	{ "declare",		KEYWORD_declare			},
	{ "define",			KEYWORD_define			}, /* this isn't really a keyword but we handle it so it's easier this way */
	{ "default",		KEYWORD_default			},
	{ "do",				KEYWORD_do				},
	{ "echo",			KEYWORD_echo			},
	{ "else",			KEYWORD_else			},
	{ "elseif",			KEYWORD_elif			},
	{ "enddeclare",		KEYWORD_enddeclare		},
	{ "endfor",			KEYWORD_endfor			},
	{ "endforeach",		KEYWORD_endforeach		},
	{ "endif",			KEYWORD_endif			},
	{ "endswitch",		KEYWORD_endswitch		},
	{ "endwhile",		KEYWORD_endwhile		},
	{ "extends",		KEYWORD_extends			},
	{ "final",			KEYWORD_final			},
	{ "finally",		KEYWORD_finally			},
	{ "for",			KEYWORD_for				},
	{ "foreach",		KEYWORD_foreach			},
	{ "function",		KEYWORD_function		},
	{ "global",			KEYWORD_global			},
	{ "goto",			KEYWORD_goto			},
	{ "if",				KEYWORD_if				},
	{ "implements",		KEYWORD_implements		},
	{ "include",		KEYWORD_include			},
	{ "include_once",	KEYWORD_include_once	},
	{ "instanceof",		KEYWORD_instanceof		},
	{ "insteadof",		KEYWORD_insteadof		},
	{ "interface",		KEYWORD_interface		},
	{ "namespace",		KEYWORD_namespace		},
	{ "new",			KEYWORD_new				},
	{ "or",				KEYWORD_or				},
	{ "print",			KEYWORD_print			},
	{ "private",		KEYWORD_private			},
	{ "protected",		KEYWORD_protected		},
	{ "public",			KEYWORD_public			},
	{ "require",		KEYWORD_require			},
	{ "require_once",	KEYWORD_require_once	},
	{ "return",			KEYWORD_return			},
	{ "static",			KEYWORD_static			},
	{ "switch",			KEYWORD_switch			},
	{ "throw",			KEYWORD_throw			},
	{ "trait",			KEYWORD_trait			},
	{ "try",			KEYWORD_try				},
	{ "use",			KEYWORD_use				},
	{ "var",			KEYWORD_var				},
	{ "while",			KEYWORD_while			},
	{ "xor",			KEYWORD_xor				},
	{ "yield",			KEYWORD_yield			}
};


typedef enum eTokenType {
	TOKEN_UNDEFINED,
	TOKEN_EOF,
	TOKEN_CHARACTER,
	TOKEN_CLOSE_PAREN,
	TOKEN_SEMICOLON,
	TOKEN_COLON,
	TOKEN_COMMA,
	TOKEN_KEYWORD,
	TOKEN_OPEN_PAREN,
	TOKEN_OPERATOR,
	TOKEN_IDENTIFIER,
	TOKEN_STRING,
	TOKEN_PERIOD,
	TOKEN_OPEN_CURLY,
	TOKEN_CLOSE_CURLY,
	TOKEN_EQUAL_SIGN,
	TOKEN_OPEN_SQUARE,
	TOKEN_CLOSE_SQUARE,
	TOKEN_VARIABLE,
	TOKEN_AMPERSAND,
	TOKEN_BACKSLASH,
	TOKEN_QMARK,
} tokenType;

#ifdef DEBUG
static const char *tokenTypes[] = {
#define E(X) [TOKEN_##X] = #X
	E(UNDEFINED),
	E(EOF),
	E(CHARACTER),
	E(CLOSE_PAREN),
	E(SEMICOLON),
	E(COLON),
	E(COMMA),
	E(KEYWORD),
	E(OPEN_PAREN),
	E(OPERATOR),
	E(IDENTIFIER),
	E(STRING),
	E(PERIOD),
	E(OPEN_CURLY),
	E(CLOSE_CURLY),
	E(EQUAL_SIGN),
	E(OPEN_SQUARE),
	E(CLOSE_SQUARE),
	E(VARIABLE),
	E(AMPERSAND),
	E(BACKSLASH),
	E(QMARK),
#undef E
};
#endif

typedef struct {
	tokenType		type;
	keywordId		keyword;
	vString *		string;
	vString *		scope;
	unsigned long 	lineNumber;
	MIOPos			filePosition;
	int 			parentKind; /* -1 if none */
	bool			anonymous;	/* true if token specifies
								 * an anonymous class */
} tokenInfo;

static langType Lang_php;
static langType Lang_zephir;

static bool InPhp = false; /* whether we are between <? ?> */
/* whether the next token may be a keyword, e.g. not after "::" or "->" */
static bool MayBeKeyword = true;

/* current statement details */
static struct {
	accessType access;
	implType impl;
} CurrentStatement;

/* Current namespace */
static vString *CurrentNamesapce;
/* Cache variable to build the tag's scope.  It has no real meaning outside
 * of initPhpEntry()'s scope. */
static vString *FullScope;
/* The class name specified at "extends" keyword in the current class
 * definition. Used to resolve "parent" in return type. */
static vString *ParentClass;

static objPool *TokenPool = NULL;

static const char *phpScopeSeparatorFor (int kind, int upperScopeKind)
{
	return scopeSeparatorFor (getInputLanguage(), kind, upperScopeKind);
}

static const char *accessToString (const accessType access)
{
	static const char *const names[COUNT_ACCESS] = {
		"undefined",
		"private",
		"protected",
		"public"
	};

	Assert (access < COUNT_ACCESS);

	return names[access];
}

static const char *implToString (const implType impl)
{
	static const char *const names[COUNT_IMPL] = {
		"undefined",
		"abstract"
	};

	Assert (impl < COUNT_IMPL);

	return names[impl];
}

static void initPhpEntry (tagEntryInfo *const e, const tokenInfo *const token,
						  const phpKind kind, const accessType access)
{
	int parentKind = -1;

	vStringClear (FullScope);

	if (vStringLength (CurrentNamesapce) > 0)
	{
		parentKind = K_NAMESPACE;
		vStringCat (FullScope, CurrentNamesapce);

	}

	initTagEntry (e, vStringValue (token->string), kind);

	updateTagLine (e, token->lineNumber, token->filePosition);

	if (access != ACCESS_UNDEFINED)
		e->extensionFields.access = accessToString (access);
	if (vStringLength (token->scope) > 0)
	{
		parentKind = token->parentKind;

		if (vStringLength (FullScope) > 0)
		{
			const char* sep;

			sep = phpScopeSeparatorFor (parentKind,
						    K_NAMESPACE);
			vStringCatS (FullScope, sep);
		}
			vStringCat (FullScope, token->scope);
	}
	if (vStringLength (FullScope) > 0)
	{
		Assert (parentKind >= 0);

		e->extensionFields.scopeKindIndex = parentKind;
		e->extensionFields.scopeName = vStringValue (FullScope);
	}

	if (token->anonymous)
		markTagExtraBit (e, XTAG_ANONYMOUS);
}

static void  makePhpTagEntry  (tagEntryInfo *const e)
{
	makeTagEntry (e);
	makeQualifiedTagEntry (e);
}

static void fillTypeRefField (tagEntryInfo *const e,
							  const vString *const rtype, const tokenInfo *const token)
{
	if ((vStringLength (rtype) == 4)
		&& (strcmp (vStringValue (rtype), "self") == 0)
		&& vStringLength (token->scope) > 0)
	{
		if (token->parentKind == -1)
			e->extensionFields.typeRef [0] = "unknown";
		else
			e->extensionFields.typeRef [0] = PhpKinds [token->parentKind].name;
		e->extensionFields.typeRef [1] = vStringValue (token->scope);
	}
	else if ((vStringLength (rtype) == 6)
			 && (strcmp (vStringValue (rtype), "parent") == 0)
			 && (ParentClass && vStringLength (ParentClass) > 0))
	{
		e->extensionFields.typeRef [0] = "class";
		e->extensionFields.typeRef [1] = vStringValue (ParentClass);
	}
	else
	{
		e->extensionFields.typeRef [0] = "unknown";
		e->extensionFields.typeRef [1] = vStringValue (rtype);
	}
}

static void makeTypedPhpTag (const tokenInfo *const token, const phpKind kind,
							 const accessType access, vString* typeName)
{
	if (PhpKinds[kind].enabled)
	{
		tagEntryInfo e;

		initPhpEntry (&e, token, kind, access);
		if (typeName)
			fillTypeRefField (&e, typeName, token);
		makePhpTagEntry (&e);
	}
}

static void makeSimplePhpTag (const tokenInfo *const token, const phpKind kind,
							  const accessType access)
{
	makeTypedPhpTag (token, kind, access, NULL);
}

static void makeNamespacePhpTag (const tokenInfo *const token, const vString *const name)
{
	if (PhpKinds[K_NAMESPACE].enabled)
	{
		tagEntryInfo e;

		initTagEntry (&e, vStringValue (name), K_NAMESPACE);

		updateTagLine (&e, token->lineNumber, token->filePosition);

		makePhpTagEntry (&e);
	}
}

static void makeClassOrIfaceTag (const phpKind kind, const tokenInfo *const token,
								 vString *const inheritance, const implType impl)
{
	if (PhpKinds[kind].enabled)
	{
		tagEntryInfo e;

		initPhpEntry (&e, token, kind, ACCESS_UNDEFINED);

		if (impl != IMPL_UNDEFINED)
			e.extensionFields.implementation = implToString (impl);
		if (vStringLength (inheritance) > 0)
			e.extensionFields.inheritance = vStringValue (inheritance);

		makePhpTagEntry (&e);
	}
}

static void makeFunctionTag (const tokenInfo *const token,
							 const vString *const arglist,
							 const vString *const rtype,
							 const accessType access, const implType impl)
{
	if (PhpKinds[K_FUNCTION].enabled)
	{
		tagEntryInfo e;

		initPhpEntry (&e, token, K_FUNCTION, access);

		if (impl != IMPL_UNDEFINED)
			e.extensionFields.implementation = implToString (impl);
		if (arglist)
			e.extensionFields.signature = vStringValue (arglist);
		if (rtype)
			fillTypeRefField (&e, rtype, token);

		makePhpTagEntry (&e);
	}
}

static void *newPoolToken (void *createArg CTAGS_ATTR_UNUSED)
{
	tokenInfo *token = xMalloc (1, tokenInfo);

	token->string = vStringNew ();
	token->scope  = vStringNew ();
	return token;
}

static void clearPoolToken (void *data)
{
	tokenInfo *token = data;

	token->type			= TOKEN_UNDEFINED;
	token->keyword		= KEYWORD_NONE;
	token->lineNumber   = getInputLineNumber ();
	token->filePosition = getInputFilePosition ();
	token->parentKind	= -1;
	token->anonymous	= false;
	vStringClear (token->string);
	vStringClear (token->scope);
}

static void deletePoolToken (void *data)
{
	tokenInfo *token = data;
	vStringDelete (token->string);
	vStringDelete (token->scope);
	eFree (token);
}

static void copyToken (tokenInfo *const dest, const tokenInfo *const src,
					   bool scope)
{
	dest->lineNumber = src->lineNumber;
	dest->filePosition = src->filePosition;
	dest->type = src->type;
	dest->keyword = src->keyword;
	vStringCopy(dest->string, src->string);
	dest->parentKind = src->parentKind;
	if (scope)
		vStringCopy(dest->scope, src->scope);
	dest->anonymous = src->anonymous;
}

#if 0
#include <stdio.h>

static const char *tokenTypeName (const tokenType type)
{
	switch (type)
	{
		case TOKEN_UNDEFINED:		return "undefined";
		case TOKEN_EOF:				return "EOF";
		case TOKEN_CHARACTER:		return "character";
		case TOKEN_CLOSE_PAREN:		return "')'";
		case TOKEN_SEMICOLON:		return "';'";
		case TOKEN_COLON:			return "':'";
		case TOKEN_COMMA:			return "','";
		case TOKEN_OPEN_PAREN:		return "'('";
		case TOKEN_OPERATOR:		return "operator";
		case TOKEN_IDENTIFIER:		return "identifier";
		case TOKEN_KEYWORD:			return "keyword";
		case TOKEN_STRING:			return "string";
		case TOKEN_PERIOD:			return "'.'";
		case TOKEN_OPEN_CURLY:		return "'{'";
		case TOKEN_CLOSE_CURLY:		return "'}'";
		case TOKEN_EQUAL_SIGN:		return "'='";
		case TOKEN_OPEN_SQUARE:		return "'['";
		case TOKEN_CLOSE_SQUARE:	return "']'";
		case TOKEN_VARIABLE:		return "variable";
	}
	return NULL;
}

static void printToken (const tokenInfo *const token)
{
	fprintf (stderr, "%p:\n\ttype:\t%s\n\tline:\t%lu\n\tscope:\t%s\n", (void *) token,
			 tokenTypeName (token->type),
			 token->lineNumber,
			 vStringValue (token->scope));
	switch (token->type)
	{
		case TOKEN_IDENTIFIER:
		case TOKEN_STRING:
		case TOKEN_VARIABLE:
			fprintf (stderr, "\tcontent:\t%s\n", vStringValue (token->string));
			break;

		case TOKEN_KEYWORD:
		{
			size_t n = ARRAY_SIZE (PhpKeywordTable);
			size_t i;

			fprintf (stderr, "\tkeyword:\t");
			for (i = 0; i < n; i++)
			{
				if (PhpKeywordTable[i].id == token->keyword)
				{
					fprintf (stderr, "%s\n", PhpKeywordTable[i].name);
					break;
				}
			}
			if (i >= n)
				fprintf (stderr, "(unknown)\n");
		}

		default: break;
	}
}
#endif

static void addToScope (tokenInfo *const token, const vString *const extra,
			int kindOfUpperScope)
{
	if (vStringLength (token->scope) > 0)
	{
		const char* sep;

		sep = phpScopeSeparatorFor(token->parentKind,
					   kindOfUpperScope);
		vStringCatS (token->scope, sep);
	}
	vStringCat (token->scope, extra);
}

static int skipToCharacter (const int c)
{
	int d;
	do
	{
		d = getcFromInputFile ();
	} while (d != EOF  &&  d != c);
	return d;
}

static void parseString (vString *const string, const int delimiter)
{
	while (true)
	{
		int c = getcFromInputFile ();

		if (c == '\\' && (c = getcFromInputFile ()) != EOF)
			vStringPut (string, c);
		else if (c == EOF || c == delimiter)
			break;
		else
			vStringPut (string, c);
	}
}

/* Strips @indent_len characters from lines in @string to get the correct
 * string value for an indented heredoc (PHP 7.3+).
 * This doesn't handle invalid values specially and might yield surprising
 * results with them, but it doesn't really matter as it's invalid anyway. */
static void stripHeredocIndent (vString *const string, size_t indent_len)
{
	char *str = vStringValue (string);
	size_t str_len = vStringLength (string);
	char *p = str;
	size_t new_len = str_len;
	bool at_line_start = true;

	while (*p)
	{
		if (at_line_start)
		{
			size_t p_len;
			size_t strip_len;

			p_len = str_len - (p - str);
			strip_len = p_len < indent_len ? p_len : indent_len;
			memmove (p, p + strip_len, p_len - strip_len);
			p += strip_len;
			new_len -= strip_len;
		}
		/* CRLF is already normalized as LF */
		at_line_start = (*p == '\r' || *p == '\n');
		p++;
	}
	vStringTruncate (string, new_len);
}

/* reads a PHP >= 7.3 HereDoc or a NowDoc (the part after the <<<).
 * 	<<<[ \t]*(ID|'ID'|"ID")
 * 	...
 * 	[ \t]*ID[^:indent-char:];?
 *
 * note that:
 *  1) starting ID must be immediately followed by a newline;
 *  2) closing ID is the same as opening one;
 *  3) closing ID must not be immediately followed by an identifier character;
 *  4) optional indentation of the closing ID is stripped from body lines,
 *     which lines must have the exact same prefix indentation.
 *
 * This is slightly relaxed from PHP < 7.3, where the closing ID had to be the
 * only thing on its line, with the only exception of a semicolon right after
 * the ID.
 *
 * Example of a single valid heredoc:
 * 	<<< FOO
 * 	something
 * 	something else
 * 	FOO_this is not an end
 * 	FOO;
 * 	# previous line was the end, but the semicolon wasn't required
 *
 * Another example using indentation and more code after the heredoc:
 * 	<<<FOO
 * 		something
 * 		something else
 * 		FOO . 'hello';
 * 	# the heredoc ends at FOO, and leading tabs are stripped from the body.
 * 	# ". 'hello'" is a normal concatenation operator and the string "hello".
 */
static void parseHeredoc (vString *const string)
{
	int c;
	unsigned int len;
	char delimiter[64]; /* arbitrary limit, but more is crazy anyway */
	int quote = 0;

	do
	{
		c = getcFromInputFile ();
	}
	while (c == ' ' || c == '\t');

	if (c == '\'' || c == '"')
	{
		quote = c;
		c = getcFromInputFile ();
	}
	for (len = 0; len < ARRAY_SIZE (delimiter) - 1; len++)
	{
		if (! isIdentChar (c))
			break;
		delimiter[len] = (char) c;
		c = getcFromInputFile ();
	}
	delimiter[len] = 0;

	if (len == 0) /* no delimiter, give up */
		goto error;
	if (quote)
	{
		if (c != quote) /* no closing quote for quoted identifier, give up */
			goto error;
		c = getcFromInputFile ();
	}
	if (c != '\r' && c != '\n') /* missing newline, give up */
		goto error;

	do
	{
		c = getcFromInputFile ();

		vStringPut (string, c);
		if (c == '\r' || c == '\n')
		{
			/* new line, check for a delimiter right after.  No need to handle
			 * CRLF, getcFromInputFile() normalizes it to LF already. */
			const size_t prev_string_len = vStringLength (string) - 1;
			size_t indent_len = 0;

			c = getcFromInputFile ();
			while (c == ' ' || c == '\t')
			{
				vStringPut (string, c);
				c = getcFromInputFile ();
				indent_len++;
			}

			for (len = 0; c != 0 && (c - delimiter[len]) == 0; len++)
				c = getcFromInputFile ();

			if (delimiter[len] != 0)
				ungetcToInputFile (c);
			else if (! isIdentChar (c))
			{
				/* line start matched the delimiter and has a separator, we're done */
				ungetcToInputFile (c);

				/* strip trailing newline and indent of the end delimiter */
				vStringTruncate (string, prev_string_len);

				/* strip indent from the value if needed */
				if (indent_len > 0)
					stripHeredocIndent (string, indent_len);
				break;
			}
			/* if we are here it wasn't a delimiter, so put everything in the
			 * string */
			vStringNCatS (string, delimiter, len);
		}
	}
	while (c != EOF);

	return;

error:
	ungetcToInputFile (c);
}

static void parseIdentifier (vString *const string, const int firstChar)
{
	int c = firstChar;
	do
	{
		vStringPut (string, c);
		c = getcFromInputFile ();
	} while (isIdentChar (c));
	ungetcToInputFile (c);
}

static bool isSpace (int c)
{
	return (c == '\t' || c == ' ' || c == '\v' ||
			c == '\n' || c == '\r' || c == '\f');
}

static int skipWhitespaces (int c)
{
	while (isSpace (c))
		c = getcFromInputFile ();
	return c;
}

/* <script[:white:]+language[:white:]*=[:white:]*(php|'php'|"php")[:white:]*>
 *
 * This is ugly, but the whole "<script language=php>" tag is and we can't
 * really do better without adding a lot of code only for this */
static bool isOpenScriptLanguagePhp (int c)
{
	int quote = 0;

	/* <script[:white:]+language[:white:]*= */
	if (c                                   != '<' ||
		tolower ((c = getcFromInputFile ()))         != 's' ||
		tolower ((c = getcFromInputFile ()))         != 'c' ||
		tolower ((c = getcFromInputFile ()))         != 'r' ||
		tolower ((c = getcFromInputFile ()))         != 'i' ||
		tolower ((c = getcFromInputFile ()))         != 'p' ||
		tolower ((c = getcFromInputFile ()))         != 't' ||
		! isSpace ((c = getcFromInputFile ()))              ||
		tolower ((c = skipWhitespaces (c))) != 'l' ||
		tolower ((c = getcFromInputFile ()))         != 'a' ||
		tolower ((c = getcFromInputFile ()))         != 'n' ||
		tolower ((c = getcFromInputFile ()))         != 'g' ||
		tolower ((c = getcFromInputFile ()))         != 'u' ||
		tolower ((c = getcFromInputFile ()))         != 'a' ||
		tolower ((c = getcFromInputFile ()))         != 'g' ||
		tolower ((c = getcFromInputFile ()))         != 'e' ||
		(c = skipWhitespaces (getcFromInputFile ())) != '=')
		return false;

	/* (php|'php'|"php")> */
	c = skipWhitespaces (getcFromInputFile ());
	if (c == '"' || c == '\'')
	{
		quote = c;
		c = getcFromInputFile ();
	}
	if (tolower (c)                         != 'p' ||
		tolower ((c = getcFromInputFile ()))         != 'h' ||
		tolower ((c = getcFromInputFile ()))         != 'p' ||
		(quote != 0 && (c = getcFromInputFile ()) != quote) ||
		(c = skipWhitespaces (getcFromInputFile ())) != '>')
		return false;

	return true;
}

static int findPhpStart (int *tagStartLineOffset)
{
	int c;
	do
	{
		if ((c = getcFromInputFile ()) == '<')
		{
			*tagStartLineOffset = getInputLineOffset () - 1;
			c = getcFromInputFile ();
			/* <?, <?= and <?php, but not <?xml */
			if (c == '?')
			{
				c = getcFromInputFile ();
				/* echo tag */
				if (c == '=')
					c = getcFromInputFile ();
				/* don't enter PHP mode on "<?xml", yet still support short open tags (<?) */
				else if (tolower (c)                          != 'x' ||
				         tolower ((c = getcFromInputFile ())) != 'm' ||
				         tolower ((c = getcFromInputFile ())) != 'l')
				{
					break;
				}
			}
			/* <script language="php"> */
			else
			{
				ungetcToInputFile (c);
				if (isOpenScriptLanguagePhp ('<'))
					break;
			}
		}
	}
	while (c != EOF);

	return c;
}

static int skipSingleComment (void)
{
	int c;
	do
	{
		c = getcFromInputFile ();
		/* ?> in single-line comments leaves PHP mode */
		if (c == '?')
		{
			int next = getcFromInputFile ();
			if (next == '>')
				InPhp = false;
			else
				ungetcToInputFile (next);
		}
	} while (InPhp && c != EOF && c != '\n' && c != '\r');
	return c;
}

static void readToken (tokenInfo *const token)
{
	int c;
	bool nextMayBeKeyword = true;

	token->type		= TOKEN_UNDEFINED;
	token->keyword	= KEYWORD_NONE;
	vStringClear (token->string);

getNextChar:

	if (! InPhp)
	{
		unsigned long startSourceLineNumber = getSourceLineNumber ();
		unsigned long startLineNumber = getInputLineNumber ();
		int startLineOffset = getInputLineOffset ();
		int endLineOffset;

		c = findPhpStart (&endLineOffset);
		if (c != EOF)
			InPhp = true;
		else
			endLineOffset = getInputLineOffset ();

		unsigned long endLineNumber = getInputLineNumber ();

		makePromise ("HTML", startLineNumber, startLineOffset,
					 endLineNumber, endLineOffset, startSourceLineNumber);
	}
	else
		c = getcFromInputFile ();

	c = skipWhitespaces (c);

	token->lineNumber   = getInputLineNumber ();
	token->filePosition = getInputFilePosition ();

	switch (c)
	{
		case EOF: token->type = TOKEN_EOF;					break;
		case '(': token->type = TOKEN_OPEN_PAREN;			break;
		case ')': token->type = TOKEN_CLOSE_PAREN;			break;
		case ';': token->type = TOKEN_SEMICOLON;			break;
		case ',': token->type = TOKEN_COMMA;				break;
		case '.': token->type = TOKEN_PERIOD;				break;
		case '{': token->type = TOKEN_OPEN_CURLY;			break;
		case '}': token->type = TOKEN_CLOSE_CURLY;			break;
		case '[': token->type = TOKEN_OPEN_SQUARE;			break;
		case ']': token->type = TOKEN_CLOSE_SQUARE;			break;
		case '&': token->type = TOKEN_AMPERSAND;			break;
		case '\\': token->type = TOKEN_BACKSLASH;			break;

		case ':':
		{
			int d = getcFromInputFile ();
			if (d == c) /* :: */
			{
				nextMayBeKeyword = false;
				token->type = TOKEN_OPERATOR;
			}
			else
			{
				ungetcToInputFile (d);
				token->type = TOKEN_COLON;
			}
			break;
		}

		case '=':
		{
			int d = getcFromInputFile ();
			if (d == '=' || d == '>')
				token->type = TOKEN_OPERATOR;
			else
			{
				ungetcToInputFile (d);
				token->type = TOKEN_EQUAL_SIGN;
			}
			break;
		}

		case '\'':
		case '"':
			token->type = TOKEN_STRING;
			parseString (token->string, c);
			token->lineNumber = getInputLineNumber ();
			token->filePosition = getInputFilePosition ();
			break;

		case '<':
		{
			int d = getcFromInputFile ();
			if (d == '/')
			{
				/* </script[:white:]*> */
				if (tolower ((d = getcFromInputFile ())) == 's' &&
					tolower ((d = getcFromInputFile ())) == 'c' &&
					tolower ((d = getcFromInputFile ())) == 'r' &&
					tolower ((d = getcFromInputFile ())) == 'i' &&
					tolower ((d = getcFromInputFile ())) == 'p' &&
					tolower ((d = getcFromInputFile ())) == 't' &&
					(d = skipWhitespaces (getcFromInputFile ())) == '>')
				{
					InPhp = false;
					goto getNextChar;
				}
				else
				{
					ungetcToInputFile (d);
					token->type = TOKEN_UNDEFINED;
				}
			}
			else if (d == '<' && (d = getcFromInputFile ()) == '<')
			{
				token->type = TOKEN_STRING;
				parseHeredoc (token->string);
			}
			else
			{
				ungetcToInputFile (d);
				token->type = TOKEN_UNDEFINED;
			}
			break;
		}

		case '#': /* comment */
			skipSingleComment ();
			goto getNextChar;
			break;

		case '+':
		case '-':
		case '*':
		case '%':
		{
			int d = getcFromInputFile ();
			if (c == '-' && d == '>')
				nextMayBeKeyword = false;
			else if (d != '=')
				ungetcToInputFile (d);
			token->type = TOKEN_OPERATOR;
			break;
		}

		case '/': /* division or comment start */
		{
			int d = getcFromInputFile ();
			if (d == '/') /* single-line comment */
			{
				skipSingleComment ();
				goto getNextChar;
			}
			else if (d == '*')
			{
				do
				{
					c = skipToCharacter ('*');
					if (c != EOF)
					{
						c = getcFromInputFile ();
						if (c == '/')
							break;
						else
							ungetcToInputFile (c);
					}
				} while (c != EOF && c != '\0');
				goto getNextChar;
			}
			else
			{
				if (d != '=')
					ungetcToInputFile (d);
				token->type = TOKEN_OPERATOR;
			}
			break;
		}

		case '$': /* variable start */
		{
			int d = getcFromInputFile ();
			if (! isIdentChar (d))
			{
				ungetcToInputFile (d);
				token->type = TOKEN_UNDEFINED;
			}
			else
			{
				parseIdentifier (token->string, d);
				token->type = TOKEN_VARIABLE;
			}
			break;
		}

		case '?': /* maybe the end of the PHP chunk */
		{
			int d = getcFromInputFile ();
			if (d == '>')
			{
				InPhp = false;
				goto getNextChar;
			}
			else
			{
				ungetcToInputFile (d);
				token->type = TOKEN_QMARK;
			}
			break;
		}

		default:
			if (! isIdentChar (c))
				token->type = TOKEN_UNDEFINED;
			else
			{
				parseIdentifier (token->string, c);
				if (MayBeKeyword)
					token->keyword = lookupCaseKeyword (vStringValue (token->string), getInputLanguage ());
				else
					token->keyword = KEYWORD_NONE;

				if (token->keyword == KEYWORD_NONE)
					token->type = TOKEN_IDENTIFIER;
				else
					token->type = TOKEN_KEYWORD;
			}
			break;
	}

	if (token->type == TOKEN_SEMICOLON ||
		token->type == TOKEN_OPEN_CURLY ||
		token->type == TOKEN_CLOSE_CURLY)
	{
		/* reset current statement details on statement end, and when entering
		 * a deeper scope.
		 * it is a bit ugly to do this in readToken(), but it makes everything
		 * a lot simpler. */
		CurrentStatement.access = ACCESS_UNDEFINED;
		CurrentStatement.impl = IMPL_UNDEFINED;
	}

	MayBeKeyword = nextMayBeKeyword;

	TRACE_PRINT("token: %s (%s)", tokenTypes[token->type], vStringValue (token->string));
}

static void readQualifiedName (tokenInfo *const token, vString *name,
                               tokenInfo *const lastToken)
{
	while (token->type == TOKEN_IDENTIFIER || token->type == TOKEN_BACKSLASH)
	{
		if (name)
		{
			if (token->type == TOKEN_BACKSLASH)
				vStringPut (name, '\\');
			else
				vStringCat (name, token->string);
		}
		if (lastToken)
			copyToken (lastToken, token, true);
		readToken (token);
	}
}

static void enterScope (tokenInfo *const parentToken,
						const vString *const extraScope,
						const int parentKind);

static void skipOverParens (tokenInfo *token)
{
	if (token->type == TOKEN_OPEN_PAREN)
	{
		int depth = 1;

		do
		{
			readToken (token);
			switch (token->type)
			{
				case TOKEN_OPEN_PAREN:  depth++; break;
				case TOKEN_CLOSE_PAREN: depth--; break;
				default: break;
			}
		}
		while (token->type != TOKEN_EOF && depth > 0);

		readToken (token);
	}
}

/* parses a class or an interface:
 * 	class Foo {}
 * 	class Foo extends Bar {}
 * 	class Foo extends Bar implements iFoo, iBar {}
 * 	interface iFoo {}
 * 	interface iBar extends iFoo {}
 *
 * if @name is not NULL, parses an anonymous class with name @name
 * 	new class {}
 * 	new class(1, 2) {}
 * 	new class(1, 2) extends Foo implements iFoo, iBar {} */
static bool parseClassOrIface (tokenInfo *const token, const phpKind kind,
                                  const tokenInfo *name)
{
	bool readNext = true;
	implType impl = CurrentStatement.impl;
	tokenInfo *nameFree = NULL;
	vString *inheritance = NULL;
	vString *parent = NULL;

	readToken (token);
	if (name) /* anonymous class */
	{
		/* skip possible construction arguments */
		skipOverParens (token);
	}
	else /* normal, named class */
	{
		if (token->type != TOKEN_IDENTIFIER)
			return false;

		name = nameFree = newToken ();
		copyToken (nameFree, token, true);

		readToken (token);
	}

	inheritance = vStringNew ();
	/* read every identifiers, keywords and commas, and assume each
	 *  identifier (not keyword) is an inheritance
	 * (like in "class Foo extends Bar implements iA, iB") */
	enum { inheritance_initial,
		   inheritance_extends,
		   inheritance_implements
	} istat = inheritance_initial;
	while (token->type == TOKEN_IDENTIFIER ||
	       token->type == TOKEN_BACKSLASH ||
	       token->type == TOKEN_KEYWORD ||
	       token->type == TOKEN_COMMA)
	{
		if (token->type == TOKEN_IDENTIFIER || token->type == TOKEN_BACKSLASH)
		{
			vString *qualifiedName = vStringNew ();

			readQualifiedName (token, qualifiedName, NULL);
			vStringJoin(inheritance, ',', qualifiedName);
			if (istat == inheritance_extends && !parent)
				parent = qualifiedName;
			else
				vStringDelete (qualifiedName);
		}
		else
		{
			if (token->type == TOKEN_KEYWORD)
			{
				if (token->keyword == KEYWORD_extends)
					istat = inheritance_extends;
				else if (token->keyword == KEYWORD_implements)
					istat = inheritance_implements;
			}
			readToken (token);
		}
	}

	makeClassOrIfaceTag (kind, name, inheritance, impl);

	if (token->type == TOKEN_OPEN_CURLY)
	{
		vString *backup = ParentClass;
		ParentClass = parent;
		enterScope (token, name->string, kind);
		ParentClass = backup;
	}
	else
		readNext = false;

	if (nameFree)
		deleteToken (nameFree);
	vStringDelete (parent);
	vStringDelete (inheritance);

	return readNext;
}

/* parses a trait:
 * 	trait Foo {} */
static bool parseTrait (tokenInfo *const token)
{
	bool readNext = true;
	tokenInfo *name;

	readToken (token);
	if (token->type != TOKEN_IDENTIFIER)
		return false;

	name = newToken ();
	copyToken (name, token, true);

	makeSimplePhpTag (name, K_TRAIT, ACCESS_UNDEFINED);

	readToken (token);
	if (token->type == TOKEN_OPEN_CURLY)
		enterScope (token, name->string, K_TRAIT);
	else
		readNext = false;

	deleteToken (name);

	return readNext;
}

/* parse a function
 *
 * if @name is NULL, parses a normal function
 * 	function myfunc($foo, $bar) {}
 * 	function &myfunc($foo, $bar) {}
 * 	function myfunc($foo, $bar) : type {}
 * 	function myfunc($foo, $bar) : ?type {}
 *
 * if @name is not NULL, parses an anonymous function with name @name
 * 	$foo = function($foo, $bar) {}
 * 	$foo = function&($foo, $bar) {}
 * 	$foo = function($foo, $bar) use ($x, &$y) {}
 * 	$foo = function($foo, $bar) use ($x, &$y) : type {}
 * 	$foo = function($foo, $bar) use ($x, &$y) : ?type {} */
static bool parseFunction (tokenInfo *const token, const tokenInfo *name)
{
	bool readNext = true;
	accessType access = CurrentStatement.access;
	implType impl = CurrentStatement.impl;
	tokenInfo *nameFree = NULL;
	vString *rtype = NULL;
	vString *arglist = NULL;

	readToken (token);
	/* skip a possible leading ampersand (return by reference) */
	if (token->type == TOKEN_AMPERSAND)
		readToken (token);

	if (! name)
	{
		if (token->type != TOKEN_IDENTIFIER && token->type != TOKEN_KEYWORD)
			return false;

		name = nameFree = newToken ();
		copyToken (nameFree, token, true);
		readToken (token);
	}

	if (token->type == TOKEN_OPEN_PAREN)
	{
		int depth = 1;

		arglist = vStringNew ();
		vStringPut (arglist, '(');
		do
		{
			readToken (token);

			switch (token->type)
			{
				case TOKEN_OPEN_PAREN:  depth++; break;
				case TOKEN_CLOSE_PAREN: depth--; break;
				default: break;
			}
			/* display part */
			switch (token->type)
			{
				case TOKEN_AMPERSAND:		vStringPut (arglist, '&');		break;
				case TOKEN_CLOSE_CURLY:		vStringPut (arglist, '}');		break;
				case TOKEN_CLOSE_PAREN:		vStringPut (arglist, ')');		break;
				case TOKEN_CLOSE_SQUARE:	vStringPut (arglist, ']');		break;
				case TOKEN_COLON:			vStringPut (arglist, ':');		break;
				case TOKEN_COMMA:			vStringCatS (arglist, ", ");	break;
				case TOKEN_EQUAL_SIGN:		vStringCatS (arglist, " = ");	break;
				case TOKEN_OPEN_CURLY:		vStringPut (arglist, '{');		break;
				case TOKEN_OPEN_PAREN:		vStringPut (arglist, '(');		break;
				case TOKEN_OPEN_SQUARE:		vStringPut (arglist, '[');		break;
				case TOKEN_PERIOD:			vStringPut (arglist, '.');		break;
				case TOKEN_SEMICOLON:		vStringPut (arglist, ';');		break;
				case TOKEN_BACKSLASH:		vStringPut (arglist, '\\');		break;
				case TOKEN_STRING:
				{
					vStringPut (arglist, '\'');
					vStringCat  (arglist, token->string);
					vStringPut (arglist, '\'');
					break;
				}

				case TOKEN_IDENTIFIER:
				case TOKEN_KEYWORD:
				case TOKEN_VARIABLE:
				{
					switch (vStringLast (arglist))
					{
						case 0:
						case ' ':
						case '{':
						case '(':
						case '[':
						case '.':
						case '\\':
							/* no need for a space between those and the identifier */
							break;

						default:
							vStringPut (arglist, ' ');
							break;
					}
					if (token->type == TOKEN_VARIABLE)
						vStringPut (arglist, '$');
					vStringCat (arglist, token->string);
					break;
				}

				default: break;
			}
		}
		while (token->type != TOKEN_EOF && depth > 0);

		readToken (token); /* normally it's an open brace or "use" keyword */
	}

	/* skip use(...) */
	if (token->type == TOKEN_KEYWORD && token->keyword == KEYWORD_use)
	{
		readToken (token);
		skipOverParens (token);
	}

	/* PHP7 return type declaration or if parsing Zephir, gather function return
	 * type hint to fill typeRef. */
	if ((getInputLanguage () == Lang_php && token->type == TOKEN_COLON) ||
	    (getInputLanguage () == Lang_zephir && token->type == TOKEN_OPERATOR))
	{
		if (arglist)
			rtype = vStringNew ();

		readToken (token);
		if (token->type == TOKEN_QMARK)
		{
			if (rtype)
				vStringPut (rtype, '?');
			readToken (token);
		}
		readQualifiedName (token, rtype, NULL);

		if (rtype && vStringIsEmpty (rtype))
		{
			vStringDelete (rtype);
			rtype = NULL;
		}
	}

	if (arglist)
		makeFunctionTag (name, arglist, rtype, access, impl);

	if (token->type == TOKEN_OPEN_CURLY)
		enterScope (token, name->string, K_FUNCTION);
	else
		readNext = false;

	vStringDelete (rtype);
	vStringDelete (arglist);
	if (nameFree)
		deleteToken (nameFree);

	return readNext;
}

/* parses declarations of the form
 * 	const NAME = VALUE */
static bool parseConstant (tokenInfo *const token)
{
	tokenInfo *name;

	readToken (token); /* skip const keyword */
	if (token->type != TOKEN_IDENTIFIER && token->type != TOKEN_KEYWORD)
		return false;

	name = newToken ();
	copyToken (name, token, true);

	readToken (token);
	if (token->type == TOKEN_EQUAL_SIGN)
		makeSimplePhpTag (name, K_DEFINE, ACCESS_UNDEFINED);

	deleteToken (name);

	return token->type == TOKEN_EQUAL_SIGN;
}

/* parses declarations of the form
 * 	define('NAME', 'VALUE')
 * 	define(NAME, 'VALUE) */
static bool parseDefine (tokenInfo *const token)
{
	int depth = 1;

	readToken (token); /* skip "define" identifier */
	if (token->type != TOKEN_OPEN_PAREN)
		return false;

	readToken (token);
	if (token->type == TOKEN_STRING ||
		token->type == TOKEN_IDENTIFIER)
	{
		makeSimplePhpTag (token, K_DEFINE, ACCESS_UNDEFINED);
		readToken (token);
	}

	/* skip until the close parenthesis.
	 * no need to handle nested blocks since they would be invalid
	 * in this context anyway (the VALUE may only be a scalar, like
	 * 	42
	 * 	(42)
	 * and alike) */
	while (token->type != TOKEN_EOF && depth > 0)
	{
		switch (token->type)
		{
			case TOKEN_OPEN_PAREN:	depth++; break;
			case TOKEN_CLOSE_PAREN:	depth--; break;
			default: break;
		}
		readToken (token);
	}

	return false;
}

/* parses declarations of the form
 * 	use Foo
 * 	use Foo\Bar\Class
 * 	use Foo\Bar\Class as FooBarClass
 * 	use function Foo\Bar\func
 * 	use function Foo\Bar\func as foobarfunc
 * 	use const Foo\Bar\CONST
 * 	use const Foo\Bar\CONST as FOOBARCONST
 * 	use Foo, Bar
 * 	use Foo, Bar as Baz
 * 	use Foo as Test, Bar as Baz
 * 	use Foo\{Bar, Baz as Child, Nested\Other, Even\More as Something} */
static bool parseUse (tokenInfo *const token)
{
	bool readNext = false;
	/* we can't know the use type, because class, interface and namespaces
	 * aliases are the same, and the only difference is the referenced name's
	 * type */
	const char *refType = "unknown";
	vString *refName = vStringNew ();
	tokenInfo *nameToken = newToken ();
	bool grouped = false;

	readToken (token); /* skip use keyword itself */
	if (token->type == TOKEN_KEYWORD && (token->keyword == KEYWORD_function ||
	                                     token->keyword == KEYWORD_const))
	{
		switch (token->keyword)
		{
			case KEYWORD_function:	refType = PhpKinds[K_FUNCTION].name;	break;
			case KEYWORD_const:		refType = PhpKinds[K_DEFINE].name;		break;
			default: break; /* silence compilers */
		}
		readNext = true;
	}

	if (readNext)
		readToken (token);

	readQualifiedName (token, refName, nameToken);
	grouped = readNext = (token->type == TOKEN_OPEN_CURLY);

	do
	{
		size_t refNamePrefixLength = grouped ? vStringLength (refName) : 0;

		/* if it's either not the first name in a comma-separated list, or we
		 * are in a grouped alias and need to read the leaf name */
		if (readNext)
		{
			readToken (token);
			/* in case of a trailing comma (or an empty group) */
			if (token->type == TOKEN_CLOSE_CURLY)
				break;
			readQualifiedName (token, refName, nameToken);
		}

		if (token->type == TOKEN_KEYWORD && token->keyword == KEYWORD_as)
		{
			readToken (token);
			copyToken (nameToken, token, true);
			readToken (token);
		}

		if (nameToken->type == TOKEN_IDENTIFIER && PhpKinds[K_ALIAS].enabled)
		{
			tagEntryInfo entry;

			initPhpEntry (&entry, nameToken, K_ALIAS, ACCESS_UNDEFINED);

			entry.extensionFields.typeRef[0] = refType;
			entry.extensionFields.typeRef[1] = vStringValue (refName);

			makePhpTagEntry (&entry);
		}

		vStringTruncate (refName, refNamePrefixLength);

		readNext = true;
	}
	while (token->type == TOKEN_COMMA);

	if (grouped && token->type == TOKEN_CLOSE_CURLY)
		readToken (token);

	vStringDelete (refName);
	deleteToken (nameToken);

	return (token->type == TOKEN_SEMICOLON);
}

/* parses declarations of the form
 * 	$var = VALUE
 * 	$var; */
static bool parseVariable (tokenInfo *const token, vString * typeName)
{
	tokenInfo *name;
	bool readNext = true;
	accessType access = CurrentStatement.access;

	name = newToken ();
	copyToken (name, token, true);

	readToken (token);
	if (token->type == TOKEN_EQUAL_SIGN)
	{
		phpKind kind = K_VARIABLE;

		if (token->parentKind == K_FUNCTION)
			kind = K_LOCAL_VARIABLE;

		readToken (token);
		if (token->type == TOKEN_KEYWORD &&
			token->keyword == KEYWORD_function &&
			PhpKinds[kind].enabled)
		{
			if (parseFunction (token, name))
				readToken (token);
			readNext = (bool) (token->type == TOKEN_SEMICOLON);
		}
		else
		{
			makeSimplePhpTag (name, kind, access);
			readNext = false;
		}
	}
	else if (token->type == TOKEN_SEMICOLON)
	{
		/* generate tags for variable declarations in classes
		 * 	class Foo {
		 * 		protected $foo;
		 * 	}
		 * but don't get fooled by stuff like $foo = $bar; */
		if (token->parentKind == K_CLASS ||
		    token->parentKind == K_INTERFACE ||
		    token->parentKind == K_TRAIT)
			makeTypedPhpTag (name, K_VARIABLE, access, typeName);
	}
	else
		readNext = false;

	deleteToken (name);

	return readNext;
}

/* parses namespace declarations
 * 	namespace Foo {}
 * 	namespace Foo\Bar {}
 * 	namespace Foo;
 * 	namespace Foo\Bar;
 * 	namespace;
 * 	namespace {} */
static bool parseNamespace (tokenInfo *const token)
{
	tokenInfo *nsToken = newToken ();

	vStringClear (CurrentNamesapce);
	copyToken (nsToken, token, false);

	do
	{
		readToken (token);
		if (token->type == TOKEN_IDENTIFIER)
		{
			if (vStringLength (CurrentNamesapce) > 0)
			{
				const char *sep;

				sep = phpScopeSeparatorFor(K_NAMESPACE,
							   K_NAMESPACE);
				vStringCatS (CurrentNamesapce, sep);
			}
			vStringCat (CurrentNamesapce, token->string);
		}
	}
	while (token->type != TOKEN_EOF &&
		   token->type != TOKEN_SEMICOLON &&
		   token->type != TOKEN_OPEN_CURLY);

	if (vStringLength (CurrentNamesapce) > 0)
		makeNamespacePhpTag (nsToken, CurrentNamesapce);

	if (token->type == TOKEN_OPEN_CURLY)
		enterScope (token, NULL, -1);

	deleteToken (nsToken);

	return true;
}

/* skip trait uses not to confuse typerefs
 * 	use Name;
 * 	use \\Some\\Name;
 * 	use Name, Other;
 * 	use Name { method as private; }
 * 	use Name, Other { otherMethod as public otherName; }
 *
 * Note that curly braces are only allowed on the last `use`, this is a syntax
 * error:
 * 	use Name { method as private; }, Other;
 * This has to be split in two `use`es or reordered.
 */
static bool parseClassUse (tokenInfo *const token)
{
	do
	{
		readToken (token);
		if (token->type == TOKEN_OPEN_CURLY)
		{
			enterScope (token, NULL, -1);
			return true;
		}
	}
	while (token->type == TOKEN_IDENTIFIER ||
		   token->type == TOKEN_BACKSLASH ||
		   token->type == TOKEN_COMMA);

	return (token->type == TOKEN_SEMICOLON);
}

static void enterScope (tokenInfo *const parentToken,
						const vString *const extraScope,
						const int parentKind)
{
	TRACE_ENTER();

	tokenInfo *token = newToken ();
	vString *typeName = vStringNew ();
	int origParentKind = parentToken->parentKind;

	copyToken (token, parentToken, true);

	if (extraScope)
	{
		token->parentKind = parentKind;
		addToScope (token, extraScope, origParentKind);
	}

	readToken (token);
	while (token->type != TOKEN_EOF &&
		   token->type != TOKEN_CLOSE_CURLY)
	{
		bool readNext = true;

		switch (token->type)
		{
			case TOKEN_OPEN_CURLY:
				enterScope (token, NULL, -1);
				break;

			case TOKEN_KEYWORD:
				switch (token->keyword)
				{
					/* handle anonymous classes */
					case KEYWORD_new:
						readToken (token);
						if (token->keyword != KEYWORD_class)
							readNext = false;
						else
						{
							tokenInfo *name = newToken ();

							copyToken (name, token, true);
							anonGenerate (name->string, "AnonymousClass", K_CLASS);
							name->anonymous = true;
							readNext = parseClassOrIface (token, K_CLASS, name);
							deleteToken (name);
						}
						break;

					case KEYWORD_class:		readNext = parseClassOrIface (token, K_CLASS, NULL);		break;
					case KEYWORD_interface:	readNext = parseClassOrIface (token, K_INTERFACE, NULL);	break;
					case KEYWORD_trait:		readNext = parseTrait (token);								break;
					case KEYWORD_function:	readNext = parseFunction (token, NULL);						break;
					case KEYWORD_const:		readNext = parseConstant (token);							break;
					case KEYWORD_define:	readNext = parseDefine (token);								break;

					case KEYWORD_use:
						/* aliases are only allowed at root scope, but the keyword
						 * is also used to i.e. "import" traits into a class */
						if (vStringLength (token->scope) == 0)
							readNext = parseUse (token);
						else if (parentKind == K_CLASS || parentKind == K_TRAIT)
							readNext = parseClassUse (token);
						break;

					case KEYWORD_namespace:	readNext = parseNamespace (token);	break;

					case KEYWORD_private:	CurrentStatement.access = ACCESS_PRIVATE;	break;
					case KEYWORD_protected:	CurrentStatement.access = ACCESS_PROTECTED;	break;
					case KEYWORD_public:	CurrentStatement.access = ACCESS_PUBLIC;	break;
					case KEYWORD_var:		CurrentStatement.access = ACCESS_PUBLIC;	break;

					case KEYWORD_abstract:	CurrentStatement.impl = IMPL_ABSTRACT;		break;

					default: break;
				}
				break;

			case TOKEN_QMARK:
				vStringClear (typeName);
				vStringPut (typeName, '?');
				readNext = true;
				break;
			case TOKEN_IDENTIFIER:
				vStringCat (typeName, token->string);
				readNext = true;
				break;
			case TOKEN_VARIABLE:
				readNext = parseVariable (token,
										  vStringIsEmpty(typeName)
										  ? NULL
										  : typeName);
				vStringClear (typeName);
				break;

			default: break;
		}

		if (readNext)
			readToken (token);
	}

	copyToken (parentToken, token, false);
	parentToken->parentKind = origParentKind;
	vStringDelete (typeName);
	deleteToken (token);

	TRACE_LEAVE();
}

static void findTags (bool startsInPhpMode)
{
	TRACE_ENTER();

	tokenInfo *const token = newToken ();

	InPhp = startsInPhpMode;
	MayBeKeyword = true;
	CurrentStatement.access = ACCESS_UNDEFINED;
	CurrentStatement.impl = IMPL_UNDEFINED;
	CurrentNamesapce = vStringNew ();
	FullScope = vStringNew ();
	Assert (ParentClass == NULL);

	do
	{
		enterScope (token, NULL, -1);
	}
	while (token->type != TOKEN_EOF); /* keep going even with unmatched braces */

	vStringDelete (FullScope);
	vStringDelete (CurrentNamesapce);
	deleteToken (token);

	TRACE_LEAVE();
}

static void findPhpTags (void)
{
	TRACE_ENTER();
	findTags (false);
	TRACE_LEAVE();
}

static void findZephirTags (void)
{
	TRACE_ENTER();
	findTags (true);
	TRACE_LEAVE();
}

static void initializePool (void)
{
	if (TokenPool == NULL)
		TokenPool = objPoolNew (16, newPoolToken, deletePoolToken, clearPoolToken, NULL);
}

static void initializePhpParser (const langType language)
{
	Lang_php = language;
	initializePool ();
}

static void initializeZephirParser (const langType language)
{
	Lang_zephir = language;
	initializePool ();
}

static void finalize (langType language CTAGS_ATTR_UNUSED, bool initialized)
{
	if (!initialized)
		return;

	if (TokenPool != NULL)
	{
		objPoolDelete (TokenPool);
		TokenPool = NULL;
	}
}

extern parserDefinition* PhpParser (void)
{
	static const char *const extensions [] = { "php", "php3", "php4", "php5", "php7", "phtml", NULL };
	parserDefinition* def = parserNew ("PHP");
	def->kindTable      = PhpKinds;
	def->kindCount  = ARRAY_SIZE (PhpKinds);
	def->extensions = extensions;
	def->parser     = findPhpTags;
	def->initialize = initializePhpParser;
	def->finalize   = finalize;
	def->keywordTable = PhpKeywordTable;
	def->keywordCount = ARRAY_SIZE (PhpKeywordTable);
	return def;
}

extern parserDefinition* ZephirParser (void)
{
	static const char *const extensions [] = { "zep", NULL };
	parserDefinition* def = parserNew ("Zephir");
	def->kindTable      = PhpKinds;
	def->kindCount  = ARRAY_SIZE (PhpKinds);
	def->extensions = extensions;
	def->parser     = findZephirTags;
	def->initialize = initializeZephirParser;
	def->finalize   = finalize;
	def->keywordTable = PhpKeywordTable;
	def->keywordCount = ARRAY_SIZE (PhpKeywordTable);
	return def;
}
