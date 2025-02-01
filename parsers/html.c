/*
*   Copyright (c) 2016, Jiri Techet
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for generating tags for HTML language
*   files.
*/

#include "general.h"

#include <string.h>
#include <ctype.h>

#include "entry.h"
#include "parse.h"
#include "read.h"
#include "routines.h"
#include "keyword.h"
#include "promise.h"
#include "trace.h"

/* The max. number of nested elements - prevents further recursion if the limit
 * is exceeded and avoids stack overflow for invalid input containing too many
 * open tags */
#define MAX_DEPTH 1000


typedef enum {
	K_ANCHOR,
	K_CLASS,
	K_TITLE,
	K_HEADING1,
	K_HEADING2,
	K_HEADING3,
	K_STYELSHEET,
	K_ID,
	K_SCRIPT,
} htmlKind;


typedef enum {
	CLASS_KIND_ATTRIBUTE_ROLE,
} ClassRole;

typedef enum {
	SCRIPT_KIND_EXTERNAL_FILE_ROLE,
} ScriptRole;

typedef enum {
	STYLESHEET_KIND_EXTERNAL_FILE_ROLE,
} StylesheetRole;

static roleDefinition ClassRoles [] = {
	{ true, "attribute", "assigned as attributes" },
};

static roleDefinition ScriptRoles [] = {
	{ true, "extFile", "referenced as external files" },
};

static roleDefinition StylesheetRoles [] = {
	{ true, "extFile", "referenced as external files" },
};

static kindDefinition HtmlKinds [] = {
	{ true, 'a', "anchor",		"named anchors" },
	{ true, 'c', "class",		"classes",
	  .referenceOnly = true, ATTACH_ROLES (ClassRoles)},
	{ true, 't', "title",		"titles" },
	{ true, 'h', "heading1",	"H1 headings" },
	{ true, 'i', "heading2",	"H2 headings" },
	{ true, 'j', "heading3",	"H3 headings" },
	{ true, 'C', "stylesheet",	"stylesheets",
	  .referenceOnly = true, ATTACH_ROLES (StylesheetRoles)},
	{ true, 'I', "id",			"identifiers" },
	{ true, 'J', "script",		"scripts",
	  .referenceOnly = true, ATTACH_ROLES (ScriptRoles)},
};

typedef enum {
	/* The order starting from "title" to "h3" should
	 * not be changed.
	 *
	 */
	KEYWORD_heading_start,
	KEYWORD_title = KEYWORD_heading_start,
	KEYWORD_h1,
	KEYWORD_h2,
	KEYWORD_h3,
	KEYWORD_heading_end = KEYWORD_h3,
	KEYWORD_a,
	KEYWORD_script,
	KEYWORD_style,
	KEYWORD_name,

	/* void elements */
	KEYWORD_area,
	KEYWORD_base,
	KEYWORD_br,
	KEYWORD_class,
	KEYWORD_col,
	KEYWORD_command,
	KEYWORD_embed,
	KEYWORD_hr,
	KEYWORD_href,
	KEYWORD_id,
	KEYWORD_img,
	KEYWORD_input,
	KEYWORD_keygen,
	KEYWORD_link,
	KEYWORD_meta,
	KEYWORD_param,
	KEYWORD_rel,
	KEYWORD_source,
	KEYWORD_src,
	KEYWORD_track,
	KEYWORD_wbr
} keywordId;

static const keywordTable HtmlKeywordTable[] = {
	{"title", KEYWORD_title},
	{"h1", KEYWORD_h1},
	{"h2", KEYWORD_h2},
	{"h3", KEYWORD_h3},
	{"a", KEYWORD_a},
	{"script", KEYWORD_script},
	{"style", KEYWORD_style},
	{"name", KEYWORD_name},

	/* void elements */
	{"area", KEYWORD_area},
	{"base", KEYWORD_base},
	{"br", KEYWORD_br},
	{"class", KEYWORD_class},
	{"col", KEYWORD_col},
	{"command", KEYWORD_command},
	{"embed", KEYWORD_embed},
	{"hr", KEYWORD_hr},
	{"href", KEYWORD_href},
	{"id", KEYWORD_id},
	{"img", KEYWORD_img},
	{"input", KEYWORD_input},
	{"keygen", KEYWORD_keygen},
	{"link", KEYWORD_link},
	{"meta", KEYWORD_meta},
	{"param", KEYWORD_param},
	{"rel", KEYWORD_rel},
	{"source", KEYWORD_source},
	{"src", KEYWORD_src},
	{"track", KEYWORD_track},
	{"wbr", KEYWORD_wbr},
};

