/*
 *	 Copyright (c) 2003, Darren Hiebert
 *
 *	 This source code is released for free distribution under the terms of the
 *	 GNU General Public License version 2 or (at your option) any later version.
 *
 *	 This module contains functions for generating tags for JavaScript language
 *	 files.
 *
 *	 Reference: http://www.ecma-international.org/publications/files/ECMA-ST/Ecma-262.pdf
 *
 *	 This is a good reference for different forms of the function statement:
 *		 http://www.permadi.com/tutorial/jsFunc/
 *   Another good reference:
 *       http://developer.mozilla.org/en/docs/Core_JavaScript_1.5_Guide
 */

/*
 *	 INCLUDE FILES
 */
#include "general.h"	/* must always come first */
#include <ctype.h>	/* to define isalpha () */
#ifdef DEBUG
#include <stdio.h>
#endif

#ifdef HAVE_ICONV
#include <iconv.h>
#include <errno.h>
#	ifdef WORDS_BIGENDIAN
#		define INTERNAL_ENCODING "UTF-32BE"
#	else
#		define INTERNAL_ENCODING "UTF-32LE"
#	endif /* WORDS_BIGENDIAN */
#endif

#include <string.h>
#include "debug.h"
#include "entry.h"
#include "htable.h"
#include "keyword.h"
#include "parse.h"
#include "read.h"
#include "routines.h"
#include "vstring.h"
#include "objpool.h"
#include "options.h"
#include "mbcs.h"
#include "trace.h"

/*
 *	 MACROS
 */
#define isType(token,t)		(bool) ((token)->type == (t))
#define isKeyword(token,k)	(bool) ((token)->keyword == (k))
#define isIdentChar(c) \
	(isalpha (c) || isdigit (c) || (c) == '$' || \
		(c) == '@' || (c) == '_' || (c) == '#' || \
		(c) >= 0x80)
#define newToken() (objPoolGet (TokenPool))
#define deleteToken(t) (objPoolPut (TokenPool, (t)))

/*
 *	 DATA DECLARATIONS
 */

/*	Used to specify type of keyword.
*/
enum eKeywordId {
	KEYWORD_function,
	KEYWORD_capital_function,
	KEYWORD_capital_object,
	KEYWORD_prototype,
	KEYWORD_var,
	KEYWORD_let,
	KEYWORD_const,
	KEYWORD_new,
	KEYWORD_this,
	KEYWORD_for,
	KEYWORD_while,
	KEYWORD_do,
	KEYWORD_if,
	KEYWORD_else,
	KEYWORD_switch,
	KEYWORD_try,
	KEYWORD_catch,
	KEYWORD_finally,
	KEYWORD_sap,
	KEYWORD_return,
	KEYWORD_class,
	KEYWORD_extends,
	KEYWORD_static,
	KEYWORD_default,
	KEYWORD_export,
	KEYWORD_async,
	KEYWORD_get,
	KEYWORD_set,
};
typedef int keywordId; /* to allow KEYWORD_NONE */

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
	TOKEN_IDENTIFIER,
	TOKEN_STRING,
	TOKEN_TEMPLATE_STRING,
	TOKEN_PERIOD,
	TOKEN_OPEN_CURLY,
	TOKEN_CLOSE_CURLY,
	TOKEN_EQUAL_SIGN,
	TOKEN_OPEN_SQUARE,
	TOKEN_CLOSE_SQUARE,
	TOKEN_REGEXP,
	TOKEN_POSTFIX_OPERATOR,
	TOKEN_STAR,
	TOKEN_BINARY_OPERATOR
} tokenType;

typedef struct sTokenInfo {
	tokenType		type;
	keywordId		keyword;
	vString *		string;
	int 			scope;
	unsigned long 	lineNumber;
	MIOPos 			filePosition;
	int				nestLevel;
	bool			dynamicProp;
} tokenInfo;

/*
 *	DATA DEFINITIONS
 */

static tokenType LastTokenType;
static tokenInfo *NextToken;

static langType Lang_js;

static objPool *TokenPool = NULL;

#ifdef HAVE_ICONV
static iconv_t JSUnicodeConverter = (iconv_t) -2;
#endif

typedef enum {
	JSTAG_FUNCTION,
	JSTAG_CLASS,
	JSTAG_METHOD,
	JSTAG_PROPERTY,
	JSTAG_CONSTANT,
	JSTAG_VARIABLE,
	JSTAG_GENERATOR,
	JSTAG_GETTER,
	JSTAG_SETTER,
	JSTAG_COUNT
} jsKind;

typedef enum {
	JS_FUNCTION_NAMECHAIN,
} jsFunctionRole;

typedef enum {
	JS_CLASS_NAMECHAIN,
} jsClassRole;

static roleDefinition JsFunctionRoles [] = {
	{ false, "namechain", "(EXPERIMENTAL)used as a part of a name chain line a.b.c" },
};

static roleDefinition JsClassRoles [] = {
	{ false, "namechain", "(EXPERIMENTAL)used as a part of a name chain line a.b.c" },
};

static kindDefinition JsKinds [] = {
	{ true,  'f', "function",	  "functions"		   ,
	  .referenceOnly = false, ATTACH_ROLES(JsFunctionRoles)},
	{ true,  'c', "class",		  "classes"			   ,
	  .referenceOnly = false, ATTACH_ROLES(JsClassRoles)},
	{ true,  'm', "method",		  "methods"			   },
	{ true,  'p', "property",	  "properties"		   },
	{ true,  'C', "constant",	  "constants"		   },
	{ true,  'v', "variable",	  "global variables"   },
	{ true,  'g', "generator",	  "generators"		   },
	{ true,  'G', "getter",		  "getters"			   },
	{ true,  'S', "setter",		  "setters"			   },
};

static const keywordTable JsKeywordTable [] = {
	/* keyword		keyword ID */
	{ "function",	KEYWORD_function			},
	{ "Function",	KEYWORD_capital_function	},
	{ "Object",		KEYWORD_capital_object		},
	{ "prototype",	KEYWORD_prototype			},
	{ "var",		KEYWORD_var					},
	{ "let",		KEYWORD_let					},
	{ "const",		KEYWORD_const				},
	{ "new",		KEYWORD_new					},
	{ "this",		KEYWORD_this				},
	{ "for",		KEYWORD_for					},
	{ "while",		KEYWORD_while				},
	{ "do",			KEYWORD_do					},
	{ "if",			KEYWORD_if					},
	{ "else",		KEYWORD_else				},
	{ "switch",		KEYWORD_switch				},
	{ "try",		KEYWORD_try					},
	{ "catch",		KEYWORD_catch				},
	{ "finally",	KEYWORD_finally				},
	{ "sap",	    KEYWORD_sap    				},
	{ "return",		KEYWORD_return				},
	{ "class",		KEYWORD_class				},
	{ "extends",	KEYWORD_extends				},
	{ "static",		KEYWORD_static				},
	{ "default",	KEYWORD_default				},
	{ "export",		KEYWORD_export				},
	{ "async",		KEYWORD_async				},
	{ "get",		KEYWORD_get					},
	{ "set",		KEYWORD_set					},
};

typedef enum {
	OPENUI5TAG_CLASS,
} OpenUI5Kind;

static kindDefinition OpenUI5Kinds [] = {
	{ false,  'c', "class",	  "(EXPERIMENTAL, this will be renaemd to controller)classes"  },
};

/*
 *	 FUNCTION DEFINITIONS
 */

/* Recursive functions */
static void readTokenFull (tokenInfo *const token, bool include_newlines, vString *const repr);
static void parseFunction (tokenInfo *const token);
static bool parseBlock (tokenInfo *const token, int parentScope);
static bool parseLine (tokenInfo *const token, bool is_inside_class);
static void parseUI5 (tokenInfo *const token);

#ifdef DO_TRACING
static void dumpToken (const tokenInfo *const token);
static const char *tokenTypeName(enum eTokenType e);
static const char *keywordName(enum eKeywordId e);
static const char *kindName (int k);
#endif

static void *newPoolToken (void *createArg CTAGS_ATTR_UNUSED)
{
	tokenInfo *token = xMalloc (1, tokenInfo);

	token->string		= vStringNew ();
	token->scope		= CORK_NIL;

	return token;
}

static void clearPoolToken (void *data)
{
	tokenInfo *token = data;

	token->type			= TOKEN_UNDEFINED;
	token->keyword		= KEYWORD_NONE;
	token->nestLevel	= 0;
	token->dynamicProp  = false;
	token->lineNumber   = getInputLineNumber ();
	token->filePosition = getInputFilePosition ();
	vStringClear (token->string);
	token->scope = CORK_NIL;
}

static void deletePoolToken (void *data)
{
	tokenInfo *token = data;
	vStringDelete (token->string);
	token->scope = CORK_NIL;
	eFree (token);
}

static void copyToken (tokenInfo *const dest, const tokenInfo *const src,
                       bool const include_non_read_info)
{
	dest->lineNumber = src->lineNumber;
	dest->filePosition = src->filePosition;
	dest->type = src->type;
	dest->keyword = src->keyword;
	dest->dynamicProp = src->dynamicProp;
	vStringCopy(dest->string, src->string);
	if (include_non_read_info)
	{
		dest->nestLevel = src->nestLevel;
		dest->scope = src->scope;
	}
}

static void injectDynamicName (tokenInfo *const token, vString *newName)
{
	token->dynamicProp = true;
	vStringDelete (token->string);
	token->string = newName;
}

/*
 *	 Tag generation functions
 */
static bool isAlreadyTaggedForTag (tagEntryInfo *e, int *exitingScopeIndex);
static bool isAlreadyTaggedForToken (int kindIndex, tokenInfo *const token, int *existing);

static int makeJsTagCommon (const tokenInfo *const token, const jsKind kind,
							 vString *const signature, vString *const inheritance,
							 bool anonymous)
{
	int r = CORK_NIL;
	{
		const char *name = vStringValue (token->string);
		int fullscope = token->scope;
		const char *p;
		vString *middle_name = NULL;
		tagEntryInfo e;

		if (!token->dynamicProp && kind != JSTAG_PROPERTY &&  (p = strrchr (name, '.')) != NULL )
		{
			middle_name = vStringNewNInit (name, (size_t) (p - name));
			name = p + 1;
		}

		initTagEntry (&e, name, kind);

		/* To fill the scope field of "c" of "a.b.c",
		 * "b" must be tagged if the cork API is used.
		 * Here we tag "b" with "namechain" role.
		 * ----------------------------------------------------------
		 * The original values passed to this func:
		 *   (fullscope: "a", given token "b.c")
		 * The values are converted to following tags:
		 *   a  kind:class
		 *   b	kind:class	scope:class:a roles:namechanin
		 *   c	kind:something scope:class:a.b
		 *
		 * TODO: Handle "a.b.c.d" properly.
		 */
		if (middle_name)
		{
			tagEntryInfo middle_entry;
			/* FIXME: proper parent type */
			jsKind parent_kind = JSTAG_CLASS;
			int parent_role = JS_CLASS_NAMECHAIN;

			/*
			 * If we're creating a function (and not a method),
			 * guess we're inside another function
			 */
			if (kind == JSTAG_FUNCTION)
			{
				parent_kind = JSTAG_FUNCTION;
				parent_role = JS_FUNCTION_NAMECHAIN;
			}

			TRACE_PRINT("Emitting reference tag of %s role for symbol '%s' of kind %s with scope '%s'",
						JsKinds [parent_kind].roles[parent_role].name,
						vStringValue (middle_name),
						JsKinds [parent_kind].name,
						fullscope == CORK_NIL? "": getEntryInCorkQueue (fullscope)->name);

			initRefTagEntry (&middle_entry, vStringValue (middle_name), parent_kind,
							 parent_role);
			middle_entry.lineNumber = token->lineNumber;
			middle_entry.filePosition = token->filePosition;
			middle_entry.extensionFields.scopeIndex = fullscope;
			fullscope = makeTagEntry (&middle_entry);
			vStringDelete (middle_name);
		}

		TRACE_PRINT("Emitting tag for symbol '%s' of kind %s with scope '%s'",
					name, JsKinds [kind].name,
					(fullscope == CORK_NIL)
					? ""
					: (getEntryInCorkQueue (fullscope)->placeholder
					   ? "<patchedLater>"
					   : getEntryInCorkQueue (fullscope)->name));

		e.lineNumber   = token->lineNumber;
		e.filePosition = token->filePosition;

		e.extensionFields.scopeIndex = fullscope;

		if (signature && vStringLength(signature))
		{
			size_t i;
			/* sanitize signature by replacing all control characters with a
			 * space (because it's simple).
			 * there should never be any junk in a valid signature, but who
			 * knows what the user wrote and CTags doesn't cope well with weird
			 * characters. */
			for (i = 0; i < signature->length; i++)
			{
				unsigned char c = (unsigned char) signature->buffer[i];
				if (c < 0x20 /* below space */ || c == 0x7F /* DEL */)
					signature->buffer[i] = ' ';
			}
			e.extensionFields.signature = vStringValue(signature);
		}

		if (inheritance)
			e.extensionFields.inheritance = vStringValue(inheritance);

		if (anonymous)
			markTagExtraBit (&e, XTAG_ANONYMOUS);

		if (isAlreadyTaggedForTag (&e, &r))
			return r;

		r = makeTagEntry (&e);
	}
	return r;
}

