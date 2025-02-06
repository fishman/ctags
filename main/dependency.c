/*
 *
 *  Copyright (c) 2016, Red Hat, Inc.
 *  Copyright (c) 2016, Masatake YAMATO
 *
 *  Author: Masatake YAMATO <yamato@redhat.com>
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 *
 */

#include "general.h"  /* must always come first */

#include "debug.h"
#include "dependency.h"
#include "entry.h"
#include "options.h"
#include "parse_p.h"
#include "read.h"
#include "read_p.h"
#include "routines.h"
#include "subparser.h"
#include "subparser_p.h"
#include "xtag.h"

#include <string.h>

struct slaveControlBlock {
	slaveParser *slaveParsers;	/* The parsers on this list must be initialized when
								   this parser is initialized. */
	subparser   *subparsersDefault;
	subparser   *subparsersInUse;
	langType     owner;
};

extern void linkDependencyAtInitializeParsing (depType dtype,
						   parserDefinition *const master,
						   struct slaveControlBlock *masterSCB,
						   struct kindControlBlock *masterKCB,
						   parserDefinition *const slave,
						   struct kindControlBlock *slaveKCB,
						   void *data)
{
	if (dtype == DEPTYPE_KIND_OWNER)
		linkKindDependency (masterKCB, slaveKCB);
	else if (dtype == DEPTYPE_SUBPARSER || dtype == DEPTYPE_FOREIGNER)
	{
		slaveParser *s = xMalloc (1, slaveParser);

		s->type = dtype;
		s->id = slave->id;
		s->data = data;

		s->next = masterSCB->slaveParsers;
		masterSCB->slaveParsers = s;
	}
}

static void attachSubparser (struct slaveControlBlock *base_sb, subparser *subparser)
{
	   subparser->next = base_sb->subparsersDefault;
	   base_sb->subparsersDefault = subparser;
}


extern struct slaveControlBlock *allocSlaveControlBlock (parserDefinition *parser)
{
	struct slaveControlBlock *cb;

	cb = xMalloc (1, struct slaveControlBlock);
	cb->slaveParsers = NULL;
	cb->subparsersDefault = NULL;
	cb->subparsersInUse = NULL;
	cb->owner = parser->id;

	return cb;
}

extern void freeSlaveControlBlock (struct slaveControlBlock *cb)
{
	eFree (cb);
}

extern void initializeDependencies (parserDefinition *parser,
									struct slaveControlBlock *cb)
{
	unsigned int i;
	slaveParser *sp;

	/* Initialize slaves */
	sp = cb->slaveParsers;
	while (sp != NULL)
	{
		if (sp->type == DEPTYPE_SUBPARSER)
		{
			subparser *sub;

			sub = (subparser *)sp->data;
			sub->slaveParser = sp;
		}

		if (sp->type == DEPTYPE_KIND_OWNER
			|| (sp->type == DEPTYPE_SUBPARSER &&
				(((subparser *)sp->data)->direction & SUBPARSER_BASE_RUNS_SUB)))
		{
			initializeParser (sp->id);
			if (sp->type == DEPTYPE_SUBPARSER
				&& isXtagEnabled (XTAG_SUBPARSER))
			{
				subparser *subparser = sp->data;
				attachSubparser (cb, subparser);
			}
		}
		sp = sp->next;
	}

	/* Initialize masters that act as base parsers. */
	for (i = 0; i < parser->dependencyCount; i++)
	{
		parserDependency *d = parser->dependencies + i;
		if ((d->type == DEPTYPE_SUBPARSER &&
			 ((subparser *)(d->data))->direction & SUBPARSER_SUB_RUNS_BASE)
			|| (d->type == DEPTYPE_FOREIGNER))
		{
			langType baseParser;
			baseParser = getNamedLanguage (d->upperParser, 0);
			Assert (baseParser != LANG_IGNORE);
			initializeParser (baseParser);
		}
	}
}

extern void finalizeDependencies (parserDefinition *parser,
								  struct slaveControlBlock *cb)
{
	while (cb->slaveParsers)
	{
		slaveParser *sp = cb->slaveParsers;
		cb->slaveParsers = sp->next;
		sp->next = NULL;
		eFree (sp);
	}
}

extern void notifyInputStart (void)
{
	subparser *s;

	/* for running prelude of optlib */
	langType lang = getInputLanguage ();
	notifyLanguageRegexInputStart (lang);

	foreachSubparser(s, true)
	{
		enterSubparser(s);
		if (s->inputStart)
			s->inputStart (s);
		/* propagate the event recursively */
		notifyInputStart ();
		leaveSubparser();
	}
}

extern void notifyInputEnd   (void)
{
	subparser *s;

	foreachSubparser(s, true)
	{
		enterSubparser(s);
		/* propagate the event recursively */
		notifyInputEnd ();
		if (s->inputEnd)
			s->inputEnd (s);
		leaveSubparser();
	}

	/* for running sequel of optlib */
	langType lang = getInputLanguage ();
	notifyLanguageRegexInputEnd (lang);
}

extern void notifyMakeTagEntry (const tagEntryInfo *tag, int corkIndex)
{
	subparser *s;

	/* running optscript code attaching to --makeTagEntryReflection-<LANG> */
	langType lang = tag->langType;
	notifyLanguageRegexMakeTagEntry (lang, corkIndex);

	foreachSubparser(s, false)
	{
		enterSubparser(s);
		if (s->makeTagEntryNotify)
			s->makeTagEntryNotify (s, tag, corkIndex);
		/* propagate the event recursively */
		notifyMakeTagEntry (tag, corkIndex);
		leaveSubparser();
	}
}