typedef enum {
	TOKEN_EOF,
	TOKEN_NAME,			/* tag and attribute names */
	TOKEN_STRING,		/* single- or double-quoted attribute value */
	TOKEN_TEXT,
	TOKEN_OPEN_TAG_START,	/* <  */
	TOKEN_CLOSE_TAG_START,	/* </ */
	TOKEN_TAG_END,		/* >  */
	TOKEN_TAG_END2,		/* /> */
	TOKEN_EQUAL,
	TOKEN_COMMENT,
	TOKEN_OTHER
} tokenType;

#ifdef DEBUG
static const char *tokenTypes[] = {
#define E(X) [TOKEN_##X] = #X
	E(EOF),
	E(NAME),
	E(STRING),
	E(TEXT),
	E(OPEN_TAG_START),
	E(CLOSE_TAG_START),
	E(TAG_END),
	E(TAG_END2),
	E(EQUAL),
	E(COMMENT),
	E(OTHER),
#undef E
};
#endif

typedef struct {
	tokenType type;
	vString *string;
} tokenInfo;


static int Lang_html;


static void readTag (tokenInfo *token, vString *text, int depth);

static void skipOtherScriptContent (const int delimiter);

static void readTokenText (tokenInfo *const token, bool collectText)
{
	int c;
	int lastC = 'X';  /* whatever non-space character */

	vStringClear (token->string);

getNextChar:

	c = getcFromInputFile ();

	switch (c)
	{
		case EOF:
			token->type = TOKEN_EOF;
			break;

		case '<':
			ungetcToInputFile (c);
			token->type = TOKEN_TEXT;
			break;

		default:
			if (collectText)
			{
				if (isspace (c))
					c = ' ';
				if (c != ' ' || lastC != ' ')
				{
					vStringPut (token->string, c);
					lastC = c;
				}
			}

			goto getNextChar;
	}
}

static void readTokenInScript (tokenInfo *const token)
{
	int c;

	vStringClear (token->string);

	c = getcFromInputFile ();
	while (isspace (c))
		c = getcFromInputFile ();

	switch (c)
	{
		case EOF:
			token->type = TOKEN_EOF;
			break;

		case '<':
		{
			int d = getcFromInputFile ();
			if (d == '/')
				token->type = TOKEN_CLOSE_TAG_START;
			else
			{
				ungetcToInputFile (d);
				token->type = TOKEN_OTHER;
			}
			break;
		}
		default:
		{
			while (!isspace (c) && c != '<' && c != '>' && c != '/' &&
				   c != '=' && c != '\'' && c != '"' && c != EOF)
			{
				vStringPut (token->string, tolower (c));
				c = getcFromInputFile ();
			}

			if (vStringLength (token->string) == 0)
				token->type = TOKEN_OTHER;
			else
			{
				token->type = TOKEN_NAME;
				if (c != EOF)
					ungetcToInputFile (c);
			}
			break;
		}
	}

	TRACE_PRINT("token (in script): %s (%s)", tokenTypes[token->type], vStringValue (token->string));
}