static int makeJsTag (const tokenInfo *const token, const jsKind kind,
					   vString *const signature, vString *const inheritance)
{
	return makeJsTagCommon (token, kind, signature, inheritance, false);
}

static int makeClassTagCommon (tokenInfo *const token, vString *const signature,
                          vString *const inheritance, bool anonymous)
{
	int r;
	{
		if (!isAlreadyTaggedForToken (JSTAG_CLASS, token, &r))
			r = makeJsTagCommon (token, JSTAG_CLASS, signature, inheritance,
								 anonymous);
	}
	return r;
}

static int makeClassTag (tokenInfo *const token, vString *const signature,
						  vString *const inheritance)
{
	return makeClassTagCommon (token, signature, inheritance, false);
}

static int makeFunctionTagCommon (tokenInfo *const token, vString *const signature, bool generator,
								   bool anonymous)
{
	int r;
	{
		int kindIndex = generator ? JSTAG_GENERATOR : JSTAG_FUNCTION;
		if (!isAlreadyTaggedForToken (kindIndex, token, &r))
			r = makeJsTagCommon (token, kindIndex, signature, NULL, anonymous);
	}
	return r;
}

static int makeFunctionTag (tokenInfo *const token, vString *const signature, bool generator)
{
	return makeFunctionTagCommon (token, signature, generator, false);
}

static void patchScopeFieldOfEntriesBetween (int start, int end, int scopeIndex)
{
	if (! (end - start > 1))
		return;

	for (int i = start + 1; i < end; i++)
	{
		tagEntryInfo *e = getEntryInCorkQueue (i);
		if (e && e->extensionFields.scopeIndex == start)
			patchScopeFieldForCorkEntry (i, scopeIndex);
	}
}

/*
 *	 Parsing functions
 */

/* given @p point, returns the first byte of the encoded output sequence, and
 * make sure the next ones will be returned by calls to getcFromInputFile()
 * as if the code point was simply written in the input file. */
static int handleUnicodeCodePoint (uint32_t point)
{
	int c = (int) point;

	Assert (point < 0x110000);

#ifdef HAVE_ICONV
	/* if we do have iconv and the encodings are specified, use this */
	if (isConverting () && JSUnicodeConverter == (iconv_t) -2)
	{
		/* if we didn't try creating the converter yet, try and do so */
		JSUnicodeConverter = iconv_open (getLanguageEncoding (Lang_js), INTERNAL_ENCODING);
	}
	if (isConverting () && JSUnicodeConverter != (iconv_t) -1)
	{
		char *input_ptr = (char *) &point;
		size_t input_left = sizeof point;
		/* 4 bytes should be enough for any encoding (it's how much UTF-32
		 * would need). */
		/* FIXME: actually iconv has a tendency to output a BOM for Unicode
		 * encodings where it matters when the endianess is not specified in
		 * the target encoding name.  E.g., if the target encoding is "UTF-32"
		 * or "UTF-16" it will output 2 code points, the BOM (U+FEFF) and the
		 * one we expect. This does not happen if the endianess is specified
		 * explicitly, e.g. with "UTF-32LE", or "UTF-16BE".
		 * However, it's not very relevant for the moment as nothing in CTags
		 * cope well (if at all) with non-ASCII-compatible encodings like
		 * UTF-32 or UTF-16 anyway. */
		char output[4] = { 0 };
		char *output_ptr = output;
		size_t output_left = ARRAY_SIZE (output);

		if (iconv (JSUnicodeConverter, &input_ptr, &input_left, &output_ptr, &output_left) == (size_t) -1)
		{
			/* something went wrong, which probably means the output encoding
			 * cannot represent the character.  Use a placeholder likely to be
			 * supported instead, that's also valid in an identifier */
			verbose ("JavaScript: Encoding: %s\n", strerror (errno));
			c = '_';
		}
		else
		{
			const size_t output_len = ARRAY_SIZE (output) - output_left;

			/* put all but the first byte back so that getcFromInputFile() will
			 * return them in the right order */
			for (unsigned int i = 1; i < output_len; i++)
				ungetcToInputFile ((unsigned char) output[output_len - i]);
			c = (unsigned char) output[0];
		}

		iconv (JSUnicodeConverter, NULL, NULL, NULL, NULL);
	}
	else
#endif
	{
		/* when no encoding is specified (or no iconv), assume UTF-8 is good.
		 * Why UTF-8?  Because it's an ASCII-compatible common Unicode encoding. */
		if (point < 0x80)
			c = (unsigned char) point;
		else if (point < 0x800)
		{
			c = (unsigned char) (0xc0 | ((point >> 6) & 0x1f));
			ungetcToInputFile ((unsigned char) (0x80 | (point & 0x3f)));
		}
		else if (point < 0x10000)
		{
			c = (unsigned char) (0xe0 | ((point >> 12) & 0x0f));
			ungetcToInputFile ((unsigned char) (0x80 | ((point >>  0) & 0x3f)));
			ungetcToInputFile ((unsigned char) (0x80 | ((point >>  6) & 0x3f)));
		}
		else if (point < 0x110000)
		{
			c = (unsigned char) (0xf0 | ((point >> 18) & 0x07));
			ungetcToInputFile ((unsigned char) (0x80 | ((point >>  0) & 0x3f)));
			ungetcToInputFile ((unsigned char) (0x80 | ((point >>  6) & 0x3f)));
			ungetcToInputFile ((unsigned char) (0x80 | ((point >> 12) & 0x3f)));
		}
	}

	return c;
}

/* reads a Unicode escape sequence after the "\" prefix.
 * @param value Location to store the escape sequence value.
 * @param isUTF16 Location to store whether @param value is an UTF-16 word.
 * @returns Whether a valid sequence was read. */
static bool readUnicodeEscapeSequenceValue (uint32_t *const value,
                                            bool *const isUTF16)
{
	bool valid = false;
	int d = getcFromInputFile ();

	if (d != 'u')
		ungetcToInputFile (d);
	else
	{
		int e = getcFromInputFile ();
		char cp[6 + 1]; /* up to 6 hex + possible closing '}' or invalid char */
		unsigned int cp_len = 0;

		*isUTF16 = (e != '{');
		if (e == '{')
		{	/* Handles Unicode code point escapes: \u{ HexDigits }
			 * We skip the leading 0s because there can be any number of them
			 * and they don't change any meaning. */
			bool has_leading_zero = false;

			while ((cp[cp_len] = (char) getcFromInputFile ()) == '0')
				has_leading_zero = true;

			while (isxdigit (cp[cp_len]) && ++cp_len < ARRAY_SIZE (cp))
				cp[cp_len] = (char) getcFromInputFile ();
			valid = ((cp_len > 0 || has_leading_zero) &&
					 cp_len < ARRAY_SIZE (cp) && cp[cp_len] == '}' &&
					 /* also check if it's a valid Unicode code point */
					 (cp_len < 6 ||
					  (cp_len == 6 && strncmp (cp, "110000", 6) < 0)));
			if (! valid) /* put back the last (likely invalid) character */
				ungetcToInputFile (cp[cp_len]);
		}
		else
		{	/* Handles Unicode escape sequences: \u Hex4Digits */
			do
				cp[cp_len] = (char) ((cp_len == 0) ? e : getcFromInputFile ());
			while (isxdigit (cp[cp_len]) && ++cp_len < 4);
			valid = (cp_len == 4);
		}

		if (! valid)
		{
			/* we don't get every character back, but it would require to
			 * be able to put up to 9 characters back (in the worst case
			 * for handling invalid \u{10FFFFx}), and here we're recovering
			 * from invalid syntax anyway. */
			ungetcToInputFile (e);
			ungetcToInputFile (d);
		}
		else
		{
			*value = 0;
			for (unsigned int i = 0; i < cp_len; i++)
			{
				*value *= 16;

				/* we know it's a hex digit, no need to double check */
				if (cp[i] < 'A')
					*value += (unsigned int) cp[i] - '0';
				else if (cp[i] < 'a')
					*value += 10 + (unsigned int) cp[i] - 'A';
				else
					*value += 10 + (unsigned int) cp[i] - 'a';
			}
		}
	}

	return valid;
}

static int valueToXDigit (unsigned char v)
{
	Assert (v <= 0xF);

	if (v >= 0xA)
		return 'A' + (v - 0xA);
	else
		return '0' + v;
}

/* Reads and expands a Unicode escape sequence after the "\" prefix.  If the
 * escape sequence is a UTF16 high surrogate, also try and read the low
 * surrogate to emit the proper code point.
 * @param fallback The character to return if the sequence is invalid. Usually
 *                 this would be the '\' character starting the sequence.
 * @returns The first byte of the sequence, or @param fallback if the sequence
 *          is invalid. On success, next calls to getcFromInputFile() will
 *          return subsequent bytes (if any). */
static int readUnicodeEscapeSequence (const int fallback)
{
	int c;
	uint32_t value;
	bool isUTF16;

	if (! readUnicodeEscapeSequenceValue (&value, &isUTF16))
		c = fallback;
	else
	{
		if (isUTF16 && (value & 0xfc00) == 0xd800)
		{	/* this is a high surrogate, try and read its low surrogate and
			 * emit the resulting code point */
			uint32_t low;
			int d = getcFromInputFile ();

			if (d != '\\' || ! readUnicodeEscapeSequenceValue (&low, &isUTF16))
				ungetcToInputFile (d);
			else if (! isUTF16)
			{	/* not UTF-16 low surrogate but a plain code point */
				d = handleUnicodeCodePoint (low);
				ungetcToInputFile (d);
			}
			else if ((low & 0xfc00) != 0xdc00)
			{	/* not a low surrogate, so put back the escaped representation
				 * in case it was another high surrogate we should read as part
				 * of another pair. */
				ungetcToInputFile (valueToXDigit ((unsigned char) ((low & 0x000f) >>  0)));
				ungetcToInputFile (valueToXDigit ((unsigned char) ((low & 0x00f0) >>  4)));
				ungetcToInputFile (valueToXDigit ((unsigned char) ((low & 0x0f00) >>  8)));
				ungetcToInputFile (valueToXDigit ((unsigned char) ((low & 0xf000) >> 12)));
				ungetcToInputFile ('u');
				ungetcToInputFile ('\\');
			}
			else
				value = 0x010000 + ((value & 0x03ff) << 10) + (low & 0x03ff);
		}
		c = handleUnicodeCodePoint (value);
	}

	return c;
}

