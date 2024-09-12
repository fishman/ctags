/*
*   Copyright (c) 1996-2003, Darren Hiebert
*
*   This source code is released into the public domain.
*
*   This module contains functions for reading tag files.
*/

#include "general.h"

#include "ctags.h"
#include "readtags.h"
#include "printtags.h"
#include "routines.h"
#include "routines_p.h"

#include "vstring.h"
#include "htable.h"
#include "ptrarray.h"
#include "fname.h"
#include "ptrarray.h"

#include <string.h>		/* strerror */
#include <stdlib.h>		/* exit */
#include <stdio.h>		/* stderr */
#include <stdbool.h>

typedef struct sReadOption {
	bool sortOverride;
	sortType sortMethod;
	/* options passed to libreadtags API functions.*/
	int matchOpts;
} readOptions;

struct canonWorkArea {
	struct canonFnameCacheTable *cacheTable;
	bool ptags;
	bool absoluteOnly;
};

typedef struct tagFileX {
	const char *fileName;
	tagFile *tagFile;
	tagFileInfo info;
} tagFileX;

struct inputSpec {
	const char *tagFileName;
	char *tempFileName;
};

enum actionType {
		ACTION_NONE,
		ACTION_FIND = 1 << 0,
		ACTION_LIST = 1 << 1,
		ACTION_LIST_PTAGS = 1 << 2,
};

struct actionSpec {
	unsigned int action;		/* bitset of actionType items */
	const char *name;			/* for ACTION_FIND */
	bool canonicalizing;
	struct canonWorkArea canon;
	ptrArray *tagEntryArray;
	void (* walkerfn) (const tagEntry *, void *);
	void *dataForWalkerFn;
};

static const char *ProgramName;
static int debugMode;
#ifdef READTAGS_DSL
#include "dsl/qualifier.h"
static QCode *Qualifier;
#include "dsl/sorter.h"
static SCode *Sorter;
#include "dsl/formatter.h"
static FCode *Formatter;
#endif

static const char* tagsStrerror (int err)
{
	if (err > 0)
		return strerror (err);
	else if (err < 0)
	{
		switch (err)
		{
		case TagErrnoUnexpectedSortedMethod:
			return "Unexpected sorted method";
		case TagErrnoUnexpectedFormat:
			return "Unexpected format number";
		case TagErrnoUnexpectedLineno:
			return "Unexpected value for line: field";
		case TagErrnoInvalidArgument:
			return "Unexpected argument passed to the API function";
		default:
			return "Unknown error";
		}
	}
	else
		return "no error";
}

static void printTag (const tagEntry *entry, void *data)
{
	tagsPrint (entry, (tagPrintOptions *)data, NULL, stdout);
}

#ifdef READTAGS_DSL
static void printTagWithFormatter (const tagEntry *entry, void *unused)
{
	f_print (entry, Formatter, stdout);
}
#endif

static void printPseudoTag (const tagEntry *entry, void *data)
{
	tagsPrintPseudoTag (entry, (tagPrintOptions *)data, NULL, stdout);
}

#ifdef READTAGS_DSL
static void freeCopiedTag (tagEntry *e)
{
	free ((void *)e->name);
	free ((void *)e->file);
	if (e->address.pattern)
		free ((void *)e->address.pattern);
	if (e->kind)
		free ((void *)e->kind);
	for (unsigned short c = 0; c < e->fields.count; c++)
	{
		free ((void *)e->fields.list[c].key);
		free ((void *)e->fields.list[c].value);
	}
	if (e->fields.count)
		free ((void *)e->fields.list);
	free ((void *)e);
}

static tagEntry *copyTag (tagEntry *o)
{
	tagEntry *n;

	n = eCalloc (1, sizeof  (*o));

	n->name = eStrdup (o->name);

	if (o->file)
		n->file = eStrdup (o->file);

	if (o->address.pattern)
		n->address.pattern = eStrdup (o->address.pattern);

	n->address.lineNumber = o->address.lineNumber;

	if (o->kind)
		n->kind = eStrdup (o->kind);

	n->fileScope = o->fileScope;
	n->fields.count = o->fields.count;

	if (o->fields.count == 0)
		return n;

	n->fields.list = eMalloc (o->fields.count * sizeof (*o->fields.list));

	for (unsigned short c = 0; c < o->fields.count; c++)
	{
		n->fields.list[c].key = eStrdup (o->fields.list[c].key);
		n->fields.list[c].value = eStrdup (o->fields.list[c].value);
	}

	return n;
}