static void readToken (tokenInfo *const token, bool skipComments)
{
	int c;

	vStringClear (token->string);

getNextChar:

	c = getcFromInputFile ();
	while (isspace (c))
		c = getcFromInputFile ();

	switch (c)
	{
		case EOF:
			token->type = TOKEN_EOF;
			break;

		case '<':
		{
			int d = getcFromInputFile ();
			if (d == '!')
			{
				d = getcFromInputFile ();
				if (d == '-')
				{
					d = getcFromInputFile ();
					if (d == '-')
					{
						int e = ' ';
						int f = ' ';
						do
						{
							d = e;
							e = f;
							f = getcFromInputFile ();
						}
						while (f != EOF && ! (d == '-' && e == '-' && f == '>'));

						if (skipComments)
							goto getNextChar;
						else
						{
							token->type = TOKEN_COMMENT;
							break;
						}
					}
				}
				ungetcToInputFile (d);
				token->type = TOKEN_OTHER;
			}
			else if (d == '?')
			{
				skipOtherScriptContent(d);
				goto getNextChar;
			}
			else if (d == '/')
				token->type = TOKEN_CLOSE_TAG_START;
			else
			{
				ungetcToInputFile (d);
				token->type = TOKEN_OPEN_TAG_START;
			}
			break;
		}
		case '/':
		{
			int d = getcFromInputFile ();
			if (d == '>')
				token->type = TOKEN_TAG_END2;
			else
			{
				ungetcToInputFile (d);
				token->type = TOKEN_OTHER;
			}
			break;
		}
		case '>':
			token->type = TOKEN_TAG_END;
			break;

		case '=':
			token->type = TOKEN_EQUAL;
			break;

		case '"':
		case '\'':
		{
			const int delimiter = c;
			c = getcFromInputFile ();
			while (c != EOF && c != delimiter)
			{
				vStringPut (token->string, c);
				c = getcFromInputFile ();
			}
			token->type = TOKEN_STRING;
			break;
		}

		default:
		{
			do
			{
				vStringPut (token->string, tolower (c));
				c = getcFromInputFile ();
			}
			while (!isspace (c) && c != '<' && c != '>' && c != '/' &&
				   c != '=' && c != '\'' && c != '"' && c != EOF);
			if (c != EOF)
				ungetcToInputFile (c);
			token->type = TOKEN_NAME;
			break;
		}
	}

	TRACE_PRINT("token: %s (%s)", tokenTypes[token->type], vStringValue (token->string));
}

static void appendText (vString *text, vString *appendedText)
{
	if (text != NULL && vStringLength (appendedText) > 0)
	{
		if (vStringLength (text) > 0 && vStringLast (text) == ' ' &&
			vStringLength (appendedText) > 0 && vStringChar (appendedText, 0) == ' ')
		{
			vStringStripTrailing (text);
		}
		vStringCat (text, appendedText);
	}
}

static bool readTagContent (tokenInfo *token, vString *text, long *line, long *lineOffset, int depth)
{
	TRACE_ENTER();

	tokenType type;

	readTokenText (token, text != NULL);
	appendText (text, token->string);

	do
	{
		*line = getInputLineNumber ();
		*lineOffset = getInputLineOffset ();
		readToken (token, false);
		type = token->type;
		if (type == TOKEN_OPEN_TAG_START)
			readTag (token, text, depth + 1);
		if (type == TOKEN_COMMENT || type == TOKEN_OPEN_TAG_START)
		{
			readTokenText (token, text != NULL);
			appendText (text, token->string);
		}
	}
	while (type == TOKEN_COMMENT || type == TOKEN_OPEN_TAG_START);

	TRACE_LEAVE_TEXT("is_close_tag? %d", type == TOKEN_CLOSE_TAG_START);

	return type == TOKEN_CLOSE_TAG_START;
}

static bool skipScriptContent (tokenInfo *token, long *line, long *lineOffset)
{
	TRACE_ENTER();

	bool found_start = false;
	bool found_script = false;

	long line_tmp[2] = {0};
	long lineOffset_tmp[2] = {0};

	tokenType type;

	do
	{
		line_tmp[0] = getInputLineNumber ();
		lineOffset_tmp[0] = getInputLineOffset ();

		readTokenInScript (token);
		type = token->type;

		if (type == TOKEN_CLOSE_TAG_START)
		{
			found_start = true;
			line_tmp[1] = line_tmp[0];
			lineOffset_tmp[1] = lineOffset_tmp[0];
		}
		else if (found_start
				 && type == TOKEN_NAME
				 && lookupKeyword (vStringValue (token->string), Lang_html) == KEYWORD_script)
		{
			found_script = true;
			*line = line_tmp[1];
			*lineOffset = lineOffset_tmp[1];
		}
		else
			found_start = false;
	}
	while ((type != TOKEN_EOF) && (!found_script));

	TRACE_LEAVE_TEXT("found_script? %d", found_script);

	return found_script;
}