static void parseString (vString *const string, const int delimiter)
{
	bool end = false;
	while (! end)
	{
		int c = getcFromInputFile ();
		if (c == EOF)
			end = true;
		else if (c == '\\')
		{
			/* Eat the escape sequence (\", \', etc).  We properly handle
			 * <LineContinuation> by eating a whole \<CR><LF> not to see <LF>
			 * as an unescaped character, which is invalid and handled below.
			 * Also, handle the fact that <LineContinuation> produces an empty
			 * sequence.
			 * See ECMA-262 7.8.4 */
			c = getcFromInputFile ();
			if (c == 'u')
			{
				ungetcToInputFile (c);
				c = readUnicodeEscapeSequence ('\\');
				vStringPut (string, c);
			}
			else if (c != '\r' && c != '\n')
				vStringPut(string, c);
			else if (c == '\r')
			{
				c = getcFromInputFile();
				if (c != '\n')
					ungetcToInputFile (c);
			}
		}
		else if (c == delimiter)
			end = true;
		else if (c == '\r' || c == '\n')
		{
			/* those are invalid when not escaped */
			end = true;
			/* we don't want to eat the newline itself to let the automatic
			 * semicolon insertion code kick in */
			ungetcToInputFile (c);
		}
		else
			vStringPut (string, c);
	}
}

static void parseRegExp (void)
{
	int c;
	bool in_range = false;

	do
	{
		c = getcFromInputFile ();
		if (! in_range && c == '/')
		{
			do /* skip flags */
			{
				c = getcFromInputFile ();
			} while (isalpha (c));
			ungetcToInputFile (c);
			break;
		}
		else if (c == '\n' || c == '\r')
		{
			/* invalid in a regex */
			ungetcToInputFile (c);
			break;
		}
		else if (c == '\\')
			c = getcFromInputFile (); /* skip next character */
		else if (c == '[')
			in_range = true;
		else if (c == ']')
			in_range = false;
	} while (c != EOF);
}

/*	Read a C identifier beginning with "firstChar" and places it into
 *	"name".
 */
static void parseIdentifier (vString *const string, const int firstChar)
{
	int c = firstChar;
	Assert (isIdentChar (c));
	do
	{
		vStringPut (string, c);
		c = getcFromInputFile ();
		if (c == '\\')
			c = readUnicodeEscapeSequence (c);
	} while (isIdentChar (c));
	/* if readUnicodeEscapeSequence() read an escape sequence this is incorrect,
	 * as we should actually put back the whole escape sequence and not the
	 * decoded character.  However, it's not really worth the hassle as it can
	 * only happen if the input has an invalid escape sequence. */
	ungetcToInputFile (c);		/* unget non-identifier character */
}

static void parseTemplateString (vString *const string)
{
	int c;
	do
	{
		c = getcFromInputFile ();
		if (c == '`' || c == EOF)
			break;

		vStringPut (string, c);

		if (c == '\\')
		{
			c = getcFromInputFile();
			if (c != EOF)
				vStringPut(string, c);
		}
		else if (c == '$')
		{
			c = getcFromInputFile ();
			if (c != '{')
				ungetcToInputFile (c);
			else
			{
				int depth = 1;
				/* we need to use the real token machinery to handle strings,
				 * comments, regexes and whatnot */
				tokenInfo *token = newToken ();
				LastTokenType = TOKEN_UNDEFINED;
				vStringPut(string, c);
				do
				{
					readTokenFull (token, false, string);
					if (isType (token, TOKEN_OPEN_CURLY))
						depth++;
					else if (isType (token, TOKEN_CLOSE_CURLY))
						depth--;
				}
				while (! isType (token, TOKEN_EOF) && depth > 0);
				deleteToken (token);
			}
		}
	}
	while (c != EOF);
}

static void readTokenFull (tokenInfo *const token, bool include_newlines, vString *const repr)
{
	int c;
	int i;
	bool newline_encountered = false;

	/* if we've got a token held back, emit it */
	if (NextToken)
	{
		copyToken (token, NextToken, false);
		deleteToken (NextToken);
		NextToken = NULL;
		return;
	}

	token->type			= TOKEN_UNDEFINED;
	token->keyword		= KEYWORD_NONE;
	vStringClear (token->string);

getNextChar:
	i = 0;
	do
	{
		c = getcFromInputFile ();
		if (include_newlines && (c == '\r' || c == '\n'))
			newline_encountered = true;
		i++;
	}
	while (c == '\t' || c == ' ' || c == '\r' || c == '\n');

	token->lineNumber   = getInputLineNumber ();
	token->filePosition = getInputFilePosition ();

	if (repr && c != EOF)
	{
		if (i > 1)
			vStringPut (repr, ' ');
		vStringPut (repr, c);
	}

	switch (c)
	{
		case EOF: token->type = TOKEN_EOF;					break;
		case '(': token->type = TOKEN_OPEN_PAREN;			break;
		case ')': token->type = TOKEN_CLOSE_PAREN;			break;
		case ';': token->type = TOKEN_SEMICOLON;			break;
		case ',': token->type = TOKEN_COMMA;				break;
		case '.': token->type = TOKEN_PERIOD;				break;
		case ':': token->type = TOKEN_COLON;				break;
		case '{': token->type = TOKEN_OPEN_CURLY;			break;
		case '}': token->type = TOKEN_CLOSE_CURLY;			break;
		case '=': token->type = TOKEN_EQUAL_SIGN;			break;
		case '[': token->type = TOKEN_OPEN_SQUARE;			break;
		case ']': token->type = TOKEN_CLOSE_SQUARE;			break;

		case '+':
		case '-':
			{
				int d = getcFromInputFile ();
				if (d == c) /* ++ or -- */
					token->type = TOKEN_POSTFIX_OPERATOR;
				else
				{
					ungetcToInputFile (d);
					token->type = TOKEN_BINARY_OPERATOR;
				}
				break;
			}

		case '*':
			token->type = TOKEN_STAR;
			break;
		case '%':
		case '?':
		case '>':
		case '<':
		case '^':
		case '|':
		case '&':
			token->type = TOKEN_BINARY_OPERATOR;
			break;

		case '\'':
		case '"':
				  token->type = TOKEN_STRING;
				  parseString (token->string, c);
				  token->lineNumber = getInputLineNumber ();
				  token->filePosition = getInputFilePosition ();
				  if (repr)
				  {
					  vStringCat (repr, token->string);
					  vStringPut (repr, c);
				  }
				  break;

		case '`':
				  token->type = TOKEN_TEMPLATE_STRING;
				  parseTemplateString (token->string);
				  token->lineNumber = getInputLineNumber ();
				  token->filePosition = getInputFilePosition ();
				  if (repr)
				  {
					  vStringCat (repr, token->string);
					  vStringPut (repr, c);
				  }
				  break;

		case '/':
				  {
					  int d = getcFromInputFile ();
					  if ( (d != '*') &&		/* is this the start of a comment? */
							  (d != '/') )		/* is a one line comment? */
					  {
						  ungetcToInputFile (d);
						  switch (LastTokenType)
						  {
							  case TOKEN_CHARACTER:
							  case TOKEN_IDENTIFIER:
							  case TOKEN_STRING:
							  case TOKEN_TEMPLATE_STRING:
							  case TOKEN_CLOSE_CURLY:
							  case TOKEN_CLOSE_PAREN:
							  case TOKEN_CLOSE_SQUARE:
								  token->type = TOKEN_BINARY_OPERATOR;
								  break;

							  default:
								  token->type = TOKEN_REGEXP;
								  parseRegExp ();
								  token->lineNumber = getInputLineNumber ();
								  token->filePosition = getInputFilePosition ();
								  break;
						  }
					  }
					  else
					  {
						  if (repr) /* remove the / we added */
							  repr->buffer[--repr->length] = 0;
						  if (d == '*')
						  {
							  skipToCharacterInInputFile2('*', '/');
							  goto getNextChar;
						  }
						  else if (d == '/')	/* is this the start of a comment?  */
						  {
							  skipToCharacterInInputFile ('\n');
							  /* if we care about newlines, put it back so it is seen */
							  if (include_newlines)
								  ungetcToInputFile ('\n');
							  goto getNextChar;
						  }
					  }
					  break;
				  }

		case '#':
				  /* skip shebang in case of e.g. Node.js scripts */
				  if (token->lineNumber > 1)
					  token->type = TOKEN_UNDEFINED;
				  else if ((c = getcFromInputFile ()) != '!')
				  {
					  ungetcToInputFile (c);
					  token->type = TOKEN_UNDEFINED;
				  }
				  else
				  {
					  skipToCharacterInInputFile ('\n');
					  goto getNextChar;
				  }
				  break;

		case '\\':
				  c = readUnicodeEscapeSequence (c);
				  /* fallthrough */
		default:
				  if (! isIdentChar (c))
					  token->type = TOKEN_UNDEFINED;
				  else
				  {
					  parseIdentifier (token->string, c);
					  token->lineNumber = getInputLineNumber ();
					  token->filePosition = getInputFilePosition ();
					  token->keyword = lookupKeyword (vStringValue (token->string), Lang_js);
					  if (isKeyword (token, KEYWORD_NONE))
						  token->type = TOKEN_IDENTIFIER;
					  else
						  token->type = TOKEN_KEYWORD;
					  if (repr && vStringLength (token->string) > 1)
						  vStringCatS (repr, vStringValue (token->string) + 1);
				  }
				  break;
	}

	if (include_newlines && newline_encountered)
	{
		/* This isn't strictly correct per the standard, but following the
		 * real rules means understanding all statements, and that's not
		 * what the parser currently does.  What we do here is a guess, by
		 * avoiding inserting semicolons that would make the statement on
		 * the left or right obviously invalid.  Hopefully this should not
		 * have false negatives (e.g. should not miss insertion of a semicolon)
		 * but might have false positives (e.g. it will wrongfully emit a
		 * semicolon sometimes, i.e. for the newline in "foo\n(bar)").
		 * This should however be mostly harmless as we only deal with
		 * newlines in specific situations where we know a false positive
		 * wouldn't hurt too bad. */

		/* these already end a statement, so no need to duplicate it */
		#define IS_STMT_SEPARATOR(t) ((t) == TOKEN_SEMICOLON    || \
		                              (t) == TOKEN_EOF          || \
		                              (t) == TOKEN_COMMA        || \
		                              (t) == TOKEN_OPEN_CURLY)
		/* these cannot be the start or end of a statement */
		#define IS_BINARY_OPERATOR(t) ((t) == TOKEN_EQUAL_SIGN      || \
		                               (t) == TOKEN_COLON           || \
		                               (t) == TOKEN_PERIOD          || \
		                               (t) == TOKEN_STAR            || \
		                               (t) == TOKEN_BINARY_OPERATOR)

		if (! IS_STMT_SEPARATOR(LastTokenType) &&
		    ! IS_STMT_SEPARATOR(token->type) &&
		    ! IS_BINARY_OPERATOR(LastTokenType) &&
		    ! IS_BINARY_OPERATOR(token->type) &&
		    /* these cannot be followed by a semicolon */
		    ! (LastTokenType == TOKEN_OPEN_PAREN ||
		       LastTokenType == TOKEN_OPEN_SQUARE))
		{
			/* hold the token... */
			Assert (NextToken == NULL);
			NextToken = newToken ();
			copyToken (NextToken, token, false);

			/* ...and emit a semicolon instead */
			token->type		= TOKEN_SEMICOLON;
			token->keyword	= KEYWORD_NONE;
			vStringClear (token->string);
			if (repr)
				vStringPut (token->string, '\n');
		}

		#undef IS_STMT_SEPARATOR
		#undef IS_BINARY_OPERATOR
	}

	LastTokenType = token->type;
}