static int compareTagEntry (const void *a, const void *b)
{
	return s_compare (a, b, Sorter);
}

static void walkTags (tagFile *const file, tagEntry *first_entry,
					  tagResult (* nextfn) (tagFile *const, tagEntry *),
					  void (* actionfn) (const tagEntry *, void *), void *data,
					  struct actionSpec *actionSpec)
{
	ptrArray *a = actionSpec->tagEntryArray;

	do
	{
		tagEntry *shadow = first_entry;
		tagEntry  shadowRec;
		if (actionSpec->canonicalizing
			&& (actionSpec->canon.ptags == false
				|| strcmp (first_entry->name, "!_TAG_PROC_CWD") == 0))
		{
			shadowRec = *first_entry;
			shadow = &shadowRec;
			shadow->file = canonicalizeFileName (actionSpec->canon.cacheTable,
												 first_entry->file);
		}

		if (Qualifier)
		{
			int i = q_is_acceptable (Qualifier, shadow);
			switch (i)
			{
			case Q_REJECT:
				continue;
			case Q_ERROR:
				exit (1);
			}
		}

		if (a)
		{
			tagEntry *e = copyTag (shadow);
			ptrArrayAdd (a, e);
		}
		else
			(* actionfn) (shadow, data);
	} while ( (*nextfn) (file, first_entry) == TagSuccess);

	int err = tagsGetErrno (file);
	if (err != 0)
	{
		fprintf (stderr, "%s: error in walkTags(): %s\n",
				 ProgramName,
				 tagsStrerror (err));
		exit (1);
	}

	if (a)
	{
		actionSpec->walkerfn = actionfn;
		actionSpec->dataForWalkerFn = data;
	}
}
#else
static void walkTags (tagFile *const file, tagEntry *first_entry,
					  tagResult (* nextfn) (tagFile *const, tagEntry *),
					  void (* actionfn) (const tagEntry *, void *), void *data,
					  struct actionSpec *actionSpec)
{
	do
	{
		tagEntry *shadow = first_entry;
		tagEntry  shadowRec;
		if (actionSpec->canonicalizing
			&& (actionSpec->canon.ptags == false
				|| strcmp (first_entry->name, "!_TAG_PROC_CWD") == 0))
		{
			shadow = &shadowRec;
			shadowRec = *first_entry;
			shadow->file = canonicalizeFileName (actionSpec->canon.cacheTable,
												 first_entry->file);
		}

		(* actionfn) (shadow, data);
	}
	while ( (*nextfn) (file, first_entry) == TagSuccess);

	int err = tagsGetErrno (file);
	if (err != 0)
	{
		fprintf (stderr, "%s: error in walkTags(): %s\n",
				 ProgramName,
				 tagsStrerror (err));
		exit (1);
	}
}
#endif

static int copyFile (FILE *in, FILE *out)
{
#define BUFSIZE (4096 * 10)
	static unsigned char buffer [BUFSIZE];

	while (1)
	{
		size_t r, t;

		r = fread (buffer, 1, BUFSIZE, in);
		if (!r)
		{
			if (ferror(in))
			{
				fprintf (stderr, "%s: error in reading from stdin\n", ProgramName);
				return -1;
			}
			/* EOF */
			break;
		}
		t = fwrite (buffer, 1, r, out);
		if (r != t)
		{
			fprintf (stderr, "%s error in writing to the temporarily file", ProgramName);
			return -1;
		}
	}
	return 0;
}

