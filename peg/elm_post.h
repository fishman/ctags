/*
 *   Copyright (c) 2022 Nik Silver
 *   based on the Kotlin code by Jan Dolinár
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 *
 *   This module contains functions to generate tags for Elm.
 *   See https://elm-lang.org/docs/syntax for language reference.
 */

/*
 *   INCLUDE FILES
 */
#include "parse.h"
#include "trace.h"

/*
 * FUNCTION DEFINITIONS
 */

/* kind - The kind of this tag.
 * role - The role this tag plays. But def(ined) is expressed by setting
 *     this to ROLE_DEFINITION_INDEX.
 * pushScope - If true, also update the scope to be this tag.
 */
static int makeElmTag (struct parserCtx *auxil, const char *name, long offset, int kind, int role)
{
	tagEntryInfo e;

	if (role == ROLE_DEFINITION_INDEX)
	{
		initTagEntry (&e, name, kind);
	}
	else
	{
		initRefTagEntry (&e, name, kind, role);
	}

	unsigned long lineNumber = getInputLineNumberForFileOffset (offset);
	updateTagLine (&e, lineNumber, getInputFilePositionForLine (lineNumber));
	e.extensionFields.scopeIndex = BASE_SCOPE (auxil);
	int scope_index = makeTagEntry (&e);
	return scope_index;
}

/* Like makeElmTag, but have this tag be the latest scope.
 */
static int makeElmTagSettingScope (struct parserCtx *auxil, const char *name, long offset, int kind, int role)
{
	// Here is a useless line just to use a function and so
	// get rid of a compiler warning.
	if (1 < 0)
	{
		PEEK_KIND (auxil);
	}
	// The real function starts here...

	int scope_index = makeElmTag (auxil, name, offset, kind, role);
	SET_SCOPE (auxil, scope_index);
	return scope_index;
}

static void addElmSignature(int scope_index, const char *sig)
{
	tagEntryInfo *e = getEntryInCorkQueue (scope_index);

	if (e)
	{
		vString *vsig = collapseWhitespace (sig);

		e->extensionFields.signature = vStringDeleteUnwrap (vsig);
	}
}

static void addElmTypeRef(int scope_index, const char *sig)
{
	tagEntryInfo *e = getEntryInCorkQueue (scope_index);

	if (e)
	{
		vString *vsig = collapseWhitespace (sig);

		e->extensionFields.typeRef [0] = eStrdup ("typename");
		e->extensionFields.typeRef [1] = vStringDeleteUnwrap (vsig);
	}
}

static void addElmAccess(int scope_index, const char *access_)
{
	tagEntryInfo *e = getEntryInCorkQueue (scope_index);

	if (e)
		e->extensionFields.access = eStrdup (access_);
}

/* There are several steps to making the type of constructors within
 * a custom type:
 *   1. Initialise the fields when we encounter the custom type declaration.
 *   2. Initialise the subtype field (do this for each new constructor).
 *   3. Add each subtype as we find it.
 *   4. Make the typeref field.
 *   5. Tidy up
 */
static void initElmConstructorFields (struct parserCtx *auxil, const char *name)
{
	auxil->customType = vStringNewInit (name);
	auxil->consSubtype = vStringNew ();
}

static void initElmConstructorSubtypeFields (struct parserCtx *auxil)
{
	vStringClear (auxil->consSubtype);
}

static void addElmConstructorSubtype (struct parserCtx *auxil, const char *name)
{
	vStringCatS (auxil->consSubtype, name);
	vStringCatS (auxil->consSubtype, " -> ");
}

static void addElmConstructorTypeRef (struct parserCtx *auxil, int tag_index)
{
	vStringCat (auxil->consSubtype, auxil->customType);
	addElmTypeRef (tag_index, vStringValue (auxil->consSubtype));
}

static void tidyElmConstructorFields (struct parserCtx *auxil)
{
	vStringDelete (auxil->consSubtype);
	vStringDelete (auxil->customType);
}

/* For a signature such as "a1   b2  c3" we want to transform it
 * to "a1 b2 c3" for the signature field.
 */
static vString *collapseWhitespace (const char *sig)
{
	vString *vstr = vStringNew ();

	const char *c = sig;
	char c2;
	int found_ws = 0;

	for ( ; *c != '\0'; c++)
	{
		// The character, in case we need to change it
		c2 = *c;

		if (isspace ((unsigned char) c2))
		{
			// It's whitespace. Make it plain space
			c2 = ' ';
			if (found_ws)
			{
				// We found whitespace just before, so ignore this
				continue;
			}

			found_ws = 1;
		}
		else
		{
			found_ws = 0;
		}
		vStringPut (vstr, c2);
	}

	return vstr;
}

static void ctxInit (struct parserCtx *auxil)
{
	BASE_INIT (auxil, K_MODULE);
}

static void ctxFini (struct parserCtx *auxil)
{
	BASE_FINI (auxil);
}

static void findElmTags (void)
{
	struct parserCtx auxil;

	ctxInit (&auxil);
	pelm_context_t *pctx = pelm_create (&auxil);

	while (pelm_parse (pctx, NULL) && (! BASE_ERROR (&auxil)))
		;

	pelm_destroy (pctx);
	ctxFini (&auxil);
}

extern parserDefinition *ElmParser (void)
{
	static const char *const extensions [] = { "elm", NULL };
	parserDefinition *def = parserNew ("Elm");
	def->kindTable = ElmKinds;
	def->kindCount = ARRAY_SIZE (ElmKinds);
	def->extensions = extensions;
	def->parser = findElmTags;
	def->fieldTable = ElmFields;
	def->fieldCount = ARRAY_SIZE (ElmFields);
	def->useCork = true;
	def->enabled    = true;
	//def->requestAutomaticFQTag = true;
	def->defaultScopeSeparator = ".";
	return def;
}