#ifdef JSCRIPT_DO_DEBUGGING
/* trace readTokenFull() */
static void readTokenFullDebug (tokenInfo *const token, bool include_newlines, vString *const repr)
{
	readTokenFull (token, include_newlines, repr);
	TRACE_PRINT("token '%s' of type %02x with scope '%s'",vStringValue(token->string),token->type, vStringValue(token->scope));
}
# define readTokenFull readTokenFullDebug
#endif

static void readToken (tokenInfo *const token)
{
	readTokenFull (token, false, NULL);
}

/*
 *	 Token parsing functions
 */

static void skipArgumentList (tokenInfo *const token, bool include_newlines, vString *const repr)
{
	int nest_level = 0;

	if (isType (token, TOKEN_OPEN_PAREN))	/* arguments? */
	{
		nest_level++;
		if (repr)
			vStringPut (repr, '(');
		while (nest_level > 0 && ! isType (token, TOKEN_EOF))
		{
			readTokenFull (token, false, repr);
			if (isType (token, TOKEN_OPEN_PAREN))
				nest_level++;
			else if (isType (token, TOKEN_CLOSE_PAREN))
				nest_level--;
			else if (isKeyword (token, KEYWORD_function))
				parseFunction (token);
		}
		readTokenFull (token, include_newlines, NULL);
	}
}

static void skipArrayList (tokenInfo *const token, bool include_newlines)
{
	int nest_level = 0;

	/*
	 * Handle square brackets
	 *	 var name[1]
	 * So we must check for nested open and closing square brackets
	 */

	if (isType (token, TOKEN_OPEN_SQUARE))	/* arguments? */
	{
		nest_level++;
		while (nest_level > 0 && ! isType (token, TOKEN_EOF))
		{
			readToken (token);
			if (isType (token, TOKEN_OPEN_SQUARE))
				nest_level++;
			else if (isType (token, TOKEN_CLOSE_SQUARE))
				nest_level--;
		}
		readTokenFull (token, include_newlines, NULL);
	}
}

static void skipQualifiedIdentifier (tokenInfo *const token)
{
	/* Skip foo.bar.baz */
	while (isType (token, TOKEN_IDENTIFIER))
	{
		readToken (token);
		if (isType (token, TOKEN_PERIOD))
			readToken (token);
		else
			break;
	}
}

static void addContext (tokenInfo* const parent, const tokenInfo* const child)
{
	if (vStringLength (parent->string) > 0)
	{
		vStringPut (parent->string, '.');
	}
	vStringCat (parent->string, child->string);
}

static void addToScope (tokenInfo* const token, int parent)
{
	token->scope = parent;
}

/*
 *	 Scanning functions
 */

static bool findCmdTerm (tokenInfo *const token, bool include_newlines,
                            bool include_commas, bool include_close_square)
{
	/*
	 * Read until we find either a semicolon or closing brace.
	 * Any nested braces will be handled within.
	 */
	while (! isType (token, TOKEN_SEMICOLON) &&
		   ! isType (token, TOKEN_CLOSE_CURLY) &&
		   ! (include_commas && isType (token, TOKEN_COMMA)) &&
		   ! (include_close_square && isType (token, TOKEN_CLOSE_SQUARE)) &&
		   ! isType (token, TOKEN_EOF))
	{
		/* Handle nested blocks */
		if ( isType (token, TOKEN_OPEN_CURLY))
		{
			parseBlock (token, CORK_NIL);
			readTokenFull (token, include_newlines, NULL);
		}
		else if ( isType (token, TOKEN_OPEN_PAREN) )
		{
			skipArgumentList(token, include_newlines, NULL);
		}
		else if ( isType (token, TOKEN_OPEN_SQUARE) )
		{
			skipArrayList(token, include_newlines);
		}
		else
		{
			readTokenFull (token, include_newlines, NULL);
		}
	}

	return isType (token, TOKEN_SEMICOLON);
}

static void parseSwitch (tokenInfo *const token)
{
	/*
	 * switch (expression) {
	 * case value1:
	 *	   statement;
	 *	   break;
	 * case value2:
	 *	   statement;
	 *	   break;
	 * default : statement;
	 * }
	 */

	readToken (token);

	if (isType (token, TOKEN_OPEN_PAREN))
	{
		skipArgumentList(token, false, NULL);
	}

	if (isType (token, TOKEN_OPEN_CURLY))
	{
		parseBlock (token, CORK_NIL);
	}
}

static bool parseLoop (tokenInfo *const token)
{
	/*
	 * Handles these statements
	 *	   for (x=0; x<3; x++)
	 *		   document.write("This text is repeated three times<br>");
	 *
	 *	   for (x=0; x<3; x++)
	 *	   {
	 *		   document.write("This text is repeated three times<br>");
	 *	   }
	 *
	 *	   while (number<5){
	 *		   document.write(number+"<br>");
	 *		   number++;
	 *	   }
	 *
	 *	   do{
	 *		   document.write(number+"<br>");
	 *		   number++;
	 *	   }
	 *	   while (number<5);
	 */
	bool is_terminated = true;

	if (isKeyword (token, KEYWORD_for) || isKeyword (token, KEYWORD_while))
	{
		readToken(token);

		if (isType (token, TOKEN_OPEN_PAREN))
		{
			skipArgumentList(token, false, NULL);
		}

		if (isType (token, TOKEN_OPEN_CURLY))
		{
			parseBlock (token, CORK_NIL);
		}
		else
		{
			is_terminated = parseLine(token, false);
		}
	}
	else if (isKeyword (token, KEYWORD_do))
	{
		readToken(token);

		if (isType (token, TOKEN_OPEN_CURLY))
		{
			parseBlock (token, CORK_NIL);
		}
		else
		{
			is_terminated = parseLine(token, false);
		}

		if (is_terminated)
			readToken(token);

		if (isKeyword (token, KEYWORD_while))
		{
			readToken(token);

			if (isType (token, TOKEN_OPEN_PAREN))
			{
				skipArgumentList(token, true, NULL);
			}
			if (! isType (token, TOKEN_SEMICOLON))
			{
				/* oddly enough, `do {} while (0) var foo = 42` is perfectly
				 * valid JS, so explicitly handle the remaining of the line
				 * for the sake of the root scope handling (as parseJsFile()
				 * always advances a token not to ever get stuck) */
				is_terminated = parseLine(token, false);
			}
		}
	}

	return is_terminated;
}

static bool parseIf (tokenInfo *const token)
{
	bool read_next_token = true;
	/*
	 * If statements have two forms
	 *	   if ( ... )
	 *		   one line;
	 *
	 *	   if ( ... )
	 *		  statement;
	 *	   else
	 *		  statement
	 *
	 *	   if ( ... ) {
	 *		  multiple;
	 *		  statements;
	 *	   }
	 *
	 *
	 *	   if ( ... ) {
	 *		  return elem
	 *	   }
	 *
	 *     This example if correctly written, but the
	 *     else contains only 1 statement without a terminator
	 *     since the function finishes with the closing brace.
	 *
     *     function a(flag){
     *         if(flag)
     *             test(1);
     *         else
     *             test(2)
     *     }
	 *
	 * TODO:  Deal with statements that can optional end
	 *		  without a semi-colon.  Currently this messes up
	 *		  the parsing of blocks.
	 *		  Need to somehow detect this has happened, and either
	 *		  backup a token, or skip reading the next token if
	 *		  that is possible from all code locations.
	 *
	 */

	readToken (token);

	if (isKeyword (token, KEYWORD_if))
	{
		/*
		 * Check for an "else if" and consume the "if"
		 */
		readToken (token);
	}

	if (isType (token, TOKEN_OPEN_PAREN))
	{
		skipArgumentList(token, false, NULL);
	}

	if (isType (token, TOKEN_OPEN_CURLY))
	{
		parseBlock (token, CORK_NIL);
	}
	else
	{
		/* The next token should only be read if this statement had its own
		 * terminator */
		read_next_token = findCmdTerm (token, true, false, false);
	}
	return read_next_token;
}

static void parseFunction (tokenInfo *const token)
{
	TRACE_ENTER();

	tokenInfo *const name = newToken ();
	vString *const signature = vStringNew ();
	bool is_class = false;
	bool is_generator = false;
	bool is_anonymous = false;
	/*
	 * This deals with these formats
	 *	   function validFunctionTwo(a,b) {}
	 *	   function * generator(a,b) {}
	 */

	copyToken (name, token, true);
	readToken (name);
	if (isType (name, TOKEN_STAR))
	{
		is_generator = true;
		readToken (name);
	}
	if (isType (name, TOKEN_OPEN_PAREN))
	{
		/* anonymous function */
		copyToken (token, name, false);
		anonGenerate (name->string, "AnonymousFunction", JSTAG_FUNCTION);
		is_anonymous = true;
	}
	else if (!isType (name, TOKEN_IDENTIFIER))
		goto cleanUp;
	else
		readToken (token);

	while (isType (token, TOKEN_PERIOD))
	{
		readToken (token);
		if (! isType(token, TOKEN_KEYWORD))
		{
			addContext (name, token);
			readToken (token);
		}
	}

	if ( isType (token, TOKEN_OPEN_PAREN) )
		skipArgumentList(token, false, signature);

	if ( isType (token, TOKEN_OPEN_CURLY) )
	{
		int p = makeSimplePlaceholder (name->string), q;
		is_class = parseBlock (token, p);
		if ( is_class )
			q = makeClassTagCommon (name, signature, NULL, is_anonymous);
		else
			q = makeFunctionTagCommon (name, signature, is_generator, is_anonymous);
		patchScopeFieldOfEntriesBetween (p, countEntryInCorkQueue(), q);
	}

	findCmdTerm (token, false, false, false);

 cleanUp:
	vStringDelete (signature);
	deleteToken (name);

	TRACE_LEAVE();
}

/* Parses a block surrounded by curly braces.
 * @p parentScope is the scope name for this block, or NULL for unnamed scopes */
static bool parseBlock (tokenInfo *const token, int parentScope)
{
	TRACE_ENTER();

	bool is_class = false;
	bool read_next_token = true;
	int saveScope;

	saveScope = token->scope;
	if (parentScope != CORK_NIL)
	{
		addToScope (token, parentScope);
		token->nestLevel++;
	}

	/*
	 * Make this routine a bit more forgiving.
	 * If called on an open_curly advance it
	 */
	if (isType (token, TOKEN_OPEN_CURLY))
		readToken(token);

	if (! isType (token, TOKEN_CLOSE_CURLY))
	{
		/*
		 * Read until we find the closing brace,
		 * any nested braces will be handled within
		 */
		do
		{
			read_next_token = true;
			if (isKeyword (token, KEYWORD_this))
			{
				/*
				 * Means we are inside a class and have found
				 * a class, not a function
				 */
				is_class = true;

				/*
				 * Ignore the remainder of the line
				 * findCmdTerm(token);
				 */
				read_next_token = parseLine (token, is_class);
			}
			else if (isKeyword (token, KEYWORD_var) ||
					 isKeyword (token, KEYWORD_let) ||
					 isKeyword (token, KEYWORD_const))
			{
				/*
				 * Potentially we have found an inner function.
				 * Set something to indicate the scope
				 */
				read_next_token = parseLine (token, is_class);
			}
			else if (isType (token, TOKEN_OPEN_CURLY))
			{
				/* Handle nested blocks */
				parseBlock (token, CORK_NIL);
			}
			else
			{
				/*
				 * It is possible for a line to have no terminator
				 * if the following line is a closing brace.
				 * parseLine will detect this case and indicate
				 * whether we should read an additional token.
				 */
				read_next_token = parseLine (token, is_class);
			}

			/*
			 * Always read a new token unless we find a statement without
			 * a ending terminator
			 */
			if( read_next_token )
				readToken(token);

			/*
			 * If we find a statement without a terminator consider the
			 * block finished, otherwise the stack will be off by one.
			 */
		} while (! isType (token, TOKEN_EOF) &&
				 ! isType (token, TOKEN_CLOSE_CURLY) && read_next_token);
	}

	token->scope = saveScope;
	if (parentScope != CORK_NIL)
		token->nestLevel--;

	TRACE_LEAVE();

	return is_class;
}