static const char *loadCtagsCWD (tagFileX *const fileX, tagEntry *pentry)
{
	if (tagsFindPseudoTag (fileX->tagFile, pentry, "!_TAG_PROC_CWD",
						   TAG_FULLMATCH) != TagSuccess)
	{
		int err = tagsGetErrno (fileX->tagFile);
		if (!err)
		{
			fprintf (stderr, "%s: no !_TAG_PROC_CWD in %s\n",
					 ProgramName, fileX->fileName);
			exit (1);
		}

		fprintf (stderr, "%s: cannot find !_TAG_PROC_CWD in %s: %s\n",
				 ProgramName, fileX->fileName, tagsStrerror (err));
		exit (1);
	}

	if (pentry->file[0] != '/')
	{
		fputs ("!_TAG_PROC_CWD must start with '/': ", stderr);
		tagsPrintValue (pentry->file, true, NULL, stderr);
		fputc ('\n', stderr);
		exit (1);
	}

	return pentry->file;
}

static struct canonFnameCacheTable *makeCanonFnameCacheTable (tagFileX *const fileX,
															  bool absoluteOnly)
{
	tagEntry pentry;
	const char *cwd = loadCtagsCWD (fileX, &pentry);
	return canonFnameCacheTableNew (cwd, absoluteOnly);
}

static tagFileX *makeTagFileX (const char *const filePath)
{
	tagFileX *fileX = eMalloc (sizeof (*fileX));
	fileX->fileName = eStrdup(filePath);
	fileX->tagFile = NULL;
	fileX->info = (tagFileInfo){0};
	return fileX;
}

static void deleteTagFileX (tagFileX *fileX)
{
	eFree ((void *)fileX->fileName);
	if (fileX->tagFile)
		tagsClose (fileX->tagFile);
	eFree (fileX);

}

static tagFileX *openTagFileX  (tagFileX *fileX)
{
	fileX->tagFile = tagsOpen (fileX->fileName, &fileX->info);
	return fileX;
}

static tagFileX *openTags (struct inputSpec *inputSpec)
{
	tagFileX *fileX;

	if (inputSpec->tempFileName)
		fileX = makeTagFileX(inputSpec->tempFileName);
	else if (strcmp (inputSpec->tagFileName, "-") == 0)
	{
		char *tempName = NULL;
		FILE *tempFP = tempFileFP ("w", &tempName);

		if (tempFP == NULL)
		{
			fprintf (stderr, "%s: failed to make a temporarily file for storing data from stdin\n",
					 ProgramName);
			exit (1);
		}

		fileX = makeTagFileX(tempName);
		inputSpec->tempFileName = tempName; /* Move the ownership. */
		tempName = NULL;					/* Don't touch this anymore. */

		if (copyFile (stdin, tempFP) < 0)
		{
			deleteTagFileX (fileX);
			fclose (tempFP);
			exit (1);
		}

		if (fflush (tempFP) < 0)
		{
			fprintf (stderr, "%s: failed to flush a temporarily file for storing data from stdin\n",
					 ProgramName);
			deleteTagFileX (fileX);
			fclose (tempFP);
			exit (1);
		}

		fclose (tempFP);
	}
	else
		fileX = makeTagFileX(inputSpec->tagFileName);

	return openTagFileX (fileX);
}

static int hasPsuedoTag (tagFile *const file,
						 const char *const ptag, const char *const exepectedValueAsInputField)
{
	tagEntry entry;

	return ((tagsFindPseudoTag (file, &entry,
								ptag, TAG_FULLMATCH) == TagSuccess)
			&& (strcmp(entry.file, exepectedValueAsInputField) == 0));
}