static void skipOtherScriptContent (const int delimiter)
{
	TRACE_ENTER();

	const long startSourceLineNumber = getSourceLineNumber ();
	const long startLineNumber = getInputLineNumber ();
	const long startLineOffset = getInputLineOffset () - 2;

	vString *script_name = vStringNew ();
	bool reading_script_name = true;
	while (1)
	{
		int c = getcFromInputFile ();
		if (c == EOF)
		{
			break;
		}
		else if (reading_script_name && !isspace(c))
		{
			vStringPut (script_name, c);
		}
		else if (reading_script_name)
		{
			reading_script_name = false;
		}
		else if (c == delimiter)
		{
			c = getcFromInputFile ();
			if (c == '>')
			{
				break;
			}
			ungetcToInputFile (c);
		}
	}

	if (strcasecmp ("php", vStringValue (script_name)) == 0
		|| strcmp ("=", vStringValue (script_name)) == 0)
		makePromise ("PHP", startLineNumber, startLineOffset,
					 getInputLineNumber (), getInputLineOffset (),
					 startSourceLineNumber);

	vStringDelete (script_name);

	TRACE_LEAVE();
}

static void makeClassRefTags (const char *classes)
{
	vString *klass = vStringNew ();

	do
	{
		if (*classes && !isspace ((unsigned char) *classes))
			vStringPut (klass, *classes);
		else if (!vStringIsEmpty (klass))
		{
			makeSimpleRefTag (klass, K_CLASS,
							  CLASS_KIND_ATTRIBUTE_ROLE);
			vStringClear (klass);
		}

		if (!*classes)
			break;

		classes++;
	} while (1);

	vStringDelete (klass);
}