static bool parseMethods (tokenInfo *const token, int classIndex,
                          const bool is_es6_class)
{
	TRACE_ENTER_TEXT("token is '%s' of type %s in classToken '%s' of kind '%s' (es6: %s)",
					 vStringValue(token->string), tokenTypeName (token->type),
					 classIndex == CORK_NIL? "": getEntryInCorkQueue (classIndex)->name,
					 classIndex == CORK_NIL? "": kindName (getEntryInCorkQueue (classIndex)->kindIndex),
					 is_es6_class? "yes": "no");

	/*
	 * When making a tag for `name', its core index is stored to
	 * `indexForName'. The value stored to `indexForName' is valid
	 * till the value for `name' is updated. If the value for `name'
	 * is changed, `indexForName' is reset to CORK_NIL.
	 */
	tokenInfo *const name = newToken ();
	int indexForName = CORK_NIL;
	bool has_methods = false;
	int saveScope;

	saveScope = token->scope;
	addToScope (token, classIndex);

	/*
	 * This deals with these formats
	 *	   validProperty  : 2,
	 *	   validMethod    : function(a,b) {}
	 *	   'validMethod2' : function(a,b) {}
     *     container.dirtyTab = {'url': false, 'title':false, 'snapshot':false, '*': false}
	 *     get prop() {}
	 *     set prop(val) {}
     *
     * ES6 methods:
     *     property(...) {}
     *     *generator() {}
     *
     * ES6 computed name:
     *     [property]() {}
     *     get [property]() {}
     *     set [property]() {}
     *     *[generator]() {}
	 */

	do
	{
		bool is_setter = false;
		bool is_getter = false;

		readToken (token);
		if (isType (token, TOKEN_CLOSE_CURLY))
		{
			goto cleanUp;
		}

		if (isKeyword (token, KEYWORD_async))
			readToken (token);
		else if (isType(token, TOKEN_KEYWORD) && isKeyword (token, KEYWORD_get))
		{
			is_getter = true;
			readToken (token);
		}
		else if (isType(token, TOKEN_KEYWORD) && isKeyword (token, KEYWORD_set))
		{
			is_setter = true;
			readToken (token);
		}

		if (! isType (token, TOKEN_KEYWORD) &&
		    ! isType (token, TOKEN_SEMICOLON))
		{
			bool is_generator = false;
			bool is_shorthand = false; /* ES6 shorthand syntax */
			bool is_computed_name = false; /* ES6 computed property name */
			bool is_dynamic_prop = false;
			vString *dprop = NULL; /* is_computed_name is true but
									* the name is not represnted in
									* a string literal. The expressions
									* go this string. */

			if (isType (token, TOKEN_STAR)) /* shorthand generator */
			{
				is_generator = true;
				readToken (token);
			}

			if (isType (token, TOKEN_OPEN_SQUARE))
			{
				is_computed_name = true;
				dprop = vStringNewInit ("[");
				readTokenFull (token, false, dprop);
			}

			copyToken(name, token, true);
			indexForName = CORK_NIL;
			if (is_computed_name && ! isType (token, TOKEN_STRING))
				is_dynamic_prop = true;

			readTokenFull (token, false, dprop);

			if (is_computed_name)
			{
				int depth = 1;
				do
				{
					if (isType (token, TOKEN_CLOSE_SQUARE))
						depth--;
					else
					{
						is_dynamic_prop = true;
						if (isType (token, TOKEN_OPEN_SQUARE))
							depth++;
					}
					readTokenFull (token, false, (is_dynamic_prop && depth != 0)? dprop: NULL);
				} while (! isType (token, TOKEN_EOF) && depth > 0);
			}

			if (is_dynamic_prop)
			{
				injectDynamicName (name, dprop);
				indexForName = CORK_NIL;
				dprop = NULL;
			}
			else
				vStringDelete (dprop);

			is_shorthand = isType (token, TOKEN_OPEN_PAREN);
			if ( isType (token, TOKEN_COLON) || is_shorthand )
			{
				if (! is_shorthand)
				{
					readToken (token);
					if (isKeyword (token, KEYWORD_async))
						readToken (token);
				}
				if ( is_shorthand || isKeyword (token, KEYWORD_function) )
				{
					TRACE_PRINT("Seems to be a function or shorthand");
					vString *const signature = vStringNew ();

					if (! is_shorthand)
					{
						readToken (token);
						if (isType (token, TOKEN_STAR))
						{
							/* generator: 'function' '*' '(' ... ')' '{' ... '}' */
							is_generator = true;
							readToken (token);
						}
					}
					if ( isType (token, TOKEN_OPEN_PAREN) )
					{
						skipArgumentList(token, false, signature);
					}

					if (isType (token, TOKEN_OPEN_CURLY))
					{
						has_methods = true;

						int kind = JSTAG_METHOD;
						if (is_generator)
							kind = JSTAG_GENERATOR;
						else if (is_getter)
							kind = JSTAG_GETTER;
						else if (is_setter)
							kind = JSTAG_SETTER;

						indexForName = makeJsTag (name, kind, signature, NULL);
						parseBlock (token, indexForName);

						/*
						 * If we aren't parsing an ES6 class (for which there
						 * is no mandatory separators), read to the closing
						 * curly, check next token, if a comma, we must loop
						 * again.
						 */
						if (! is_es6_class)
							readToken (token);
					}

					vStringDelete (signature);
				}
				else if (! is_es6_class)
				{
						int p = CORK_NIL, q = CORK_NIL;
						bool has_child_methods = false;

						/* skip whatever is the value */
						while (! isType (token, TOKEN_COMMA) &&
						       ! isType (token, TOKEN_CLOSE_CURLY) &&
						       ! isType (token, TOKEN_EOF))
						{
							if (isType (token, TOKEN_OPEN_CURLY))
							{
								/* Recurse to find child properties/methods */
								p = makeSimplePlaceholder (name->string);
								has_child_methods = parseMethods (token, p, false);
								readToken (token);
							}
							else if (isType (token, TOKEN_OPEN_PAREN))
							{
								skipArgumentList (token, false, NULL);
							}
							else if (isType (token, TOKEN_OPEN_SQUARE))
							{
								skipArrayList (token, false);
							}
							else
							{
								readToken (token);
							}
						}

						has_methods = true;
						if (has_child_methods)
							q = makeJsTag (name, JSTAG_CLASS, NULL, NULL);
						else
							q = makeJsTag (name, JSTAG_PROPERTY, NULL, NULL);
						if (p != CORK_NIL)
							patchScopeFieldOfEntriesBetween (p, countEntryInCorkQueue(), q);
						indexForName = q;
				}
			}
		}
	} while ( isType(token, TOKEN_COMMA) ||
	          ( is_es6_class && ! isType(token, TOKEN_EOF) ) );

	TRACE_PRINT("Finished parsing methods");

	findCmdTerm (token, false, false, false);

cleanUp:
	token->scope = saveScope;
	deleteToken (name);

	TRACE_LEAVE_TEXT("found method(s): %s", has_methods? "yes": "no");

	return has_methods;
}

static bool parseES6Class (tokenInfo *const token, const tokenInfo *targetName)
{
	TRACE_ENTER();

	tokenInfo * className = newToken ();
	vString *inheritance = NULL;
	bool is_anonymous = true;

	copyToken (className, token, true);
	readToken (className);

	/* optional name */
	if (isType (className, TOKEN_IDENTIFIER))
	{
		readToken (token);
		is_anonymous = false;
	}
	else
	{
		copyToken (token, className, true);
		/* We create a fake name so we have a scope for the members */
		if (! targetName)
			anonGenerate (className->string, "AnonymousClass", JSTAG_CLASS);
	}

	if (! targetName)
		targetName = className;

	if (isKeyword (token, KEYWORD_extends))
		inheritance = vStringNew ();

	/* skip inheritance info */
	while (! isType (token, TOKEN_OPEN_CURLY) &&
	       ! isType (token, TOKEN_EOF) &&
	       ! isType (token, TOKEN_SEMICOLON))
		readTokenFull (token, false, inheritance);

	/* remove the last added token (here we assume it's one char, "{" or ";" */
	if (inheritance && vStringLength (inheritance) > 0 &&
	    ! isType (token, TOKEN_EOF))
	{
		vStringChop (inheritance);
		vStringStripTrailing (inheritance);
		vStringStripLeading (inheritance);
	}

	TRACE_PRINT("Emitting tag for class '%s'", vStringValue(targetName->string));

	int r = makeJsTagCommon (targetName, JSTAG_CLASS, NULL, inheritance,
					 (is_anonymous && (targetName == className)));

	if (! is_anonymous && targetName != className)
	{
		/* FIXME: what to do with the secondary name?  It's local to the
		 *        class itself, so not very useful... let's hope people
		 *        don't give it another name than the target in case of
		 *        	var MyClass = class MyClassSecondaryName { ... }
		 *        I guess it could be an alias to MyClass, or duplicate it
		 *        altogether, not sure. */
		makeJsTag (className, JSTAG_CLASS, NULL, inheritance);
	}

	if (inheritance)
		vStringDelete (inheritance);

	if (isType (token, TOKEN_OPEN_CURLY))
		parseMethods (token, r, true);

	deleteToken (className);

	TRACE_LEAVE();
	return true;
}

/* Forms the left side of desturctual assignments
 *
 * T = var|let|const
 *
 * ---------------------------------------------------------
 * T [] = [];
 * T [a] = [0];
 * T [b,c] = [0];
 * T [,d] = [0];
 * T [e,...f] = [0];
 * T [,...g] = [0];
 * T [...h] = [0];
 * T {i, j} = ...;
 * T {k=DONT_TAG_ME0,l} = ...;
 * T {DONT_TAG_ME1: m, DONT_TAG_ME2: n = DONT_TAG_ME3} = ...;
 * let DONT_TAG_ME_KEY = 'z'; T {[DONT_TAG_ME_KEY]: o} = ...;
 * T {'DONT_TAG_ME_KEYLIT': p } = ...;
 * T [,{ q }] = ...;
 * T {DONT_TAG_ME_KEYLIT: [r,s]} = ;
 */