static void findTag (struct inputSpec *inputSpec,
					 const char *const name, readOptions *readOpts,
					 tagPrintOptions *printOpts, struct actionSpec *actionSpec)
{
	tagEntry entry;
	int err = 0;
	tagFileX *const fileX = openTags (inputSpec);
	if (fileX->tagFile == NULL || !fileX->info.status.opened)
	{
		fprintf (stderr, "%s: cannot open tag file: %s: %s\n",
				 ProgramName, tagsStrerror (fileX->info.status.error_number),
				 fileX->fileName);
		deleteTagFileX (fileX);
		exit (1);
	}

	if (actionSpec->canonicalizing && actionSpec->canon.cacheTable == NULL)
		actionSpec->canon.cacheTable = makeCanonFnameCacheTable (fileX, actionSpec->canon.absoluteOnly);

	if (printOpts->escaping)
	{
		printOpts->escapingInputField = false;
		if (hasPsuedoTag (fileX->tagFile, "!_TAG_OUTPUT_MODE", "u-ctags")
			&& hasPsuedoTag (fileX->tagFile, "!_TAG_OUTPUT_FILESEP", "slash"))
			printOpts->escapingInputField = true;
	}

	if (readOpts->sortOverride)
	{
		if (tagsSetSortType (fileX->tagFile, readOpts->sortMethod) != TagSuccess)
		{
			err = tagsGetErrno (fileX->tagFile);
			fprintf (stderr, "%s: cannot set sort type to %d: %s\n",
					 ProgramName,
					 readOpts->sortMethod,
					 tagsStrerror (err));
			exit (1);
		}
	}
	if (debugMode)
		fprintf (stderr, "%s: searching for \"%s\" in \"%s\"\n",
					 ProgramName, name, fileX->fileName);
	if (tagsFind (fileX->tagFile, &entry, name, readOpts->matchOpts) == TagSuccess)
		walkTags (fileX->tagFile, &entry, tagsFindNext,
#ifdef READTAGS_DSL
				  Formatter? printTagWithFormatter:
#endif
				  printTag, printOpts,
				  actionSpec);
	else if ((err = tagsGetErrno (fileX->tagFile)) != 0)
	{
		fprintf (stderr, "%s: error in tagsFind(): %s\n",
				 ProgramName,
				 tagsStrerror (err));
		exit (1);
	}
	deleteTagFileX (fileX);
}

static void listTags (struct inputSpec* inputSpec, bool pseudoTags, tagPrintOptions *printOpts,
					  struct actionSpec *actionSpec)
{
	tagEntry entry;
	int err = 0;
	tagFileX *const fileX = openTags (inputSpec);
	if (fileX->tagFile == NULL || !fileX->info.status.opened)
	{
		fprintf (stderr, "%s: cannot open tag file: %s: %s\n",
				 ProgramName,
				 tagsStrerror (fileX->info.status.error_number),
				 fileX->fileName);
		deleteTagFileX (fileX);
		exit (1);
	}

	if (actionSpec->canonicalizing && actionSpec->canon.cacheTable == NULL)
		actionSpec->canon.cacheTable = makeCanonFnameCacheTable (fileX,
																 actionSpec->canon.absoluteOnly);

	if (printOpts->escaping)
	{
		printOpts->escapingInputField = false;
		if (hasPsuedoTag (fileX->tagFile, "!_TAG_OUTPUT_MODE", "u-ctags")
			&& hasPsuedoTag (fileX->tagFile, "!_TAG_OUTPUT_FILESEP", "slash"))
			printOpts->escapingInputField = true;
	}

	if (pseudoTags)
	{
		if (tagsFirstPseudoTag (fileX->tagFile, &entry) == TagSuccess)
			walkTags (fileX->tagFile, &entry, tagsNextPseudoTag, printPseudoTag, printOpts,
					  actionSpec);
		else if ((err = tagsGetErrno (fileX->tagFile)) != 0)
		{
			fprintf (stderr, "%s: error in tagsFirstPseudoTag(): %s\n",
					 ProgramName,
					 tagsStrerror (err));
			exit (1);
		}
	}
	else
	{
		if (tagsFirst (fileX->tagFile, &entry) == TagSuccess)
			walkTags (fileX->tagFile, &entry, tagsNext,
#ifdef READTAGS_DSL
					  Formatter? printTagWithFormatter:
#endif
					  printTag, printOpts,
					  actionSpec);
		else if ((err = tagsGetErrno (fileX->tagFile)) != 0)
		{
			fprintf (stderr, "%s: error in tagsFirst(): %s\n",
					 ProgramName,
					 tagsStrerror (err));
			exit (1);
		}
	}
	deleteTagFileX (fileX);
}

static const char *const Usage =
	"Find tag file entries matching specified names.\n\n"
	"Usage: \n"
	"    %s -h | --help\n"
	"        Print this help message.\n"
#ifdef READTAGS_DSL
	"    %s -H POSTPROCESSOR | --help-expression POSTPROCESSOR\n"
	"        Print available terms that can be used in POSTPROCESSOR expression.\n"
	"        POSTPROCESSOR: filter sorter formatter\n"