extern langType getSubparserLanguage (subparser *s)
{
	return s->slaveParser->id;
}

extern void chooseExclusiveSubparser (subparser *s, void *data)
{
	if (s->exclusiveSubparserChosenNotify)
	{
		s->chosenAsExclusiveSubparser = true;
		enterSubparser(s);
		s->exclusiveSubparserChosenNotify (s, data);
		verbose ("%s is chosen as exclusive subparser\n",
				 getLanguageName (getSubparserLanguage (s)));
		leaveSubparser();
	}
}

extern subparser *getFirstSubparser(struct slaveControlBlock *controlBlock)
{
	if (controlBlock)
		return controlBlock->subparsersInUse;
	return NULL;
}

extern void useDefaultSubparsers (struct slaveControlBlock *controlBlock)
{
	controlBlock->subparsersInUse = controlBlock->subparsersDefault;
}

extern void useSpecifiedSubparser (struct slaveControlBlock *controlBlock, subparser *s)
{
	s->schedulingBaseparserExplicitly = true;
	controlBlock->subparsersInUse = s;
}

extern void setupSubparsersInUse (struct slaveControlBlock *controlBlock)
{
	if (!controlBlock->subparsersInUse)
		useDefaultSubparsers(controlBlock);
}

extern subparser* teardownSubparsersInUse (struct slaveControlBlock *controlBlock)
{
	subparser *tmp;
	subparser *s = NULL;

	tmp = controlBlock->subparsersInUse;
	controlBlock->subparsersInUse = NULL;

	if (tmp && tmp->schedulingBaseparserExplicitly)
	{
		tmp->schedulingBaseparserExplicitly = false;
		s = tmp;
	}

	if (s)
		return s;

	while (tmp)
	{
		if (tmp->chosenAsExclusiveSubparser)
		{
			s = tmp;
		}
		tmp = tmp->next;
	}

	return s;
}


static int subparserDepth;

extern void enterSubparser(subparser *subparser)
{
	subparserDepth++;
	pushLanguage (getSubparserLanguage (subparser));
}

extern void leaveSubparser(void)
{
	popLanguage ();
	subparserDepth--;
}

extern bool doesSubparserRun (void)
{
	if (getLanguageForBaseParser () == getInputLanguage())
		return false;
	return subparserDepth;
}

extern slaveParser *getFirstSlaveParser (struct slaveControlBlock *scb)
{
	if (scb)
		return scb->slaveParsers;
	return NULL;
}

extern subparser *getLanguageSubparser (langType sublang,
										bool including_none_crafted_parser)
{
	subparser *s;

	foreachSubparser (s, including_none_crafted_parser)
	{
		if (getSubparserLanguage (s) == sublang)
			return s;
	}
	return NULL;
}

extern struct colprintTable * subparserColprintTableNew (void)
{
	return colprintTableNew ("L:NAME", "L:BASEPARSER", "L:DIRECTIONS", NULL);
}

extern void subparserColprintAddSubparsers (struct colprintTable *table,
											struct slaveControlBlock *scb)
{
	slaveParser *tmp;

	pushLanguage (scb->owner);
	foreachSlaveParser(tmp)
	{
		if (tmp->type != DEPTYPE_SUBPARSER)
			continue;

		if (!isLanguageVisible (tmp->id))
			continue;

		struct colprintLine *line = colprintTableGetNewLine(table);

		colprintLineAppendColumnCString (line, getLanguageName (tmp->id));
		colprintLineAppendColumnCString (line, getLanguageName (scb->owner));

		const char *direction;
		switch (((subparser *)tmp->data)->direction)
		{
		case SUBPARSER_BASE_RUNS_SUB:
			direction = "base => sub {shared}";
			break;
		case SUBPARSER_SUB_RUNS_BASE:
			direction = "base <= sub {dedicated}";
			break;
		case SUBPARSER_BI_DIRECTION:
			direction = "base <> sub {bidirectional}";
			break;
		default:
			direction  = "UNKNOWN(INTERNAL BUG)";
			break;
		}
		colprintLineAppendColumnCString (line, direction);
	}
	popLanguage ();
}

static int subparserColprintCompareLines (struct colprintLine *a , struct colprintLine *b)
{
	const char *a_name = colprintLineGetColumn (a, 0);
	const char *b_name = colprintLineGetColumn (b, 0);

	int r;
	r = strcmp (a_name, b_name);
	if (r != 0)
		return r;

	const char *a_baseparser = colprintLineGetColumn (a, 1);
	const char *b_baseparser = colprintLineGetColumn (b, 1);

	return strcmp(a_baseparser, b_baseparser);
}

extern void subparserColprintTablePrint (struct colprintTable *table,
										 bool withListHeader, bool machinable, FILE *fp)
{
	colprintTableSort (table, subparserColprintCompareLines);
	colprintTablePrint (table, 0, withListHeader, machinable, fp);
}

extern const char *dependencyTypeString(enum eDepType e)
{ /* Generated by misc/enumstr.sh with cmdline:
     misc/enumstr.sh main/dependency.h eDepType dependencyTypeString DEPTYPE_  */
	switch (e)
	{
		case   DEPTYPE_KIND_OWNER: return "KIND_OWNER";
		case    DEPTYPE_SUBPARSER: return "SUBPARSER";
		case    DEPTYPE_FOREIGNER: return "FOREIGNER";
		case       COUNT_DEPTYPES: return "COUNT_DEPTYPES";
		default:                   return "UNKNOWN";
	}
}