#define MAX_DESTRUCTUAL_ASSIGNMENTS_DEPTH 32
static void parseLeftSideOfDestructualAssignment (tokenInfo * const token, int kindIndex, int depth)
{
	if (depth > MAX_DESTRUCTUAL_ASSIGNMENTS_DEPTH)
		return;

	tokenInfo *const name = newToken ();
	int terminator = isType (token, TOKEN_OPEN_CURLY)? TOKEN_CLOSE_CURLY: TOKEN_CLOSE_SQUARE;
	int dont_read = 0;

	while (!(isType(token, terminator) || isType(token, TOKEN_EOF)))
	{
		if (!dont_read)
			readToken(token);
		else
			dont_read--;

		bool started_from_id = isType(token, TOKEN_IDENTIFIER);
		if (started_from_id
			|| isType (token, TOKEN_STRING)
			|| (terminator == TOKEN_CLOSE_CURLY && isType(token, TOKEN_OPEN_SQUARE)))
		{
			if (started_from_id)
			{
				copyToken(name, token, true);
				readToken(token);
			}
			else
			{
				/*
				 * let DONT_TAG_ME_KEY = 'z'; T {[DONT_TAG_ME_KEY]: o} = ...;
				 */
				skipArrayList (token, true);
			}

			if (isType (token, TOKEN_COLON))
			{
				readToken(token);
				if (isType(token, TOKEN_IDENTIFIER))
				{
					/*
					 * T {DONT_TAG_ME1: m, DONT_TAG_ME2: n = DONT_TAG_ME3} = ...;
					 */
					if ((kindIndex != KIND_GHOST_INDEX)
						&& (!vStringIsEmpty (token->string)))
						makeJsTag (token, kindIndex, NULL, NULL);
				}
				else if (isType(token, TOKEN_OPEN_CURLY) || isType(token, TOKEN_OPEN_SQUARE))
				{
					/*
					 * T {DONT_TAG_ME_KEYLIT: [r,s]} = ;
					 */
					parseLeftSideOfDestructualAssignment (token, kindIndex, ++depth);
				}
				else
				{
					/* Unexpected input */
					dont_read++;
				}
			}
			else if (started_from_id)
			{
				/*
				 * T [] = [];
				 * T [a] = [0];
				 * T [b,c] = [0];
				 * T [,d] = [0];
				 * T [e,...f] = [0];
				 * T [,...g] = [0];
				 * T [...h] = [0];
				 * T {i, j} = ...;
				 */

				if ((kindIndex != KIND_GHOST_INDEX)
					&& (!vStringIsEmpty (name->string)))
					makeJsTag (name, kindIndex, NULL, NULL);
				dont_read++;
			}
		}
		else if (isType(token, TOKEN_EQUAL_SIGN))
		{
			/*
			 * T {k=DONT_TAG_ME0,l} = ...;
			 */
			/* DONT_TAG_ME0 can be an expresion.
			   Skip to the next `,' or `}'. */
			readToken(token);
			findCmdTerm (token, true, true, true);
		}
		else if (isType(token, TOKEN_OPEN_CURLY) || isType(token, TOKEN_OPEN_SQUARE))
		{
			parseLeftSideOfDestructualAssignment (token, kindIndex, ++depth);
		}
	}
	deleteToken (name);
}