#endif
	"    %s -v | --version\n"
	"        Print the version identifier.\n"
	"    %s [OPTIONS] ACTION\n"
	"        Do the specified action.\n"
	"Actions:\n"
	"    -l | --list\n"
	"        List regular tags.\n"
	"    [-] NAME...\n"
	"        List regular tags matching NAME(s).\n"
	"        \"-\" indicates arguments after this as NAME(s) even if they start with -.\n"
	"    -D | --list-pseudo-tags\n"
	"        List pseudo tags.\n"
	"Options:\n"
	"    -d | --debug\n"
	"        Turn on debugging output.\n"
	"    -E | --escape-output\n"
	"        Escape characters like tabs in output as described in tags(5).\n"
	"    -e | --extension-fields\n"
	"        Include extension fields in output.\n"
	"    -i | --icase-match\n"
	"        Perform case-insensitive matching in the NAME action.\n"
	"    -n | --line-number\n"
	"        Also include the line number field when -e option is given.\n"
	"    -p | --prefix-match\n"
	"        Perform prefix matching in the NAME action.\n"
	"    -P | --with-pseudo-tags\n"
	"        List pseudo tags as if -D option is specified but continues processing without exiting.\n"
	"    -t TAGFILE | --tag-file TAGFILE\n"
	"        Use specified tag file (default: \"tags\").\n"
	"        \"-\" indicates taking tag file data from standard input.\n"
	"    -s[0|1|2] | --override-sort-detection METHOD\n"
	"        Override sort detection of tag file.\n"
	"        METHOD: unsorted|sorted|foldcase\n"
	"    -C | --canonicalize-input\n"
	"        Reduct '..' and '.' in input fields.\n"
	"    -A | --absolute-input\n"
	"        Do the same as -C but use absolute path form\n"
#ifdef READTAGS_DSL
	"    -F EXP | --formatter EXP\n"
	"        Format the tags listed by ACTION with EXP when printing.\n"
	"    -Q EXP | --filter EXP\n"
	"        Filter the tags listed by ACTION with EXP before printing.\n"
	"    -S EXP | --sorter EXP\n"
	"        Sort the tags listed by ACTION with EXP before printing.\n"
#endif
	;

static void printUsage(FILE* stream, int exitCode)
{
	fprintf (stream, Usage, ProgramName,
#ifdef READTAGS_DSL
			 ProgramName,
#endif
			 ProgramName,
			 ProgramName);
	exit (exitCode);
}

#ifdef READTAGS_DSL
static void printFilterExpression (FILE *stream, int exitCode)
{
	fprintf (stream, "Filter expression: \n");
	q_help (stream);
	exit (exitCode);
}

static void printSorterExpression (FILE *stream, int exitCode)
{
	fprintf (stream, "Sorter expression: \n");
	s_help (stream);
	exit (exitCode);
}

static void printFormatterExpression (FILE *stream, int exitCode)
{
	fprintf (stream, "Formatter expression: \n");
	f_help (stream);
	exit (exitCode);
}

static void *compileExpression(const char* exp, void * (*compiler) (EsObject *),
							   const char *compiler_name)
{
	EsObject *sexp = es_read_from_string (exp, NULL);
	void *code;

	if (es_error_p (sexp))
	{
		fprintf (stderr,
				 "Failed to read the expression for %s: %s\n", compiler_name, exp);
		fprintf (stderr,
				 "Reason: %s\n", es_error_name (sexp));
		exit (1);
	}

	code = compiler (sexp);
	if (code == NULL)
	{
		fprintf (stderr,
				 "Failed to compile the expression of %s: %s\n", compiler_name, exp);
		exit (1);
	}
	es_object_unref (sexp);
	return code;
}
#endif

static void printVersion(void)
{
	/* readtags uses code of ctags via libutil.
	 * So we here use the versoin of ctags as the version of readtags. */
	puts(PROGRAM_VERSION);
	exit (0);
}

static void deleteInputSpec (void *data)
{
	struct inputSpec *inputSpec = data;
	if (inputSpec->tempFileName)
	{
		remove (inputSpec->tempFileName);
		eFree (inputSpec->tempFileName);
	}
	eFree (data);
}

