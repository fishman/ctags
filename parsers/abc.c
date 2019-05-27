/*
*   Copyright (c) 2009, Eric Forgeot
*
*   Based on work by Jon Strait
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License.
*
*   This module contains functions for generating tags for Abc files.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"	/* must always come first */

#include <ctype.h>
#include <string.h>

#include "parse.h"
#include "read.h"
#include "vstring.h"
#include "routines.h"
#include "entry.h"

/*
*   DATA DEFINITIONS
*/

typedef enum {
   K_SECTION,
} AbcKind;

static kindDefinition AbcKinds[] = {
	{ true, 's', "section", "sections" },
};

/*
*   FUNCTION DEFINITIONS
*/

static void makeAbcTag (const vString* const name, bool name_before)
{
	tagEntryInfo e;
	initTagEntry (&e, vStringValue(name), K_SECTION);

	if (name_before)
		e.lineNumber--;	/* we want the line before the underline chars */

	makeTagEntry(&e);
}

static void findAbcTags (void)
{
	vString *name = vStringNew();
	const unsigned char *line;

	while ((line = readLineFromInputFile()) != NULL)
	{
		if (line[0] == 'T') {
			vStringCatS(name, " / ");
			vStringCatS(name, (const char *) line);
			makeAbcTag(name, false);
		}
		else {
			vStringClear (name);
			if (! isspace(*line))
				vStringCatS(name, (const char*) line);
		}
	}
	vStringDelete (name);
}

extern parserDefinition* AbcParser (void)
{
	static const char *const patterns [] = { "*.abc", NULL };
	static const char *const extensions [] = { "abc", NULL };
	parserDefinition* const def = parserNew ("Abc");

	def->kindTable = AbcKinds;
	def->kindCount = ARRAY_SIZE (AbcKinds);
	def->patterns = patterns;
	def->extensions = extensions;
	def->parser = findAbcTags;
	return def;
}