static bool parseStatement (tokenInfo *const token, bool is_inside_class)
{
	TRACE_ENTER_TEXT("is_inside_class: %s", is_inside_class? "yes": "no");

	tokenInfo *const name = newToken ();
	int indexForName = CORK_NIL;
	tokenInfo *const secondary_name = newToken ();
	tokenInfo *const method_body_token = newToken ();
	int saveScope;
	bool is_class = false;
	bool is_var = false;
	bool is_const = false;
	bool is_terminated = true;
	bool is_global = false;
	bool has_methods = false;

	saveScope = token->scope;
	/*
	 * Functions can be named or unnamed.
	 * This deals with these formats:
	 * Function
	 *	   validFunctionOne = function(a,b) {}
	 *	   testlib.validFunctionFive = function(a,b) {}
	 *	   var innerThree = function(a,b) {}
	 *	   var innerFour = (a,b) {}
	 *	   var D2 = secondary_fcn_name(a,b) {}
	 *	   var D3 = new Function("a", "b", "return a+b;");
	 * Class
	 *	   testlib.extras.ValidClassOne = function(a,b) {
	 *		   this.a = a;
	 *	   }
	 * Class Methods
	 *	   testlib.extras.ValidClassOne.prototype = {
	 *		   'validMethodOne' : function(a,b) {},
	 *		   'validMethodTwo' : function(a,b) {}
	 *	   }
     *     ValidClassTwo = function ()
     *     {
     *         this.validMethodThree = function() {}
     *         // unnamed method
     *         this.validMethodFour = () {}
     *     }
	 *	   Database.prototype.validMethodThree = Database_getTodaysDate;
	 */

	if ( is_inside_class )
		is_class = true;
	/*
	 * var can precede an inner function
	 */
	if ( isKeyword(token, KEYWORD_var) ||
		 isKeyword(token, KEYWORD_let) ||
		 isKeyword(token, KEYWORD_const) )
	{
		TRACE_PRINT("var/let/const case");
		is_const = isKeyword(token, KEYWORD_const);
		/*
		 * Only create variables for global scope
		 */
		if ( token->nestLevel == 0 )
		{
			is_global = true;
		}
		readToken(token);
		if (isType (token, TOKEN_OPEN_CURLY) ||
			isType (token, TOKEN_OPEN_SQUARE))
		{
			int kindIndex;
			if (!is_global)
				kindIndex = KIND_GHOST_INDEX;
			else if (is_const)
				kindIndex = JSTAG_CONSTANT;
			else
				kindIndex = JSTAG_VARIABLE;
			parseLeftSideOfDestructualAssignment (token, kindIndex, 0);
		}
	}

nextVar:
	if ( isKeyword(token, KEYWORD_this) )
	{
		TRACE_PRINT("found 'this' keyword");

		readToken(token);
		if (isType (token, TOKEN_PERIOD))
		{
			readToken(token);
		}
	}

	copyToken(name, token, true);
	indexForName = CORK_NIL;
	TRACE_PRINT("name becomes '%s' of type %s",
				vStringValue(token->string), tokenTypeName (token->type));

	while (! isType (token, TOKEN_CLOSE_CURLY) &&
	       ! isType (token, TOKEN_SEMICOLON)   &&
	       ! isType (token, TOKEN_EQUAL_SIGN)  &&
	       ! isType (token, TOKEN_COMMA)       &&
	       ! isType (token, TOKEN_EOF))
	{
		if (isType (token, TOKEN_OPEN_CURLY))
			parseBlock (token, CORK_NIL);

		/* Potentially the name of the function */
		if (isType (token, TOKEN_PERIOD))
		{
			/*
			 * Cannot be a global variable is it has dot references in the name
			 */
			is_global = false;
			/* Assume it's an assignment to a global name (e.g. a class) using
			 * its fully qualified name, so strip the scope.
			 * FIXME: resolve the scope so we can make more than an assumption. */
			token->scope = CORK_NIL;
			name->scope = CORK_NIL;
			do
			{
				readToken (token);
				if (! isType(token, TOKEN_KEYWORD))
				{
					if ( is_class )
					{
						addToScope(token, indexForName);
					}
					else
					{
						addContext (name, token);
						indexForName = CORK_NIL;
					}

					readToken (token);
				}
				else if ( isKeyword(token, KEYWORD_prototype) )
				{
					/*
					 * When we reach the "prototype" tag, we infer:
					 *     "BindAgent" is a class
					 *     "build"     is a method
					 *
					 * function BindAgent( repeatableIdName, newParentIdName ) {
					 * }
					 *
					 * CASE 1
					 * Specified function name: "build"
					 *     BindAgent.prototype.build = function( mode ) {
					 *     	  maybe parse nested functions
					 *     }
					 *
					 * CASE 2
					 * Prototype listing
					 *     ValidClassOne.prototype = {
					 *         'validMethodOne' : function(a,b) {},
					 *         'validMethodTwo' : function(a,b) {}
					 *     }
					 *
					 */
					if (! ( isType (name, TOKEN_IDENTIFIER)
						|| isType (name, TOKEN_STRING) ) )
						/*
						 * Unexpected input. Try to reset the parsing.
						 *
						 * TOKEN_STRING is acceptable. e.g.:
						 * -----------------------------------
						 * "a".prototype = function( mode ) {}
						 */
						goto cleanUp;

					indexForName = makeClassTag (name, NULL, NULL);
					is_class = true;

					/*
					 * There should a ".function_name" next.
					 */
					readToken (token);
					if (isType (token, TOKEN_PERIOD))
					{
						/*
						 * Handle CASE 1
						 */
						readToken (token);
						if (! isType(token, TOKEN_KEYWORD))
						{
							vString *const signature = vStringNew ();

							addToScope(token, indexForName);

							copyToken (method_body_token, token, true);
							readToken (method_body_token);

							while (! isType (method_body_token, TOKEN_SEMICOLON) &&
							       ! isType (method_body_token, TOKEN_CLOSE_CURLY) &&
							       ! isType (method_body_token, TOKEN_OPEN_CURLY) &&
							       ! isType (method_body_token, TOKEN_EOF))
							{
								if ( isType (method_body_token, TOKEN_OPEN_PAREN) )
									skipArgumentList(method_body_token, false,
													 vStringLength (signature) == 0 ? signature : NULL);
								else
									readToken (method_body_token);
							}

							int r = makeJsTag (token, JSTAG_METHOD, signature, NULL);
							vStringDelete (signature);

							if ( isType (method_body_token, TOKEN_OPEN_CURLY))
							{
								parseBlock (method_body_token, r);
								is_terminated = true;
							}
							else
								is_terminated = isType (method_body_token, TOKEN_SEMICOLON);
							goto cleanUp;
						}
					}
					else if (isType (token, TOKEN_EQUAL_SIGN))
					{
						readToken (token);
						if (isType (token, TOKEN_OPEN_CURLY))
						{
							/*
							 * Handle CASE 2
							 *
							 * Creates tags for each of these class methods
							 *     ValidClassOne.prototype = {
							 *         'validMethodOne' : function(a,b) {},
							 *         'validMethodTwo' : function(a,b) {}
							 *     }
							 */
							parseMethods(token, indexForName, false);
							/*
							 * Find to the end of the statement
							 */
							findCmdTerm (token, false, false, false);
							is_terminated = true;
							goto cleanUp;
						}
					}
				}
				else
					readToken (token);
			} while (isType (token, TOKEN_PERIOD));
		}
		else
			readTokenFull (token, true, NULL);

		if ( isType (token, TOKEN_OPEN_PAREN) )
			skipArgumentList(token, false, NULL);

		if ( isType (token, TOKEN_OPEN_SQUARE) )
			skipArrayList(token, false);

		/*
		if ( isType (token, TOKEN_OPEN_CURLY) )
		{
			is_class = parseBlock (token, name->string);
		}
		*/
	}

	if ( isType (token, TOKEN_CLOSE_CURLY) )
	{
		/*
		 * Reaching this section without having
		 * processed an open curly brace indicates
		 * the statement is most likely not terminated.
		 */
		is_terminated = false;
		goto cleanUp;
	}

	if ( isType (token, TOKEN_SEMICOLON) ||
	     isType (token, TOKEN_EOF) ||
	     isType (token, TOKEN_COMMA) )
	{
		/*
		 * Only create variables for global scope
		 */
		if ( token->nestLevel == 0 && is_global )
		{
			/*
			 * Handles this syntax:
			 *	   var g_var2;
			 */
			indexForName = makeJsTag (name, is_const ? JSTAG_CONSTANT : JSTAG_VARIABLE, NULL, NULL);
		}
		/*
		 * Statement has ended.
		 * This deals with calls to functions, like:
		 *     alert(..);
		 */
		if (isType (token, TOKEN_COMMA))
		{
			readToken (token);
			goto nextVar;
		}
		goto cleanUp;
	}

	if ( isType (token, TOKEN_EQUAL_SIGN) )
	{
		int parenDepth = 0;

		readToken (token);

		/* rvalue might be surrounded with parentheses */
		while (isType (token, TOKEN_OPEN_PAREN))
		{
			parenDepth++;
			readToken (token);
		}

		if (isKeyword (token, KEYWORD_async))
			readToken (token);

		if ( isKeyword (token, KEYWORD_function) )
		{
			vString *const signature = vStringNew ();
			bool is_generator = false;

			readToken (token);
			if (isType (token, TOKEN_STAR))
			{
				is_generator = true;
				readToken (token);
			}

			if (! isType (token, TOKEN_KEYWORD) &&
			    ! isType (token, TOKEN_OPEN_PAREN))
			{
				/*
				 * Functions of this format:
				 *	   var D2A = function theAdd(a, b)
				 *	   {
				 *		  return a+b;
				 *	   }
				 * Are really two separate defined functions and
				 * can be referenced in two ways:
				 *	   alert( D2A(1,2) );			  // produces 3
				 *	   alert( theAdd(1,2) );		  // also produces 3
				 * So it must have two tags:
				 *	   D2A
				 *	   theAdd
				 * Save the reference to the name for later use, once
				 * we have established this is a valid function we will
				 * create the secondary reference to it.
				 */
				copyToken(secondary_name, token, true);
				readToken (token);
			}

			if ( isType (token, TOKEN_OPEN_PAREN) )
				skipArgumentList(token, false, signature);

			if (isType (token, TOKEN_OPEN_CURLY))
			{
				/*
				 * This will be either a function or a class.
				 * We can only determine this by checking the body
				 * of the function.  If we find a "this." we know
				 * it is a class, otherwise it is a function.
				 */
				if ( is_inside_class )
				{
					indexForName = makeJsTag (name, is_generator ? JSTAG_GENERATOR : JSTAG_METHOD, signature, NULL);
					if ( vStringLength(secondary_name->string) > 0 )
						makeFunctionTag (secondary_name, signature, is_generator);
					parseBlock (token, indexForName);
				}
				else
				{
					if (! ( isType (name, TOKEN_IDENTIFIER)
					     || isType (name, TOKEN_STRING)
					     || isType (name, TOKEN_KEYWORD) ) )
					{
						/* Unexpected input. Try to reset the parsing. */
						TRACE_PRINT("Unexpected input, trying to reset");
						vStringDelete (signature);
						goto cleanUp;
					}

					int p = makeSimplePlaceholder (name->string), q;
					is_class = parseBlock (token, p);
					if ( is_class )
						q = makeClassTag (name, signature, NULL);
					else
						q = makeFunctionTag (name, signature, is_generator);
					patchScopeFieldOfEntriesBetween (p, countEntryInCorkQueue(), q);
					indexForName = q;

					if ( vStringLength(secondary_name->string) > 0 )
						makeFunctionTag (secondary_name, signature, is_generator);
				}
			}

			vStringDelete (signature);
		}
		else if (isKeyword (token, KEYWORD_class))
		{
			is_terminated = parseES6Class (token, name);
		}
		else if (isType (token, TOKEN_OPEN_CURLY))
		{
			/*
			 * Creates tags for each of these class methods
			 *     ValidClassOne.prototype = {
			 *         'validMethodOne' : function(a,b) {},
			 *         'validMethodTwo' : function(a,b) {}
			 *     }
			 * Or checks if this is a hash variable.
			 *     var z = {};
			 */
			bool anonClass = vStringIsEmpty (name->string);
			if (anonClass)
			{
				anonGenerate (name->string, "AnonymousClass", JSTAG_CLASS);
				indexForName = CORK_NIL;
			}
			int p = makeSimplePlaceholder (name->string), q;
			has_methods = parseMethods(token, p, false);
			if (has_methods)
			{
				q = makeJsTagCommon (name, JSTAG_CLASS, NULL, NULL, anonClass);
				patchScopeFieldOfEntriesBetween (p, countEntryInCorkQueue(), q);
				indexForName = q;
			}
			else
			{
				/*
				 * Only create variables for global scope
				 */
				if ( token->nestLevel == 0 && is_global )
				{
					/*
					 * A pointer can be created to the function.
					 * If we recognize the function/class name ignore the variable.
					 * This format looks identical to a variable definition.
					 * A variable defined outside of a block is considered
					 * a global variable:
					 *	   var g_var1 = 1;
					 *	   var g_var2;
					 * This is not a global variable:
					 *	   var g_var = function;
					 * This is a global variable:
					 *	   var g_var = different_var_name;
					 */
					if (isAlreadyTaggedForToken(JSTAG_FUNCTION, token, &indexForName))
						;
					else if (isAlreadyTaggedForToken(JSTAG_CLASS, token, &indexForName))
						;
					else
						indexForName = makeJsTag (name, is_const ? JSTAG_CONSTANT : JSTAG_VARIABLE, NULL, NULL);
				}
			}
			/* Here we should be at the end of the block, on the close curly.
			 * If so, read the next token not to confuse that close curly with
			 * the end of the current statement. */
			if (isType (token, TOKEN_CLOSE_CURLY))
			{
				readTokenFull(token, true, NULL);
				is_terminated = isType (token, TOKEN_SEMICOLON);
			}
		}
		else if (isKeyword (token, KEYWORD_new))
		{
			readToken (token);
			is_var = isType (token, TOKEN_IDENTIFIER);
			if ( isKeyword (token, KEYWORD_function) ||
					isKeyword (token, KEYWORD_capital_function) ||
					isKeyword (token, KEYWORD_capital_object) ||
					is_var )
			{
				if ( isKeyword (token, KEYWORD_capital_object) )
					is_class = true;

				if (is_var)
					skipQualifiedIdentifier (token);
				else
					readToken (token);

				if ( isType (token, TOKEN_OPEN_PAREN) )
					skipArgumentList(token, true, NULL);

				if (isType (token, TOKEN_SEMICOLON))
				{
					if ( token->nestLevel == 0 )
					{
						if ( is_var )
						{
							indexForName = makeJsTag (name, is_const ? JSTAG_CONSTANT : JSTAG_VARIABLE, NULL, NULL);
						}
						else if ( is_class )
						{
							indexForName = makeClassTag (name, NULL, NULL);
						}
						else
						{
							/* FIXME: we cannot really get a meaningful
							 * signature from a `new Function()` call,
							 * so for now just don't set any */
							indexForName = makeFunctionTag (name, NULL, false);
						}
					}
				}
				else if (isType (token, TOKEN_CLOSE_CURLY))
					is_terminated = false;
			}
		}
		else if (! isType (token, TOKEN_KEYWORD))
		{
			/*
			 * Only create variables for global scope
			 */
			if ( token->nestLevel == 0 && is_global )
			{
				/*
				 * A pointer can be created to the function.
				 * If we recognize the function/class name ignore the variable.
				 * This format looks identical to a variable definition.
				 * A variable defined outside of a block is considered
				 * a global variable:
				 *	   var g_var1 = 1;
				 *	   var g_var2;
				 * This is not a global variable:
				 *	   var g_var = function;
				 * This is a global variable:
				 *	   var g_var = different_var_name;
				 */
				if (isAlreadyTaggedForToken(JSTAG_FUNCTION, token, &indexForName))
					;
				else if (isAlreadyTaggedForToken(JSTAG_CLASS, token, &indexForName))
					;
				else if (!vStringIsEmpty (name->string))
					indexForName = makeJsTag (name, is_const ? JSTAG_CONSTANT : JSTAG_VARIABLE, NULL, NULL);
			}
		}

		if (parenDepth > 0)
		{
			while (parenDepth > 0 && ! isType (token, TOKEN_EOF))
			{
				if (isType (token, TOKEN_OPEN_PAREN))
					parenDepth++;
				else if (isType (token, TOKEN_CLOSE_PAREN))
					parenDepth--;
				readTokenFull (token, true, NULL);
			}
			if (isType (token, TOKEN_CLOSE_CURLY))
				is_terminated = false;
		}
	}
	/* if we aren't already at the cmd end, advance to it and check whether
	 * the statement was terminated */
	if (! isType (token, TOKEN_CLOSE_CURLY) &&
	    ! isType (token, TOKEN_SEMICOLON))
	{
		/*
		 * Statements can be optionally terminated in the case of
		 * statement prior to a close curly brace as in the
		 * document.write line below:
		 *
		 * function checkForUpdate() {
		 *	   if( 1==1 ) {
		 *		   document.write("hello from checkForUpdate<br>")
		 *	   }
		 *	   return 1;
		 * }
		 */
		is_terminated = findCmdTerm (token, true, true, false);
		/* if we're at a comma, try and read a second var */
		if (isType (token, TOKEN_COMMA))
		{
			readToken (token);
			goto nextVar;
		}
	}

cleanUp:
	token->scope = saveScope;
	deleteToken (name);
	deleteToken (secondary_name);
	deleteToken (method_body_token);

	TRACE_LEAVE();

	return is_terminated;
}
static void parseUI5 (tokenInfo *const token)
{
	tokenInfo *const name = newToken ();
	/*
	 * SAPUI5 is built on top of jQuery.
	 * It follows a standard format:
	 *     sap.ui.controller("id.of.controller", {
	 *         method_name : function... {
	 *         },
	 *
	 *         method_name : function ... {
	 *         }
	 *     }
	 *
	 * Handle the parsing of the initial controller (and the
	 * same for "view") and then allow the methods to be
	 * parsed as usual.
	 */

	readToken (token);

	if (isType (token, TOKEN_PERIOD))
	{
		readToken (token);
		while (! isType (token, TOKEN_OPEN_PAREN) &&
			   ! isType (token, TOKEN_EOF))
		{
			readToken (token);
		}
		readToken (token);

		if (isType (token, TOKEN_STRING))
		{
			copyToken(name, token, true);

			/* Abusing dynamicProp field here.*/
			name->dynamicProp = true;
			readToken (token);
		}

		if (isType (token, TOKEN_COMMA))
			readToken (token);

		int r = CORK_NIL;
		if (isType (name, TOKEN_STRING)) {
			r = makeClassTag (name, NULL, NULL);
			tagEntryInfo *ui5entry = getEntryInCorkQueue (r);
			ui5entry->langType = getNamedLanguage ("OpenUI5", 0);
			ui5entry->kindIndex = OPENUI5TAG_CLASS;
		}

		do
		{
			parseMethods (token, r, false);
		} while (! isType (token, TOKEN_CLOSE_CURLY) &&
				 ! isType (token, TOKEN_EOF));
	}

	deleteToken (name);
}

static bool parseLine (tokenInfo *const token, bool is_inside_class)
{
	TRACE_ENTER_TEXT("token is '%s' of type %s under %d",
					 vStringValue(token->string), tokenTypeName (token->type),
					 token->scope);

	bool is_terminated = true;
	/*
	 * Detect the common statements, if, while, for, do, ...
	 * This is necessary since the last statement within a block "{}"
	 * can be optionally terminated.
	 *
	 * If the statement is not terminated, we need to tell
	 * the calling routine to prevent reading an additional token
	 * looking for the end of the statement.
	 */

	if (isType(token, TOKEN_KEYWORD))
	{
		switch (token->keyword)
		{
			case KEYWORD_for:
			case KEYWORD_while:
			case KEYWORD_do:
				is_terminated = parseLoop (token);
				break;
			case KEYWORD_if:
			case KEYWORD_else:
			case KEYWORD_try:
			case KEYWORD_catch:
			case KEYWORD_finally:
				/* Common semantics */
				is_terminated = parseIf (token);
				break;
			case KEYWORD_switch:
				parseSwitch (token);
				break;
			case KEYWORD_return:
			case KEYWORD_async:
				readToken (token);
				is_terminated = parseLine (token, is_inside_class);
				break;
			case KEYWORD_function:
				parseFunction (token);
				break;
			case KEYWORD_class:
				is_terminated = parseES6Class (token, NULL);
				break;
			default:
				is_terminated = parseStatement (token, is_inside_class);
				break;
		}
	}
	else
	{
		/*
		 * Special case where single line statements may not be
		 * SEMICOLON terminated.  parseBlock needs to know this
		 * so that it does not read the next token.
		 */
		is_terminated = parseStatement (token, is_inside_class);
	}

	TRACE_LEAVE();

	return is_terminated;
}