static void addInputSpec (ptrArray *inputSpecs, const char *name)
{
	struct inputSpec *i = eMalloc (sizeof (struct inputSpec));

	i->tagFileName = name;
	i->tempFileName = NULL;

	ptrArrayAdd (inputSpecs, i);
}

extern int main (int argc, char **argv)
{
	int i;
	bool ignore_prefix = false;

	tagPrintOptions printOpts = {0};
	readOptions readOpts = {0};
	ptrArray *inputSpecs = ptrArrayNew (deleteInputSpec);

	struct actionSpec actionSpec = {
		.action  = ACTION_NONE,
		.name = NULL,
		.canonicalizing = false,
		.canon = {
			.cacheTable = NULL,
			.ptags = false,
			/* .absoluteOnly = false, */
		},
		.tagEntryArray = NULL,
		.walkerfn = NULL,
		.dataForWalkerFn = NULL,
	};

	ProgramName = argv [0];
	setExecutableName (ProgramName);
	if (argc == 1)
		printUsage(stderr, 1);
	for (i = 1  ;  i < argc  ;  ++i)
	{
		const char *const arg = argv [i];
		if (ignore_prefix || arg [0] != '-')
		{
			actionSpec.action |= ACTION_FIND;
			actionSpec.name = arg;
		}
		else if (arg [0] == '-' && arg [1] == '\0')
			ignore_prefix = true;
		else if (arg [0] == '-' && arg [1] == '-')
		{
			const char *optname = arg + 2;
			if (strcmp (optname, "debug") == 0)
				debugMode++;
			else if (strcmp (optname, "list-pseudo-tags") == 0
					 || strcmp (optname, "with-pseudo-tags") == 0)
				actionSpec.action |= ACTION_LIST_PTAGS;
			else if (strcmp (optname, "help") == 0)
				printUsage (stdout, 0);
#ifdef READTAGS_DSL
			else if (strcmp (optname, "help-expression") == 0)
			{
				if (i + 1 < argc)
				{
					const char *exp_klass = argv [++i];
					if (strcmp (exp_klass, "filter") == 0)
						printFilterExpression (stdout, 0);
					if (strcmp (exp_klass, "sorter") == 0)
						printSorterExpression (stdout, 0);
					if (strcmp (exp_klass, "formatter") == 0)
						printFormatterExpression (stdout, 0);
					else
					{
						fprintf (stderr, "%s: unknown expression class for --%s option\n",
								 ProgramName, optname);
						exit (1);

					}
				}
				else
				{
					fprintf (stderr, "%s: missing expression class for --%s option\n",
							 ProgramName, optname);
					exit (1);
				}
			}
#endif
			else if (strcmp (optname, "version") == 0)
				printVersion ();
			else if (strcmp (optname, "escape-output") == 0)
				printOpts.escaping = true;
			else if (strcmp (optname, "extension-fields") == 0)
				printOpts.extensionFields = true;
			else if (strcmp (optname, "icase-match") == 0)
				readOpts.matchOpts |= TAG_IGNORECASE;
			else if (strcmp (optname, "prefix-match") == 0)
				readOpts.matchOpts |= TAG_PARTIALMATCH;
			else if (strcmp (optname, "list") == 0)
				actionSpec.action |= ACTION_LIST;
			else if (strcmp (optname, "line-number") == 0)
				printOpts.lineNumber = true;
			else if (strcmp (optname, "tag-file") == 0)
			{
				if (i + 1 < argc)
					addInputSpec (inputSpecs, argv[++i]);
				else
					printUsage (stderr, 1);
			}
			else if (strcmp (optname, "override-sort-detection") == 0)
			{
				if (i + 1 < argc)
				{
					const char *sort_spec = argv [++i];
					if (strcmp (sort_spec, "0") == 0
						|| strcmp (sort_spec, "unsorted") == 0)
						readOpts.sortMethod = TAG_UNSORTED;
					else if (strcmp (sort_spec, "1") == 0
							 || strcmp (sort_spec, "sorted") == 0)
						readOpts.sortMethod = TAG_SORTED;
					else if (strcmp (sort_spec, "2") == 0
							 || strcmp (sort_spec, "foldcase") == 0)
						readOpts.sortMethod = TAG_FOLDSORTED;
					else
					{
						fprintf (stderr, "%s: unknown sort method for --%s option\n",
								 ProgramName, optname);
						exit (1);
					}
				}
				else
				{
					fprintf (stderr, "%s: missing sort method for --%s option\n",
							 ProgramName, optname);
					exit (1);
				}
			}
			else if (strcmp (optname, "absolute-input") == 0)
			{
				actionSpec.canonicalizing = true;
				actionSpec.canon.absoluteOnly = true;
			}
			else if (strcmp (optname, "canonicalize-input") == 0)
			{
				actionSpec.canonicalizing = true;
				actionSpec.canon.absoluteOnly = false;
			}
#ifdef READTAGS_DSL
			else if (strcmp (optname, "filter") == 0)
			{
				if (i + 1 < argc)
					Qualifier = compileExpression (argv[++i],
												   (void * (*)(EsObject *))q_compile,
												   optname);
				else
				{
					fprintf (stderr, "%s: missing filter expression for --%s option\n",
							 ProgramName, optname);
					exit (1);
				}
			}
			else if (strcmp (optname, "sorter") == 0)
			{
				if (i + 1 < argc)
					Sorter = compileExpression (argv[++i],
												(void * (*)(EsObject *))s_compile,
												optname);
				else
				{
					fprintf (stderr, "%s: missing sorter expression for --%s option\n",
							 ProgramName, optname);
					exit (1);
				}
			}
			else if (strcmp (optname, "formatter") == 0)
			{
				if (i + 1 < argc)
					Formatter = compileExpression (argv[++i],
												   (void * (*)(EsObject *))f_compile,
												   optname);
				else
				{
					fprintf (stderr, "%s: missing formatter expression for --%s option\n",
							 ProgramName, optname);
					exit (1);
				}
			}
#endif
			else
			{
				fprintf (stderr, "%s: unknown long options: --%s\n",
						 ProgramName, optname);
				exit (1);
				break;
			}
		}
		else
		{
			size_t j;
			for (j = 1  ;  arg [j] != '\0'  ;  ++j)
			{
				switch (arg [j])
				{
					case 'd': debugMode++; break;
					case 'D':
					case 'P':
						actionSpec.action |= ACTION_LIST_PTAGS;
						break;
					case 'h': printUsage (stdout, 0); break;
#ifdef READTAGS_DSL
					case 'H':
						if (i + 1 < argc)
						{
							const char *exp_klass = argv [++i];
							if (strcmp (exp_klass, "filter") == 0)
								printFilterExpression (stdout, 0);
							else if (strcmp (exp_klass, "sorter") == 0)
								printSorterExpression (stdout, 0);
							else if (strcmp (exp_klass, "formatter") == 0)
								printFormatterExpression (stdout, 0);
							else
								printUsage(stderr, 1);
						}
						else
							printUsage(stderr, 1);
#endif
					case 'v': printVersion ();
					case 'E': printOpts.escaping = true; break;
					case 'e': printOpts.extensionFields = true; break;
					case 'i': readOpts.matchOpts |= TAG_IGNORECASE;   break;
					case 'p': readOpts.matchOpts |= TAG_PARTIALMATCH; break;
					case 'l':
						actionSpec.action |= ACTION_LIST;
						break;
					case 'n': printOpts.lineNumber = true; break;
					case 't':
						if (arg [j+1] != '\0')
						{
							const char *tname = arg + j + 1;
							addInputSpec (inputSpecs, tname);
							j += strlen (tname);
						}
						else if (i + 1 < argc)
							addInputSpec (inputSpecs, argv [++i]);
						else
							printUsage(stderr, 1);
						break;
					case 's':
						readOpts.sortOverride = true;
						++j;
						if (arg [j] == '\0')
							readOpts.sortMethod = TAG_SORTED;
						else if (strchr ("012", arg[j]) != NULL)
							readOpts.sortMethod = (sortType) (arg[j] - '0');
						else
							printUsage(stderr, 1);
						break;
					case 'A':
						actionSpec.canonicalizing = true;
						actionSpec.canon.absoluteOnly = true;
						break;
					case 'C':
						actionSpec.canonicalizing = true;
						actionSpec.canon.absoluteOnly = false;
						break;
#ifdef READTAGS_DSL
					case 'Q':
						if (i + 1 == argc)
							printUsage(stderr, 1);
						Qualifier = compileExpression (argv[++i],
													   (void * (*)(EsObject *))q_compile,
													   "filter");
						break;
					case 'S':
						if (i + 1 == argc)
							printUsage(stderr, 1);
						Sorter = compileExpression (argv[++i],
													   (void * (*)(EsObject *))s_compile,
													   "sorter");
						break;
					case 'F':
						if (i + 1 == argc)
							printUsage(stderr, 1);
						Formatter = compileExpression (argv[++i],
													   (void * (*)(EsObject *))f_compile,
													   "formatter");
						break;
#endif
					default:
						fprintf (stderr, "%s: unknown option: %c\n",
									ProgramName, arg[j]);
						exit (1);
						break;
				}
			}
		}
	}


	if (actionSpec.action == ACTION_NONE)
	{
		fprintf (stderr,
			"%s: no action specified: specify one of NAME, -l or -D\n",
			ProgramName);
		exit (1);
	}

	if ((actionSpec.action & ACTION_FIND) && (actionSpec.action & ACTION_LIST))
	{
		fprintf (stderr,
				 "%s: choose either an action: finding a tag or listing all\n",
				 ProgramName);
		exit (1);
	}

	size_t input_count = ptrArrayCount(inputSpecs);
#ifndef READTAGS_DSL
	if (input_count > 0)
	{
		/* mk_mingw.mak sets -DREADTAGS_DSL.
		 * mk_mvc.mak doesn't. */
		fprintf (stderr,
				 "Processsing multiple tags is not supported with this (!READTAGS_DSL) build confiugraion.");
		exit (1);
	}
#endif

#ifdef READTAGS_DSL
	if (Sorter || input_count  > 0)
		actionSpec.tagEntryArray = ptrArrayNew ((ptrArrayDeleteFunc)freeCopiedTag);
#endif

	if (ptrArrayIsEmpty (inputSpecs))
	{
		addInputSpec (inputSpecs, "tags");
		input_count++;
	}

	for (unsigned int i = 0; i < input_count; i++)
	{
		struct inputSpec *inputSpec = ptrArrayItem (inputSpecs, i);

		if (actionSpec.action & ACTION_LIST_PTAGS)
		{
			if (actionSpec.canonicalizing)
				actionSpec.canon.ptags = true;
			listTags (inputSpec, true, &printOpts, &actionSpec);
			if (actionSpec.canonicalizing)
				actionSpec.canon.ptags = false;
		}

		if (actionSpec.action & ACTION_FIND)
			findTag (inputSpec, actionSpec.name, &readOpts, &printOpts, &actionSpec);
		else if (actionSpec.action & ACTION_LIST)
			listTags (inputSpec, false, &printOpts, &actionSpec);

		if (actionSpec.canon.cacheTable)
		{
			canonFnameCacheTableDelete (actionSpec.canon.cacheTable);
			actionSpec.canon.cacheTable = NULL;
		}
	}

	ptrArrayDelete (inputSpecs);

	if (actionSpec.tagEntryArray)
	{
#ifdef READTAGS_DSL
		if (Sorter)
			ptrArraySort (actionSpec.tagEntryArray, compareTagEntry);
#endif

		const size_t entry_count = ptrArrayCount(actionSpec.tagEntryArray);
		for (unsigned int i = 0; i < entry_count; i++)
		{
			tagEntry *e = ptrArrayItem (actionSpec.tagEntryArray, i);
			actionSpec.walkerfn (e, actionSpec.dataForWalkerFn);
		}
		ptrArrayDelete (actionSpec.tagEntryArray);
	}

#ifdef READTAGS_DSL
	if (Qualifier)
		q_destroy (Qualifier);
	if (Sorter)
		s_destroy (Sorter);
	if (Formatter)
		f_destroy (Formatter);
#endif

	return 0;
}