static void readTag (tokenInfo *token, vString *text, int depth)
{
	TRACE_ENTER();

	bool textCreated = false;

	readToken (token, true);
	if (token->type == TOKEN_NAME)
	{
		keywordId startTag;
		bool isHeading;
		bool isVoid;
		vString *stylesheet = NULL;
		bool stylesheet_expectation = false;

		startTag = lookupKeyword (vStringValue (token->string), Lang_html);
		isHeading = (KEYWORD_heading_start <= startTag && startTag <= KEYWORD_heading_end);
		isVoid = (startTag >= KEYWORD_area && startTag <= KEYWORD_wbr);
		if (text == NULL && isHeading)
		{
			text = vStringNew ();
			textCreated = true;
		}

		do
		{
			keywordId attribute = KEYWORD_NONE;

			readToken (token, true);
			if (token->type == TOKEN_NAME)
				attribute = lookupKeyword (vStringValue (token->string), Lang_html);

			if (attribute == KEYWORD_class)
			{
				readToken (token, true);
				if (token->type == TOKEN_EQUAL)
				{
					readToken (token, true);
					if (token->type == TOKEN_STRING)
						makeClassRefTags (vStringValue (token->string));
				}
			}
			else if (attribute == KEYWORD_id)
			{
				readToken (token, true);
				if (token->type == TOKEN_EQUAL)
				{
					readToken (token, true);
					if (token->type == TOKEN_STRING)
						makeSimpleTag (token->string, K_ID);
				}
			}
			else if (startTag == KEYWORD_a && attribute == KEYWORD_name)
			{
				readToken (token, true);
				if (token->type == TOKEN_EQUAL)
				{
					readToken (token, true);
					if (token->type == TOKEN_STRING || token->type == TOKEN_NAME)
						makeSimpleTag (token->string, K_ANCHOR);
				}
			}
			else if (startTag == KEYWORD_script && attribute == KEYWORD_src)
			{
				readToken (token, true);
				if (token->type == TOKEN_EQUAL)
				{
					readToken (token, true);
					if (token->type == TOKEN_STRING)
						makeSimpleRefTag (token->string, K_SCRIPT,
										  SCRIPT_KIND_EXTERNAL_FILE_ROLE);
				}
			}
			else if (startTag == KEYWORD_link)
			{
				if (attribute == KEYWORD_rel)
				{
					readToken (token, true);
					if (token->type == TOKEN_EQUAL)
					{
						readToken (token, true);
						if (token->type == TOKEN_STRING &&
							/* strcmp is not enough:
							 * e.g. <link href="fancy.css"
							 *            rel="alternate stylesheet" title="Fancy"> */
							vStringLength(token->string) >= 10 &&
							strstr (vStringValue (token->string), "stylesheet"))
							stylesheet_expectation = true;
					}
				}
				else if (attribute == KEYWORD_href)
				{
					readToken (token, true);
					if (token->type == TOKEN_EQUAL)
					{
						readToken (token, true);
						if (token->type == TOKEN_STRING)
						{
							if (stylesheet == NULL)
								stylesheet = vStringNewCopy (token->string);
							else
								vStringCopy (stylesheet, token->string);
						}
					}
				}
				if (stylesheet_expectation && stylesheet && !vStringIsEmpty (stylesheet))
				{
					makeSimpleRefTag (stylesheet, K_STYELSHEET,
									  STYLESHEET_KIND_EXTERNAL_FILE_ROLE);
					stylesheet_expectation = false;
					if (stylesheet)
						vStringClear (stylesheet);
				}
			}
		}
		while (token->type != TOKEN_TAG_END && token->type != TOKEN_TAG_END2 &&
			   token->type != TOKEN_EOF);

		vStringDelete (stylesheet);
		stylesheet = NULL;

		if (!isVoid && token->type == TOKEN_TAG_END && depth < MAX_DEPTH)
		{
			long startSourceLineNumber = getSourceLineNumber ();
			long startLineNumber = getInputLineNumber ();
			long startLineOffset = getInputLineOffset ();
			long endLineNumber;
			long endLineOffset;
			bool tag_start2;

			if (startTag == KEYWORD_script)
			{
				bool script = skipScriptContent (token, &endLineNumber, &endLineOffset);
				if (script)
					makePromise ("JavaScript", startLineNumber, startLineOffset,
								 endLineNumber, endLineOffset, startSourceLineNumber);
				readToken (token, true);
				goto out;
			}

			tag_start2 = readTagContent (token, text, &endLineNumber, &endLineOffset, depth);
			if (tag_start2)
			{
				readToken (token, true);
				if (isHeading && textCreated && vStringLength (text) > 0)
				{
					keywordId endTag = lookupKeyword (vStringValue (token->string), Lang_html);
					if (startTag == endTag)
					{
						htmlKind headingKind;

						if (startTag == KEYWORD_title)
							headingKind = K_TITLE;
						if (startTag == KEYWORD_h1)
							headingKind = K_HEADING1;
						else if (startTag == KEYWORD_h2)
							headingKind = K_HEADING2;
						else
							headingKind = K_HEADING3;

						vStringStripLeading (text);
						vStringStripTrailing (text);
						makeSimpleTag (text, headingKind);
					}
				}
				else if (startTag == KEYWORD_style)
				{
					keywordId endTag = lookupKeyword (vStringValue (token->string), Lang_html);
					if (startTag == endTag)
						makePromise ("CSS", startLineNumber, startLineOffset,
									 endLineNumber, endLineOffset, startSourceLineNumber);
				}

				readToken (token, true);
			}
		}
	}

 out:
	if (textCreated)
		vStringDelete (text);

	TRACE_LEAVE();
}

static void findHtmlTags (void)
{
	TRACE_ENTER();

	tokenInfo token;

	token.string = vStringNew ();

	do
	{
		readToken (&token, true);
		if (token.type == TOKEN_OPEN_TAG_START)
			readTag (&token, NULL, 0);
	}
	while (token.type != TOKEN_EOF);

	vStringDelete (token.string);

	TRACE_LEAVE();
}

static void initialize (const langType language)
{
	Lang_html = language;
}

/* parser definition */
extern parserDefinition* HtmlParser (void)
{
	static const char *const extensions [] = { "htm", "html", NULL };
	parserDefinition* def = parserNew ("HTML");
	def->kindTable        = HtmlKinds;
	def->kindCount    = ARRAY_SIZE (HtmlKinds);
	def->extensions   = extensions;
	def->parser       = findHtmlTags;
	def->initialize   = initialize;
	def->keywordTable = HtmlKeywordTable;
	def->keywordCount = ARRAY_SIZE (HtmlKeywordTable);
	return def;
}