static void parseJsFile (tokenInfo *const token)
{
	TRACE_ENTER();

	do
	{
		readToken (token);

		if (isType (token, TOKEN_KEYWORD) && token->keyword == KEYWORD_sap)
			parseUI5 (token);
		else if (isType (token, TOKEN_KEYWORD) && (token->keyword == KEYWORD_export ||
		                                           token->keyword == KEYWORD_default))
			/* skip those at top-level */;
		else
			parseLine (token, false);
	} while (! isType (token, TOKEN_EOF));

	TRACE_LEAVE();
}

#ifdef DO_TRACING
static void dumpToken (const tokenInfo *const token)
{
	fprintf(stderr, "Token <%p>: %s: %s\n",
			token,
			tokenTypeName (token->type),
			(token->type == TOKEN_KEYWORD   ? keywordName (token->keyword):
			 token->type == TOKEN_IDENTIFIER? vStringValue (token->string):
			 ""));
}

static const char *tokenTypeName(enum eTokenType e)
{ /* Generated by misc/enumstr.sh with cmdline "parsers/jscript.c" "eTokenType" "tokenTypeName" */
	switch (e)
	{
		case    TOKEN_BINARY_OPERATOR: return "TOKEN_BINARY_OPERATOR";
		case          TOKEN_CHARACTER: return "TOKEN_CHARACTER";
		case        TOKEN_CLOSE_CURLY: return "TOKEN_CLOSE_CURLY";
		case        TOKEN_CLOSE_PAREN: return "TOKEN_CLOSE_PAREN";
		case       TOKEN_CLOSE_SQUARE: return "TOKEN_CLOSE_SQUARE";
		case              TOKEN_COLON: return "TOKEN_COLON";
		case              TOKEN_COMMA: return "TOKEN_COMMA";
		case                TOKEN_EOF: return "TOKEN_EOF";
		case         TOKEN_EQUAL_SIGN: return "TOKEN_EQUAL_SIGN";
		case         TOKEN_IDENTIFIER: return "TOKEN_IDENTIFIER";
		case            TOKEN_KEYWORD: return "TOKEN_KEYWORD";
		case         TOKEN_OPEN_CURLY: return "TOKEN_OPEN_CURLY";
		case         TOKEN_OPEN_PAREN: return "TOKEN_OPEN_PAREN";
		case        TOKEN_OPEN_SQUARE: return "TOKEN_OPEN_SQUARE";
		case             TOKEN_PERIOD: return "TOKEN_PERIOD";
		case   TOKEN_POSTFIX_OPERATOR: return "TOKEN_POSTFIX_OPERATOR";
		case             TOKEN_REGEXP: return "TOKEN_REGEXP";
		case          TOKEN_SEMICOLON: return "TOKEN_SEMICOLON";
		case               TOKEN_STAR: return "TOKEN_STAR";
		case             TOKEN_STRING: return "TOKEN_STRING";
		case    TOKEN_TEMPLATE_STRING: return "TOKEN_TEMPLATE_STRING";
		case          TOKEN_UNDEFINED: return "TOKEN_UNDEFINED";
		default: return "UNKNOWN";
	}
}

static const char *keywordName(enum eKeywordId e)
{ /* Generated by misc/enumstr.sh with cmdline "parsers/jscript.c" "eKeywordId" "keywordName" */
	switch (e)
	{
		case            KEYWORD_async: return "KEYWORD_async";
		case KEYWORD_capital_function: return "KEYWORD_capital_function";
		case   KEYWORD_capital_object: return "KEYWORD_capital_object";
		case            KEYWORD_catch: return "KEYWORD_catch";
		case            KEYWORD_class: return "KEYWORD_class";
		case            KEYWORD_const: return "KEYWORD_const";
		case          KEYWORD_default: return "KEYWORD_default";
		case               KEYWORD_do: return "KEYWORD_do";
		case             KEYWORD_else: return "KEYWORD_else";
		case           KEYWORD_export: return "KEYWORD_export";
		case          KEYWORD_extends: return "KEYWORD_extends";
		case          KEYWORD_finally: return "KEYWORD_finally";
		case              KEYWORD_for: return "KEYWORD_for";
		case         KEYWORD_function: return "KEYWORD_function";
		case              KEYWORD_get: return "KEYWORD_get";
		case               KEYWORD_if: return "KEYWORD_if";
		case              KEYWORD_let: return "KEYWORD_let";
		case              KEYWORD_new: return "KEYWORD_new";
		case        KEYWORD_prototype: return "KEYWORD_prototype";
		case           KEYWORD_return: return "KEYWORD_return";
		case              KEYWORD_sap: return "KEYWORD_sap";
		case              KEYWORD_set: return "KEYWORD_set";
		case           KEYWORD_static: return "KEYWORD_static";
		case           KEYWORD_switch: return "KEYWORD_switch";
		case             KEYWORD_this: return "KEYWORD_this";
		case              KEYWORD_try: return "KEYWORD_try";
		case              KEYWORD_var: return "KEYWORD_var";
		case            KEYWORD_while: return "KEYWORD_while";
		default: return "UNKNOWN";
	}
}

static const char *kindName (int k)
{
	if (k == KIND_GHOST_INDEX)
		return "<placeholder>";
	else
		return JsKinds [k].name;
}
#endif

static void initialize (const langType language)
{
	Assert (ARRAY_SIZE (JsKinds) == JSTAG_COUNT);
	Lang_js = language;

	TokenPool = objPoolNew (16, newPoolToken, deletePoolToken, clearPoolToken, NULL);
}

static void finalize (langType language CTAGS_ATTR_UNUSED, bool initialized)
{
	if (!initialized)
		return;

	objPoolDelete (TokenPool);
}

static void findJsTags (void)
{
	tokenInfo *const token = newToken ();

	NextToken = NULL;
	LastTokenType = TOKEN_UNDEFINED;

	parseJsFile (token);

	deleteToken (token);

#ifdef HAVE_ICONV
	if (JSUnicodeConverter != (iconv_t) -2 && /* not created */
	    JSUnicodeConverter != (iconv_t) -1 /* creation failed */)
	{
		iconv_close (JSUnicodeConverter);
		JSUnicodeConverter = (iconv_t) -2;
	}
#endif

	Assert (NextToken == NULL);
}

struct findGroupData {
	int corkIndex;				/* what found! */
	int expectedKind;			/* JSTAG_FUNCTION implies JSTAG_GENERATOR, too. */
};

static bool findGroup (int childCorkIndex, void *user_data)
{
	struct findGroupData *data = user_data;
	tagEntryInfo *child = getEntryInCorkQueue (childCorkIndex);

	TRACE_PRINT ("%d (placeholder: %d)", childCorkIndex, child->placeholder);
	if ((!child->placeholder)
		&& (((data->expectedKind == JSTAG_CLASS)
			 && (data->expectedKind == child->kindIndex))
			|| ((data->expectedKind == JSTAG_FUNCTION
				 || data->expectedKind == JSTAG_GENERATOR)
				&& ((child->kindIndex == JSTAG_FUNCTION)
					|| child->kindIndex == JSTAG_GENERATOR))))
	{
		data->corkIndex = childCorkIndex;
		TRACE_PRINT ("FOUND");
		return true;			/* Found -> stop iteration */
	}
	TRACE_PRINT ("NOT FOUND");
	return false;
}

static bool isAlreadyTaggedRaw (int scopeIndex, const char *name, int kindIndex,
								int *existing)
{
	struct findGroupData data = {
		.corkIndex = CORK_NIL,
		.expectedKind = kindIndex,
	};
	TRACE_PRINT("Searching '%s' below %d (expected kind: %d)...[",
				name, scopeIndex, kindIndex);
	if (forEachNamedChildForCorkEntry (scopeIndex, name, findGroup, &data))
	{
		if (existing)
			*existing = data.corkIndex;
		TRACE_PRINT("] %d", data.corkIndex);
		return true;
	}
	else
	{
		TRACE_PRINT("] %s", "not found");
		return false;
	}
}

static bool isAlreadyTaggedForTag (tagEntryInfo *e, int *exitingScopeIndex)
{
	return isAlreadyTaggedRaw (e->extensionFields.scopeIndex,
							   e->name,
							   e->kindIndex,
							   exitingScopeIndex);
}

static bool isAlreadyTaggedForToken (int kindIndex, tokenInfo *const token, int *existing)
{
	return isAlreadyTaggedRaw (token->scope, vStringValue(token->string), kindIndex,
							   existing);
}

static bool filterChild (langType langType, int parentIndex, int childCandidateIndex,
						 const tagEntryInfo *childCandidateTag)
{
	tagEntryInfo *parent = getEntryInCorkQueue (parentIndex);

	if (parent == CORK_NIL)
		return true;

	if (isTagExtraBitMarked (childCandidateTag, XTAG_REFERENCE_TAGS))
		return false;

	if (isTagExtraBitMarked (childCandidateTag, XTAG_QUALIFIED_TAGS))
		return false;

	if (isTagExtraBitMarked (childCandidateTag, XTAG_SUBWORD))
		return false;

	return true;
}

static bool detectGroup (langType language,
						 const tagEntryInfo *newEntry,
						 const tagEntryInfo *preExistingEntry)
{
	if (newEntry->extensionFields.scopeIndex == CORK_NIL &&
		preExistingEntry->extensionFields.scopeIndex == CORK_NIL)
		return true;
	else if (newEntry->extensionFields.scopeIndex == CORK_NIL
			 || preExistingEntry->extensionFields.scopeIndex == CORK_NIL)
		return false;
	else
	{
		tagEntryInfo *a = getEntryInCorkQueue (newEntry->extensionFields.scopeIndex);
		tagEntryInfo *b = getEntryInCorkQueue (preExistingEntry->extensionFields.scopeIndex);
		return detectGroup (language, a, b);
	}
}

extern parserDefinition* OpenUI5Parser (void)
{
	parserDefinition *const def = parserNew ("OpenUI5");
	def->kindTable	= OpenUI5Kinds;
	def->kindCount	= ARRAY_SIZE (OpenUI5Kinds);

	/* This must be implementated in a separated sub parser. */
	def->invisible   = true;
	/* TODO: these setting should be inherited from the base parser automatically. */
	def->useCork	 = CORK_TABLE_REVERSE_NAME_MAP|CORK_TABLE_REVERSE_SCOPE_MAP|CORK_TABLE_QUEUE;
	return def;
}

/* Create parser definition structure */
extern parserDefinition* JavaScriptParser (void)
{
	// .jsx files are JSX: https://facebook.github.io/jsx/
	// which have JS function definitions, so we just use the JS parser
	static const char *const extensions [] = { "js", "jsx", "mjs", NULL };
	static const char *const aliases [] = { "js", "node", "nodejs",
	                                        "seed", "gjs", NULL };
	parserDefinition *const def = parserNew ("JavaScript");
	def->extensions = extensions;
	def->aliases = aliases;
	/*
	 * New definitions for parsing instead of regex
	 */
	def->kindTable	= JsKinds;
	def->kindCount	= ARRAY_SIZE (JsKinds);
	def->parser		= findJsTags;
	def->initialize = initialize;
	def->finalize   = finalize;
	def->keywordTable = JsKeywordTable;
	def->keywordCount = ARRAY_SIZE (JsKeywordTable);

	def->useCork	= CORK_TABLE_REVERSE_NAME_MAP|CORK_TABLE_REVERSE_SCOPE_MAP|CORK_TABLE_QUEUE;
	def->filterChild = filterChild;
	def->detectGroup = detectGroup;

	return def;
}
