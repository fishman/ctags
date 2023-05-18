/*
*   Copyright (c) 2000-2003, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for applying regular expression matching.
*
*   The code for utilizing the Gnu regex package with regards to processing the
*   regex option and checking for regex matches was adapted from routines in
*   Gnu etags.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include <string.h>

#include <ctype.h>
#include <stddef.h>
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>  /* declare off_t (not known to regex.h on FreeBSD) */
#endif

#include <inttypes.h>

#include "debug.h"
#include "colprint_p.h"
#include "entry_p.h"
#include "field_p.h"
#include "flags_p.h"
#include "htable.h"
#include "kind.h"
#include "options.h"
#include "optscript.h"
#include "parse_p.h"
#include "promise.h"
#include "read.h"
#include "read_p.h"
#include "routines.h"
#include "routines_p.h"
#include "script_p.h"
#include "trace.h"
#include "trashbox.h"
#include "xtag_p.h"

static bool regexAvailable = false;

/*
*   MACROS
*/

/* The max depth of taction=enter/leave stack */
#define MTABLE_STACK_MAX_DEPTH 64

/* How many times ctags allows a mtable parser
   stays at the same input position across table switching.

   The value is derived from MTABLE_STACK_MAX_DEPTH.
   No deep meaning is in that. It just for simplifying
   Tmain cases. */
#define MTABLE_MOTIONLESS_MAX (MTABLE_STACK_MAX_DEPTH + 1)

#define DEFAULT_REGEX_BACKEND "e"

/*
*   DATA DECLARATIONS
*/

enum pType { PTRN_TAG, PTRN_CALLBACK };

enum scopeAction {
	SCOPE_REF     = 1UL << 0,
	SCOPE_POP     = 1UL << 1,
	SCOPE_PUSH    = 1UL << 2,
	SCOPE_CLEAR   = 1UL << 3,
	SCOPE_REF_AFTER_POP = 1UL << 4,
	SCOPE_PLACEHOLDER = 1UL << 5,
	SCOPE_INTERVALTAB = 1UL << 6,
};

enum tableAction {
	TACTION_NOP,
	TACTION_ENTER,				/* {tenter=N} */
	TACTION_LEAVE,				/* {tleave} */
	TACTION_JUMP,					/* {tjump=N} */
	TACTION_RESET,				/* {treset=N} */
	TACTION_QUIT,					/* {tquit} */
};

struct fieldPattern {
	fieldType ftype;
	const char *template;
};

struct boundarySpec {
	int patternGroup;
	bool fromStartOfGroup;
	bool placeholder;
};

struct guestLangSpec {
	enum guestLangSpecType {
		GUEST_LANG_UNKNOWN,
		GUEST_LANG_PLACEHOLDER,			   /* _ */
		GUEST_LANG_STATIC_LANGNAME,		   /* C, Python,... */
		GUEST_LANG_PTN_GROUP_FOR_LANGNAME, /* \1, \2, ..., \9 */
		GUEST_LANG_PTN_GROUP_FOR_FILEMAP, /* *1, *2, ... *9 */
	} type;
	union {
		langType lang;
		int patternGroup;
	} spec;
};

struct guestSpec {
	struct guestLangSpec lang;
#define GUEST_BOUNDARY_START 0
#define GUEST_BOUNDARY_END  1
	struct boundarySpec boundary[2];
};

struct mGroupSpec {
#define NO_MULTILINE -1
	int forLineNumberDetermination;
	int forNextScanning;
	/* true => start, false => end */
	bool nextFromStart;
};

struct mTableActionSpec {
	enum tableAction action;
	struct regexTable *table;

	/* used when action == TACTION_ENTER */
	struct regexTable *continuation_table;
};

typedef struct {
	regexCompiledCode pattern;
	enum pType type;
	bool exclusive;
	bool accept_empty_name;
	bool postrun;
	union {
		struct {
			int kindIndex;
			roleBitsType roleBits;
			char *name_pattern;
		} tag;
		struct {
			regexCallback function;
			void *userData;
		} callback;
	} u;
	unsigned int scopeActions;
	bool *disabled;

	enum regexParserType regptype;
	struct mGroupSpec mgroup;
	struct guestSpec guest;
	struct mTableActionSpec taction;

	int   xtagType;
	ptrArray *fieldPatterns;

	char *pattern_string;

	char *anonymous_tag_prefix;
	langType foreign_lang;

	struct {
		errorSelection selection;
		char *message_string;
	} message;

	char *optscript_src;
	EsObject *optscript;

	int refcount;
} regexPattern;


typedef struct {
	/* the pattern can be shared among entries using a refcount */
	regexPattern *pattern;

	/* but the statistics are per-table-entry */
	struct {
		unsigned int match;
		unsigned int unmatch;
	} statistics;
} regexTableEntry;


#define TABLE_INDEX_UNUSED -1
struct regexTable {
	char *name;
	ptrArray *entries;
};

struct boundaryInRequest {
	bool offset_set;
	off_t offset;
};

struct guestRequest {
	bool lang_set;
	langType lang;

	struct boundaryInRequest boundary[2];
};

typedef struct {
	const char *line;
	const char *start;
	const regexPattern* const patbuf;
	const regmatch_t* const pmatch;
	int nmatch;
	struct mTableActionSpec taction;
	bool advanceto;
	unsigned int advanceto_delta;
} scriptWindow;

struct lregexControlBlock {
	int currentScope;
	ptrArray *entries [2];

	ptrArray *tables;
	ptrArray *tstack;

	struct guestRequest *guest_req;

	EsObject *local_dict;
	hashTable*param_dict;

	ptrArray *hook[SCRIPT_HOOK_MAX];
	ptrArray *hook_code[SCRIPT_HOOK_MAX];

	langType owner;
};

typedef struct {
	struct lregexControlBlock *lcb;
	scriptWindow *window;
} appData;

/*
*   DATA DEFINITIONS
*/
static OptVM *optvm;
static EsObject *lregex_dict = es_nil;

/*
*   FUNCTION DEFINITIONS
*/
static int getTableIndexForName (const struct lregexControlBlock *const lcb, const char *name);
static void deletePattern (regexPattern *p);
static int  makePromiseForAreaSpecifiedWithOffsets (const char *parser,
													off_t startOffset,
													off_t endOffset);

static struct guestRequest *guestRequestNew (void);
static void   guestRequestDelete (struct guestRequest *);
static bool   guestRequestIsFilled(struct guestRequest *);
static void   guestRequestClear (struct guestRequest *);
static void   guestRequestSubmit (struct guestRequest *);

static EsObject *scriptRead (OptVM *vm, const char *src);
static void scriptSetup (OptVM *vm, struct lregexControlBlock *lcb, int corkIndex, scriptWindow *window);
static EsObject* scriptEval (OptVM *vm, EsObject *optscript);
static void scriptEvalHook (OptVM *vm, struct lregexControlBlock *lcb, enum scriptHook hook);
static void scriptTeardown (OptVM *vm, struct lregexControlBlock *lcb);

static char* make_match_string (scriptWindow *window, int group);
static matchLoc *make_mloc (scriptWindow *window, int group, bool start);

static struct lregexControlBlock *get_current_lcb(OptVM *vm)
{
	appData * d = opt_vm_get_app_data (vm);
	return d->lcb;
}

static scriptWindow *get_current_window(OptVM *vm)
{
	appData * d = opt_vm_get_app_data (vm);
	return d->window;
}

static struct lregexControlBlock *set_current_lcb(OptVM *vm, struct lregexControlBlock *new_lcb)
{
	appData * d = opt_vm_get_app_data (vm);
	struct lregexControlBlock *old_lcb = d->lcb;
	d->lcb = new_lcb;
	return old_lcb;
}

static scriptWindow *set_current_window(OptVM *vm, scriptWindow *new_windown)
{
	appData * d = opt_vm_get_app_data (vm);
	scriptWindow *old_window = d->window;
	d->window = new_windown;
	return old_window;
}

static void deleteTable (void *ptrn)
{
	struct regexTable *t = ptrn;

	ptrArrayDelete (t->entries);
	eFree (t->name);
	eFree (t);
}

static void deleteTableEntry (void *ptrn)
{
	regexTableEntry *e = ptrn;
	Assert (e && e->pattern);
	deletePattern (e->pattern);
	eFree (e);
}

static void deletePattern (regexPattern *p)
{
	p->refcount--;

	if (p->refcount > 0)
		return;

	p->pattern.backend->delete_code (p->pattern.code);

	if (p->type == PTRN_TAG)
	{
		eFree (p->u.tag.name_pattern);
		p->u.tag.name_pattern = NULL;
	}

	if (p->fieldPatterns)
	{
		ptrArrayDelete (p->fieldPatterns);
		p->fieldPatterns = NULL;
	}

	eFree (p->pattern_string);

	if (p->message.message_string)
		eFree (p->message.message_string);

	if (p->anonymous_tag_prefix)
		eFree (p->anonymous_tag_prefix);

	if (p->optscript)
		es_object_unref (p->optscript);
	if (p->optscript_src)
		eFree (p->optscript_src);

	eFree (p);
}

static void clearPatternSet (struct lregexControlBlock *lcb)
{
	ptrArrayClear (lcb->entries [REG_PARSER_SINGLE_LINE]);
	ptrArrayClear (lcb->entries [REG_PARSER_MULTI_LINE]);
	ptrArrayClear (lcb->tables);
}

extern struct lregexControlBlock* allocLregexControlBlock (parserDefinition *parser)
{
	struct lregexControlBlock *lcb = xCalloc (1, struct lregexControlBlock);

	lcb->entries[REG_PARSER_SINGLE_LINE] = ptrArrayNew(deleteTableEntry);
	lcb->entries[REG_PARSER_MULTI_LINE] = ptrArrayNew(deleteTableEntry);
	lcb->tables = ptrArrayNew(deleteTable);
	lcb->tstack = ptrArrayNew(NULL);
	lcb->guest_req = guestRequestNew ();
	lcb->local_dict = es_nil;
	lcb->param_dict = hashTableNew (3, hashCstrhash, hashCstreq,
									eFree, eFree);

	for (int i = 0; i< SCRIPT_HOOK_MAX; i++)
	{
		lcb->hook[i] = ptrArrayNew (eFree);
		lcb->hook_code[i] = ptrArrayNew ((ptrArrayDeleteFunc)es_object_unref);
	}
	lcb->owner = parser->id;

	return lcb;
}

extern void freeLregexControlBlock (struct lregexControlBlock* lcb)
{
	clearPatternSet (lcb);

	ptrArrayDelete (lcb->entries [REG_PARSER_SINGLE_LINE]);
	lcb->entries [REG_PARSER_SINGLE_LINE] = NULL;
	ptrArrayDelete (lcb->entries [REG_PARSER_MULTI_LINE]);
	lcb->entries [REG_PARSER_MULTI_LINE] = NULL;

	ptrArrayDelete (lcb->tables);
	lcb->tables = NULL;

	ptrArrayDelete (lcb->tstack);
	lcb->tstack = NULL;

	guestRequestDelete (lcb->guest_req);
	lcb->guest_req = NULL;

	es_object_unref (lcb->local_dict);
	lcb->local_dict = es_nil;

	hashTableDelete (lcb->param_dict);
	lcb->param_dict = NULL;

	for (int i = 0; i < SCRIPT_HOOK_MAX; i++)
	{
		ptrArrayDelete (lcb->hook[i]);
		lcb->hook[i] = NULL;

		ptrArrayDelete (lcb->hook_code[i]);
		lcb->hook_code[i] = NULL;
	}

	eFree (lcb);
}

/*
*   Regex pseudo-parser
*/

static void initRegexTag (tagEntryInfo *e,
		const char * name, int kindIndex, int roleIndex, int scopeIndex, int placeholder,
		unsigned long line, MIOPos *pos, int xtag_type, langType foreign_lang)
{
	Assert (name != NULL  &&  ((name[0] != '\0') || placeholder));

	if (foreign_lang == LANG_IGNORE)
		initRefTagEntry (e, name, kindIndex, roleIndex);
	else
		initForeignRefTagEntry (e, name, foreign_lang, kindIndex, roleIndex);
	e->extensionFields.scopeIndex = scopeIndex;
	markTagAsPlaceholder(e, placeholder);
	if (line)
		updateTagLine(e, line, *pos);

	if (xtag_type != XTAG_UNKNOWN)
		markTagExtraBit (e, xtag_type);
}

/*
*   Regex pattern definition
*/

/* Take a string like "/blah/" and turn it into "blah", making sure
 * that the first and last characters are the same, and handling
 * quoted separator characters.  Actually, stops on the occurrence of
 * an unquoted separator.  Also turns "\t" into a Tab character.
 * Turns "\n" into a Newline character if MULTILINE is true.
 * Returns pointer to terminating separator.  Works in place.  Null
 * terminates name string.
 */
static char* scanSeparators (char* name, bool multiline)
{
	char sep = name [0];
	char *copyto = name;
	bool quoted = false;

	for (++name ; *name != '\0' ; ++name)
	{
		if (quoted)
		{
			if (*name == sep)
				*copyto++ = sep;
			else if (*name == 't')
				*copyto++ = '\t';
			else if (multiline && *name == 'n')
				*copyto++ = '\n';
			else
			{
				/* Something else is quoted, so preserve the quote. */
				*copyto++ = '\\';
				*copyto++ = *name;
			}
			quoted = false;
		}
		else if (*name == '\\')
			quoted = true;
		else if (*name == sep)
		{
			break;
		}
		else
			*copyto++ = *name;
	}
	*copyto = '\0';
	return name;
}

/* Parse `regexp', in form "/regex/name/[k,Kind/]flags" (where the separator
 * character is whatever the first character of `regexp' is), by breaking it
 * up into null terminated strings, removing the separators, and expanding
 * '\t' into tabs. When complete, `regexp' points to the line matching
 * pattern, a pointer to the name matching pattern is written to `name', a
 * pointer to the kinds is written to `kinds' (possibly NULL), and a pointer
 * to the trailing flags is written to `flags'. If the pattern is not in the
 * correct format, a false value is returned.
 */
static bool parseTagRegex (
		enum regexParserType regptype,
		char* const regexp, char** const name,
		char** const kinds, char** const flags)
{
	bool result = false;
	const int separator = (unsigned char) regexp [0];

	*name = scanSeparators (regexp, (regptype == REG_PARSER_MULTI_LINE
									 || regptype == REG_PARSER_MULTI_TABLE));
	if (*regexp == '\0')
		error (WARNING, "empty regexp");
	else if (**name != separator)
		error (WARNING, "%s: incomplete regexp", regexp);
	else
	{
		char* const third = scanSeparators (*name, false);
		if (**name != '\0' && (*name) [strlen (*name) - 1] == '\\')
			error (WARNING, "error in name pattern: \"%s\"", *name);
		if (*third != separator)
			error (WARNING, "%s: regexp missing final separator", regexp);
		else
		{
			/*
			 * first----------V third------------V
			 * --regex-<LANG>=/regexp/replacement/[kind-spec/][flags][{{\n...\n}}]
			 * second----------------^ fourth---------------^
			 */

			/*
			 * The following code assumes "{{\n" is never used in flags.
			 * If the input comes from the command line or an optlib file,
			 * this assumption is always correct; a new line character is never
			 * put at the middle (or end) of the input.
			 *
			 * TODO: How about the input comes from the source code translated
			 * by optlib2c?
			 */
			char *script = strstr (third, "{{\n");
			if (script)
			{
				/* The script part should not be unescaed by scanSeparators().
				 * By spitting the string, we can hide the script part from
				 * scanSeparators(). */
				script [0] = '\0';
			}

			char* const fourth = scanSeparators (third, false);
			if (*fourth == separator)
			{
				*kinds = third;
				scanSeparators (fourth, false);
				*flags = fourth;
			}
			else
			{
				*flags = third;
				*kinds = NULL;
			}

			if (script)
			{
				Assert (*flags);

				char *end = *flags + strlen (*flags);
				script [0] = '{';
				if (end != script)
				{
					size_t len = strlen (script);
					memmove (end, script, len);
					end [len] = '\0';
				}
			}

			result = true;
		}
	}
	return result;
}


static void pre_ptrn_flag_exclusive_short (char c CTAGS_ATTR_UNUSED, void* data)
{
	regexPattern * ptrn = data;
	ptrn->exclusive = true;
}

static void pre_ptrn_flag_exclusive_long (const char* const s CTAGS_ATTR_UNUSED, const char* const unused CTAGS_ATTR_UNUSED, void* data)
{
	pre_ptrn_flag_exclusive_short ('x', data);
}

static void pre_ptrn_flag_postrun_long (const char* const s CTAGS_ATTR_UNUSED, const char* const unused CTAGS_ATTR_UNUSED, void* data)
{
	regexPattern * ptrn = data;
	ptrn->postrun = true;
}

static flagDefinition prePtrnFlagDef[] = {
	{ 'x',  "exclusive", pre_ptrn_flag_exclusive_short, pre_ptrn_flag_exclusive_long ,
	  NULL, "skip testing the other patterns if a line is matched to this pattern"},
	{ '\0', "postrun",   NULL, pre_ptrn_flag_postrun_long,
	  NULL, "run after parsing with built-in code, multline regex patterns, and multitable regex patterns" },
};

static void scope_ptrn_flag_eval (const char* const f  CTAGS_ATTR_UNUSED,
				  const char* const v, void* data)
{
	unsigned int *bfields = data;

	if (strcmp (v, "ref") == 0)
		*bfields |= SCOPE_REF;
	else if (strcmp (v, "push") == 0)
		*bfields |= (SCOPE_PUSH | SCOPE_REF);
	else if (strcmp (v, "pop") == 0)
		*bfields |= SCOPE_POP;
	else if (strcmp (v, "clear") == 0)
		*bfields |= SCOPE_CLEAR;
	else if (strcmp (v, "set") == 0)
		*bfields |= (SCOPE_CLEAR | SCOPE_PUSH);
	else if (strcmp (v, "replace") == 0)
		*bfields |= (SCOPE_POP|SCOPE_REF_AFTER_POP|SCOPE_PUSH);
	else if (strcmp (v, "intervaltab") == 0)
		*bfields |= SCOPE_INTERVALTAB;
	else
		error (FATAL, "Unexpected value for scope flag in regex definition: scope=%s", v);
}

static void placeholder_ptrn_flag_eval (const char* const f  CTAGS_ATTR_UNUSED,
				     const char* const v  CTAGS_ATTR_UNUSED, void* data)
{
	unsigned int *bfields = data;
	*bfields |= SCOPE_PLACEHOLDER;
}

static flagDefinition scopePtrnFlagDef[] = {
	{ '\0', "scope",     NULL, scope_ptrn_flag_eval,
	  "ACTION", "use scope stack: ACTION = ref|push|pop|clear|set|replace|intervaltab"},
	{ '\0', "placeholder",  NULL, placeholder_ptrn_flag_eval,
	  NULL, "don't put this tag to tags file."},
};

static void intervaltab_ptrn_flag_eval (const char* const f  CTAGS_ATTR_UNUSED,
										const char* const v, void* data)
{
	unsigned int *bfields = data;
	if (strcmp (v, "intervaltab") != 0)
	{
		error (WARNING, "Unexpected value for scope flag in mline regex definition: scope=%s", v);
		error (WARNING, "NOTE: --mline-regex-<LANG> accepts only {scope=intervaltab}");
		return;
	}

	*bfields |= SCOPE_INTERVALTAB;
}

static flagDefinition intervaltabPtrnFlagDef[] = {
	{ '\0', "scope",     NULL, intervaltab_ptrn_flag_eval,
	  "intervaltab", "set scope for the tag with the interval table" },
};

static kindDefinition *kindNew (char letter, const char *name, const char *description)
{
	kindDefinition *kdef = xCalloc (1, kindDefinition);
	kdef->letter        = letter;
	kdef->name = eStrdup (name);
	kdef->description = eStrdup(description? description: kdef->name);
	kdef->enabled = true;
	return kdef;
}

static void kindFree (kindDefinition *kind)
{
	kind->letter = '\0';
	eFree ((void *)kind->name);
	kind->name = NULL;
	eFree ((void *)kind->description);
	kind->description = NULL;
	eFree (kind);
}

static void initMgroup(struct mGroupSpec *mgroup)
{
	mgroup->forLineNumberDetermination = NO_MULTILINE;
	mgroup->forNextScanning = NO_MULTILINE;
	mgroup->nextFromStart = false;
}

static void initGuestSpec (struct guestSpec *guest)
{
	guest->lang.type = GUEST_LANG_UNKNOWN;
}

static void initTaction(struct mTableActionSpec *taction)
{
	taction->action = TACTION_NOP;
	taction->table = NULL;
}

static regexPattern * refPattern (regexPattern * ptrn)
{
	ptrn->refcount++;
	return ptrn;
}

static regexPattern * newPattern (regexCompiledCode* const pattern,
								  enum regexParserType regptype)
{
	regexPattern *ptrn = xCalloc(1, regexPattern);

	ptrn->pattern.backend = pattern->backend;
	ptrn->pattern.code = pattern->code;

	ptrn->exclusive = false;
	ptrn->postrun = false;
	ptrn->accept_empty_name = false;
	ptrn->regptype = regptype;
	ptrn->xtagType = XTAG_UNKNOWN;

	if (regptype == REG_PARSER_MULTI_LINE)
		initMgroup(&ptrn->mgroup);
	if (regptype == REG_PARSER_MULTI_TABLE)
		initTaction(&ptrn->taction);
	initGuestSpec (&ptrn->guest);

	ptrn->u.tag.roleBits = 0;
	ptrn->refcount = 1;

	ptrn->optscript = NULL;
	ptrn->optscript_src = NULL;

	ptrn->foreign_lang = LANG_IGNORE;

	return ptrn;
}

static regexTableEntry * newRefPatternEntry (regexTableEntry * other)
{
	regexTableEntry *entry = xCalloc (1, regexTableEntry);

	Assert (other && other->pattern);

	entry->pattern = refPattern(other->pattern);
	return entry;
}

static regexTableEntry * newEntry (regexCompiledCode* const pattern,
								   enum regexParserType regptype)
{
	regexTableEntry *entry = xCalloc (1, regexTableEntry);
	entry->pattern = newPattern (pattern, regptype);
	return entry;
}

static regexPattern* addCompiledTagCommon (struct lregexControlBlock *lcb,
										   int table_index,
										   regexCompiledCode* const pattern,
										   enum regexParserType regptype)
{
	regexTableEntry *entry = newEntry (pattern, regptype);

	if (regptype == REG_PARSER_MULTI_TABLE)
	{
		struct regexTable *table = ptrArrayItem (lcb->tables, table_index);
		Assert(table);

		ptrArrayAdd (table->entries, entry);
	}
	else
		ptrArrayAdd (lcb->entries[regptype], entry);

	useRegexMethod(lcb->owner);

	return entry->pattern;
}

static void pre_ptrn_flag_mgroup_long (const char* const s, const char* const v, void* data)
{
	struct mGroupSpec *mgroup = data;
	if (!v)
	{
		error (WARNING, "no value is given for: %s", s);
		return;
	}
	if (!strToInt (v, 10, &mgroup->forLineNumberDetermination))
	{
		error (WARNING, "wrong %s specification: %s", s, v);
		mgroup->forLineNumberDetermination = NO_MULTILINE;
	}
	else if (mgroup->forLineNumberDetermination < 0
			 || mgroup->forLineNumberDetermination >= BACK_REFERENCE_COUNT)
	{
		error (WARNING, "out of range(0 ~ %d) %s specification: %s",
			   (BACK_REFERENCE_COUNT - 1),
			   s, v);
		mgroup->forLineNumberDetermination = NO_MULTILINE;
	}
}

static void pre_ptrn_flag_advanceTo_long (const char* const s, const char* const v, void* data)
{
	struct mGroupSpec *mgroup = data;
	char *vdup;
	char *tmp;


	if (!v)
	{
		error (WARNING, "no value is given for: %s", s);
		return;
	}

	vdup = eStrdup (v);

	mgroup->nextFromStart = false;
	if ((tmp = strstr(vdup, "start")))
	{
		mgroup->nextFromStart = true;
		*tmp = '\0';
	}
	else if ((tmp = strstr(vdup, "end")))
		*tmp = '\0';

	if (!strToInt (vdup, 10, &(mgroup->forNextScanning)))
	{
		error (WARNING, "wrong %s specification: %s", s, vdup);
		mgroup->nextFromStart = false;
	}
	else if (mgroup->forNextScanning < 0 || mgroup->forNextScanning >= BACK_REFERENCE_COUNT)
	{
		error (WARNING, "out of range(0 ~ %d) %s specification: %s",
			   (BACK_REFERENCE_COUNT - 1), s, vdup);
		mgroup->nextFromStart = false;
	}

	eFree (vdup);
}

struct guestPtrnFlagData {
	enum regexParserType type;
	struct guestSpec *guest;
};

static void pre_ptrn_flag_guest_long (const char* const s, const char* const v, void* data)
{
	struct guestPtrnFlagData *flagData = data;
	enum regexParserType type = flagData->type;
	struct guestSpec *guest = flagData->guest;
	struct boundarySpec *current;

	if (!v)
	{
		error (WARNING, "no value is given for: %s", s);
		return;
	}

	char *tmp = strchr (v, ',');
	if (tmp == NULL)
	{
		error (WARNING, "no terminator found for parser name: %s", s);
		return;
	}

	if ((tmp - v) == 0)
	{
		if (type == REG_PARSER_MULTI_LINE)
		{
			error (WARNING,
				   "using placeholder for guest name field is not allowed in multiline regex spec: %s", v);
			goto err;
		}

		guest->lang.type = GUEST_LANG_PLACEHOLDER;
	}
	else if (*v == '\\' || *v == '*')
	{
		const char *n_tmp = v + 1;
		const char *n = n_tmp;
		for (; isdigit ((unsigned char) *n_tmp); n_tmp++);
		char c = *n_tmp;
		*(char *)n_tmp = '\0';
		if (!strToInt (n, 10, &(guest->lang.spec.patternGroup)))
		{
			error (WARNING, "wrong guest name specification: %s", v);
			goto err;
		}
		else if (guest->lang.spec.patternGroup >= BACK_REFERENCE_COUNT)
		{
			error (WARNING, "wrong guest name specification (back reference count is too large): %d",
				   guest->lang.spec.patternGroup);
			goto err;
		}

		*(char *)n_tmp = c;
		if (*n_tmp != ',')
		{
			error (WARNING, "wrong guest specification (garbage at the end of end guest spec): %s", v);
			goto err;
		}

		guest->lang.type = (*v == '\\')
			? GUEST_LANG_PTN_GROUP_FOR_LANGNAME
			: GUEST_LANG_PTN_GROUP_FOR_FILEMAP;
	}
	else
	{
		guest->lang.spec.lang = getNamedLanguageOrAlias (v, (tmp - v));
		if (guest->lang.spec.lang == LANG_IGNORE)
		{
			error (WARNING, "no parser found for the guest spec: %s", v);
			goto err;
		}
		guest->lang.type = GUEST_LANG_STATIC_LANGNAME;
	}

	tmp++;
	if (*tmp == '\0')
	{
		error (WARNING, "no area spec found in the guest spec: %s", v);
		goto err;
	}

	for (int i = 0; i < 2; i++)
	{
		current = guest->boundary + i;
		const char *current_field_str = (i == GUEST_BOUNDARY_START? "start": "end");

		if (tmp [0] == ((i == GUEST_BOUNDARY_START)? ',': '\0'))
		{
			if (type == REG_PARSER_MULTI_LINE)
				error (WARNING,
					   "using placeholder for %s field is not allowed in multiline regex spec: %s",
					   current_field_str, v);

			current->placeholder = true;
		}
		else
		{
			char *n = tmp;

			for (; isdigit ((unsigned char) *tmp); tmp++);
			char c = *tmp;
			*tmp = '\0';
			if (!strToInt (n, 10, &(current->patternGroup)))
			{
				error (WARNING, "wrong guest area specification (patternGroup of %s, number expected): %s:%s",
					   current_field_str, v, n);
				goto err;
			}
			*tmp = c;
			if (*tmp == '\0')
			{
				error (WARNING, "wrong guest area specification (patternGroup of %s, nether start nor end given): %s",
					   current_field_str, v);
				goto err;
			}
			else if (strncmp (tmp, "start", 5) == 0)
			{
				current->fromStartOfGroup = true;
				tmp += 5;
			}
			else if (strncmp (tmp, "end", 3) == 0)
			{
				current->fromStartOfGroup = false;
				tmp += 3;
			}
			else
			{
				error (WARNING, "wrong guest area specification (%s): %s",
					   current_field_str, v);
				goto err;
			}
		}

		if (i == 0)
		{
			if (*tmp != ',')
			{
				error (WARNING,
					   "wrong guest area specification (separator between start and end boundaries): %s", v);
				goto err;
			}
			tmp++;
		}
		else if (i == 1 && (*tmp != '\0'))
		{
			error (WARNING, "wrong guest area specification (garbage at the end of end boundary spec): %s", v);
			goto err;
		}
	}
	return;
 err:
	guest->lang.type = GUEST_LANG_UNKNOWN;
}

static flagDefinition multilinePtrnFlagDef[] = {
	{ '\0',  "mgroup", NULL, pre_ptrn_flag_mgroup_long,
	  "N", "a group in pattern determining the line number of tag"},
	{ '\0',  "_advanceTo", NULL, pre_ptrn_flag_advanceTo_long,
	  "N[start|end]", "a group in pattern from where the next scan starts [0end]"},
};

static flagDefinition guestPtrnFlagDef[] = {
#define EXPERIMENTAL "_"
	{ '\0',  EXPERIMENTAL "guest", NULL, pre_ptrn_flag_guest_long,
	  "PARSERSPEC,N0[start|end],N1[start|end]", "run guest parser on the area"},
};

static bool hasMessage(const regexPattern *const ptrn)
{
	return (ptrn->message.selection > 0 && ptrn->message.message_string);
}

struct commonFlagData {
	const langType owner;
	const struct lregexControlBlock *const lcb;
	regexPattern *ptrn;
};

static void common_flag_msg_long (const char* const s, const char* const v, void* data)
{
	struct commonFlagData *cdata = data;
	regexPattern *ptrn = cdata->ptrn;

	Assert (ptrn);

	if (hasMessage(ptrn))
	{
		error (WARNING, "only one message flag may be given per regex (already set to '%s')",
			   ptrn->message.message_string);
		return;
	}

	if (strcmp (s, "fatal") == 0)
	{
		ptrn->message.selection = FATAL;
	}
	else if (strcmp (s, "warning") == 0)
	{
		ptrn->message.selection = WARNING;
	}

	Assert (ptrn->message.selection != 0);

	if (!v || !*v)
	{
		error (WARNING, "no message value is given for {%s}", s);
		return;
	}

	const char* begin = v;
	const char* end = v + strlen (v);
	--end;

	if (*begin != '"' || *end != '"' || begin == end)
	{
		error (WARNING, "argument for {%s} must be in double-quotes", s);
		return;
	}

	++begin;

	if (begin < end)
		ptrn->message.message_string = eStrndup (begin, end - begin);
}

static void common_flag_extra_long (const char* const s, const char* const v, void* data)
{
	struct commonFlagData * cdata = data;

	Assert (cdata->ptrn);

	if (!v)
	{
		error (WARNING, "no value is given for: %s", s);
		return;
	}

	langType lang = (cdata->ptrn->foreign_lang == LANG_IGNORE)
		? cdata->owner
		: cdata->ptrn->foreign_lang;
	cdata->ptrn->xtagType = getXtagTypeForNameAndLanguage (v, lang);
	if (cdata->ptrn->xtagType == XTAG_UNKNOWN)
		error (WARNING, "no such extra \"%s\" in %s", v, getLanguageName(lang));
}


static struct fieldPattern * fieldPatternNew (fieldType ftype, const char *template)
{
	struct fieldPattern *fp;

	fp = xMalloc(1, struct fieldPattern);
	fp->ftype = ftype;
	fp->template = eStrdup(template);

	return fp;
}

static void fieldPatternDelete (struct fieldPattern *fp)
{
	eFree ((void *)fp->template);
	eFree (fp);
}

static void common_flag_field_long (const char* const s, const char* const v, void* data)
{
	struct commonFlagData * cdata = data;
	regexPattern *ptrn = cdata->ptrn;

	Assert (ptrn);

	struct fieldPattern *fp;
	fieldType ftype;
	char *fname;
	const char* template;
	char *tmp;

	if (!v)
	{
		error (WARNING, "no value is given for: %s", s);
		return;
	}

	tmp = strchr (v, ':');
	if (tmp == NULL || tmp == v)
	{
		error (WARNING, "no field name is given for: %s", s);
		return;
	}

	fname = eStrndup (v, tmp - v);

	langType lang = (ptrn->foreign_lang == LANG_IGNORE)
		? cdata->owner
		: ptrn->foreign_lang;
	ftype = getFieldTypeForNameAndLanguage (fname, lang);
	if (ftype == FIELD_UNKNOWN)
	{
		error (WARNING, "no such field \"%s\" in %s", fname, getLanguageName(lang));
		eFree (fname);
		return;
	}

	if (ptrn->fieldPatterns)
	{
		for (unsigned int i = 0; i < ptrArrayCount(ptrn->fieldPatterns); i++)
		{
			fp = ptrArrayItem(ptrn->fieldPatterns, i);
			if (fp->ftype == ftype)
			{
				error (WARNING, "duplicated field specification \"%s\" in %s", fname, getLanguageName(cdata->owner));
				eFree (fname);
				return;
			}
		}
	}
	eFree (fname);

	template = tmp + 1;
	fp = fieldPatternNew (ftype, template);

	if (ptrn->fieldPatterns == NULL)
		ptrn->fieldPatterns = ptrArrayNew((ptrArrayDeleteFunc)fieldPatternDelete);
	ptrArrayAdd(ptrn->fieldPatterns, fp);
}

static void common_flag_role_long (const char* const s, const char* const v, void* data)
{
	struct commonFlagData * cdata = data;
	regexPattern *ptrn = cdata->ptrn;
	roleDefinition * role;

	Assert (ptrn);

	if (!v)
	{
		error (WARNING, "no value is given for: %s", s);
		return;
	}

	langType lang = (ptrn->foreign_lang == LANG_IGNORE)
		? cdata->owner
		: ptrn->foreign_lang;
	role = getLanguageRoleForName(lang,
								  ptrn->u.tag.kindIndex, v);
	if (!role)
	{
		error (WARNING, "no such role: \"%s\" in kind: \"%s\" of language: \"%s\"",
			   v,
			   getLanguageKind(lang, ptrn->u.tag.kindIndex)->name,
			   getLanguageName (lang));
		return;
	}

	ptrn->u.tag.roleBits |= makeRoleBit(role->id);
}

static void common_flag_anonymous_long (const char* const s, const char* const v, void* data)
{
	struct commonFlagData * cdata = data;
	regexPattern *ptrn = cdata->ptrn;

	Assert (ptrn);

	if (ptrn->anonymous_tag_prefix)
	{
		error (WARNING, "an anonymous tag prefix for this pattern (%s) is already given: %s",
			   ptrn->pattern_string? ptrn->pattern_string: "",
			   ptrn->anonymous_tag_prefix);
		return;
	}

	if (!v)
	{
		error (WARNING, "no PREFIX for anonymous regex flag is given (pattern == %s)",
			   ptrn->pattern_string? ptrn->pattern_string: "");
		return;
	}

	if (ptrn->u.tag.kindIndex == KIND_GHOST_INDEX)
	{
		error (WARNING, "use \"%s\" regex flag only with an explicitly defined kind", s);
		return;
	}

	ptrn->anonymous_tag_prefix = eStrdup (v);
}

static void precommon_flag_foreign_lang (const char* const s, const char* const v, void* data)
{
	struct commonFlagData * cdata = data;
	regexPattern *ptrn = cdata->ptrn;

	Assert (ptrn);

	if (ptrn->foreign_lang != LANG_IGNORE)
		error (FATAL, "foreign language for this pattern (%s) is already given: %s",
			   ptrn->pattern_string? ptrn->pattern_string: "",
			   getLanguageName(ptrn->foreign_lang));

	if (!v)
		error (FATAL, "no LANG for foreign flag is given (pattern == %s)",
			   ptrn->pattern_string? ptrn->pattern_string: "");

	langType lang = getNamedLanguage (v, 0);
	if (lang == LANG_IGNORE)
		error (FATAL, "language named '%s' specified is not found or not initialized yet",
			   v);

	if (!doesLanguageHaveForeignDependency (cdata->owner, lang))
		error (FATAL, "%s is not declared as a foreign language in %s parser",
			   v, getLanguageName (cdata->owner));

	ptrn->foreign_lang = lang;
}

static flagDefinition preCommonSpecFlagDef[] = {
	{ '\0',  EXPERIMENTAL "language", NULL, precommon_flag_foreign_lang,
	  "LANG", "make a foreign tag for LANG"},
};

static flagDefinition commonSpecFlagDef[] = {
	{ '\0',  "fatal", NULL, common_flag_msg_long ,
	  "\"MESSAGE\"", "print the given MESSAGE and exit"},
	{ '\0',  "warning", NULL, common_flag_msg_long ,
	  "\"MESSAGE\"", "print the given MESSAGE at WARNING level"},
#define EXPERIMENTAL "_"
	{ '\0',  EXPERIMENTAL "extra", NULL, common_flag_extra_long ,
	  "EXTRA", "record the tag only when the (foreign) extra is enabled"},
	{ '\0',  EXPERIMENTAL "field", NULL, common_flag_field_long ,
	  "FIELD:VALUE", "record the matched string(VALUE) to the (foreign) language specific FIELD of the tag"},
	{ '\0',  EXPERIMENTAL "role", NULL, common_flag_role_long,
	  "ROLE", "set the given ROLE to the roles field"},
	{ '\0',  EXPERIMENTAL "anonymous", NULL, common_flag_anonymous_long,
	  "PREFIX", "make an anonymous tag with PREFIX"},
};


static void pre_ptrn_flag_mtable_long (const char* const s, const char* const v, void* data)
{
	struct commonFlagData * cdata = data;
	regexPattern *ptrn = cdata->ptrn;
	struct mTableActionSpec *taction;
	bool taking_table = true;

	Assert (ptrn);
	Assert (cdata->lcb);

	taction = &ptrn->taction;

	if (strcmp (s, "tenter") == 0)
		taction->action = TACTION_ENTER;
	else if (strcmp (s, "tleave") == 0)
	{
		taction->action = TACTION_LEAVE;
		taking_table = false;
	}
	else if (strcmp (s, "tjump") == 0)
		taction->action = TACTION_JUMP;
	else if (strcmp (s, "treset") == 0)
		taction->action = TACTION_RESET;
	else if (strcmp (s, "tquit") == 0)
	{
		taction->action = TACTION_QUIT;
		taking_table = false;
	}

	if (taking_table)
	{
		int t;
		char *continuation = NULL;


		if (!v || (!*v))
			error (FATAL, "no table is given for table action: %s", s);

		if (taction->action == TACTION_ENTER
			&& (continuation = strchr (v, ',')))
		{
			char *tableEnterTo;

			tableEnterTo = eStrndup (v, continuation - v);
			t = getTableIndexForName (cdata->lcb, tableEnterTo);
			if (t < 0)
				error (FATAL, "table is not defined: %s", tableEnterTo);
			taction->table = ptrArrayItem (cdata->lcb->tables, t);
			eFree (tableEnterTo);

			if (!*(continuation + 1))
				error (FATAL, "no continuation table is given for: %s", v);

			int t_cont = getTableIndexForName (cdata->lcb, continuation + 1);
			if (t_cont < 0)
				error (FATAL, "table for continuation is not defined: %s", continuation + 1);
			taction->continuation_table = ptrArrayItem (cdata->lcb->tables, t_cont);
		}
		else
		{
			t = getTableIndexForName (cdata->lcb, v);
			if (t < 0)
				error (FATAL, "table is not defined: %s", v);
			taction->table = ptrArrayItem (cdata->lcb->tables, t);
			taction->continuation_table = NULL;
		}
	}
}

static flagDefinition multitablePtrnFlagDef[] = {
	{ '\0',  "tenter", NULL, pre_ptrn_flag_mtable_long ,
	  "TABLE[,CONT]", "enter to given regext table (with specifying continuation)"},
	{ '\0',  "tleave", NULL, pre_ptrn_flag_mtable_long ,
	  NULL, "leave from the current regext table"},
	{ '\0',  "tjump", NULL, pre_ptrn_flag_mtable_long ,
	  "TABLE", "jump to another regext table(don't push the current table to state stack)"},
	{ '\0',  "treset", NULL, pre_ptrn_flag_mtable_long ,
	  "TABLE", "clear the state stack and jump to given regex table"},
	{ '\0',  "tquit", NULL, pre_ptrn_flag_mtable_long ,
	  NULL, "stop the parsing with this parser"},
};


static void setKind(regexPattern * ptrn, const langType owner,
					const char kindLetter, const char* kindName,
					const char *const description,
					bool kind_explicitly_defined)
{
	Assert (ptrn);
	Assert (ptrn->u.tag.name_pattern);
	Assert (kindName);

	langType lang = (ptrn->foreign_lang == LANG_IGNORE? owner: ptrn->foreign_lang);
	kindDefinition *kdef = getLanguageKindForLetter (lang, kindLetter);

	if (kdef)
	{
		if (strcmp (kdef->name, kindName) && (strcmp(kindName, KIND_REGEX_DEFAULT_NAME)))
			/* When using a same kind letter for multiple regex patterns, the name of kind
			   should be the same. */
			error  (WARNING, "Don't reuse the kind letter `%c' in a language %s (old: \"%s\", new: \"%s\")",
					kdef->letter, getLanguageName (lang),
					kdef->name, kindName);
		ptrn->u.tag.kindIndex = kdef->id;
	}
	else if (*ptrn->u.tag.name_pattern == '\0' &&
			 kindLetter == KIND_REGEX_DEFAULT_LETTER &&
			 (strcmp(kindName, KIND_REGEX_DEFAULT_NAME) == 0) &&
			 (!kind_explicitly_defined))
		ptrn->u.tag.kindIndex = KIND_GHOST_INDEX;
	else
	{
		kdef = kindNew (kindLetter, kindName, description);
		defineLanguageKind (lang, kdef, kindFree);
		ptrn->u.tag.kindIndex = kdef->id;
	}
}

static void prePatternEvalFlags(struct lregexControlBlock *lcb,
							  regexPattern * ptrn,
							  enum regexParserType regptype,
							  const char* flags)
{
	struct commonFlagData commonFlagData = {
		.owner = lcb->owner,
		.lcb = lcb,
		.ptrn = ptrn
	};

	flagsEval (flags, preCommonSpecFlagDef, ARRAY_SIZE(preCommonSpecFlagDef), &commonFlagData);
}

static void patternEvalFlags (struct lregexControlBlock *lcb,
							  regexPattern * ptrn,
							  enum regexParserType regptype,
							  const char* flags)
{
	struct commonFlagData commonFlagData = {
		.owner = lcb->owner,
		.lcb = lcb,
		.ptrn = ptrn
	};

	if (regptype == REG_PARSER_SINGLE_LINE)
		flagsEval (flags, prePtrnFlagDef, ARRAY_SIZE(prePtrnFlagDef), ptrn);

	const char * optscript = flagsEval (flags, commonSpecFlagDef, ARRAY_SIZE(commonSpecFlagDef), &commonFlagData);
	if (optscript)
	{
		ptrn->optscript = scriptRead (optvm, optscript);
		ptrn->optscript_src = eStrdup (optscript);
	}

	if (regptype == REG_PARSER_SINGLE_LINE || regptype == REG_PARSER_MULTI_TABLE)
	{
		flagsEval (flags, scopePtrnFlagDef, ARRAY_SIZE(scopePtrnFlagDef), &ptrn->scopeActions);
		if ((ptrn->scopeActions & (SCOPE_REF|SCOPE_REF_AFTER_POP)) == (SCOPE_REF|SCOPE_REF_AFTER_POP))
			error (WARNING, "%s: don't combine \"replace\" with the other scope action.",
				   getLanguageName (lcb->owner));
	}
	else
	{
		flagsEval (flags, intervaltabPtrnFlagDef, ARRAY_SIZE(intervaltabPtrnFlagDef), &ptrn->scopeActions);
		Assert (ptrn->scopeActions == 0 || ptrn->scopeActions == SCOPE_INTERVALTAB);
	}

	if (regptype == REG_PARSER_MULTI_LINE || regptype == REG_PARSER_MULTI_TABLE)
	{
		ptrn->mgroup.forNextScanning = 0;
		/* ptrn->mgroup.nextFromStart is initialized in initMgroup() already. */
		flagsEval (flags, multilinePtrnFlagDef, ARRAY_SIZE(multilinePtrnFlagDef), &ptrn->mgroup);
	}

	struct guestPtrnFlagData guestPtrnFlagData = {
		.type = regptype,
		.guest = &ptrn->guest,
	};
	flagsEval (flags, guestPtrnFlagDef, ARRAY_SIZE(guestPtrnFlagDef), &guestPtrnFlagData);

	if (regptype == REG_PARSER_MULTI_TABLE)
		flagsEval (flags, multitablePtrnFlagDef, ARRAY_SIZE(multitablePtrnFlagDef), &commonFlagData);
}

static regexPattern *addCompiledTagPattern (struct lregexControlBlock *lcb,
											int table_index,
											enum regexParserType regptype, regexCompiledCode* const pattern,
					    const char* const name, char kindLetter, const char* kindName,
					    char *const description, const char* flags,
					    bool kind_explicitly_defined,
					    bool *disabled)
{
	regexPattern * ptrn = addCompiledTagCommon(lcb, table_index, pattern, regptype);

	ptrn->type = PTRN_TAG;
	ptrn->u.tag.name_pattern = eStrdup (name);
	ptrn->disabled = disabled;

	prePatternEvalFlags (lcb, ptrn, regptype, flags);
	setKind(ptrn, lcb->owner, kindLetter, kindName, description, kind_explicitly_defined);
	patternEvalFlags (lcb, ptrn, regptype, flags);

	return ptrn;
}

static regexPattern *addCompiledCallbackPattern (struct lregexControlBlock *lcb, regexCompiledCode* const pattern,
					const regexCallback callback, const char* flags,
					bool *disabled,
					void *userData)
{
	regexPattern * ptrn;
	ptrn = addCompiledTagCommon(lcb, TABLE_INDEX_UNUSED, pattern, REG_PARSER_SINGLE_LINE);
	flagsEval (flags, prePtrnFlagDef, ARRAY_SIZE(prePtrnFlagDef), ptrn);
	ptrn->type    = PTRN_CALLBACK;
	ptrn->u.callback.function = callback;
	ptrn->u.callback.userData = userData;
	ptrn->disabled = disabled;
	return ptrn;
}

#ifndef HAVE_PCRE2
static void no_pcre2_regex_flag_short (char c, void* data)
{
	error (WARNING, "'p' flag is specied but pcre2 regex engine is not linked.");
}
static void no_pcre2_regex_flag_long (const char* const s, const char* const unused CTAGS_ATTR_UNUSED, void* data)
{
	error (WARNING, "{pcre2} flag is specied but pcre2 regex engine is not linked.");
}
#endif

static flagDefinition backendFlagDefs[] = {
	{ 'b', "basic",  basic_regex_flag_short,  basic_regex_flag_long,
	  NULL, "interpreted as a Posix basic regular expression."},
	{ 'e', "extend", extend_regex_flag_short, extend_regex_flag_long,
	  NULL, "interpreted as a Posix extended regular expression (default)"},
#ifdef HAVE_PCRE2
	{ 'p', "pcre2",  pcre2_regex_flag_short, pcre2_regex_flag_long,
	  NULL, "use pcre2 regex engine"},
#else
	{ 'p', "pcre2",  no_pcre2_regex_flag_short, no_pcre2_regex_flag_long,
	  NULL, "pcre2 is NOT linked!"},
#endif
};

static void regex_flag_icase_short (char c CTAGS_ATTR_UNUSED, void* data)
{
	struct flagDefsDescriptor *desc = data;
	desc->backend->set_icase_flag (&desc->flags);
}

static void regex_flag_icase_long (const char* s CTAGS_ATTR_UNUSED, const char* const unused CTAGS_ATTR_UNUSED, void* data)
{
	regex_flag_icase_short ('i', data);
}

static flagDefinition backendCommonRegexFlagDefs[] = {
	{ 'i', "icase",  regex_flag_icase_short,  regex_flag_icase_long,
	  NULL, "applied in a case-insensitive manner"},
};


static struct flagDefsDescriptor choose_backend (const char *flags, enum regexParserType regptype, bool error_if_no_backend)
{
	struct flagDefsDescriptor desc = {
		.backend  = NULL,
		.flags = 0,
		.regptype = regptype,
	};

	if (flags)
		flagsEval (flags,
				   backendFlagDefs,
				   ARRAY_SIZE(backendFlagDefs),
				   &desc);

	/* Choose the default backend. */
	if (desc.backend == NULL)
	{
		if (flags && error_if_no_backend)
			error (FATAL, "No sunch backend for the name: \"%s\"", flags);

		flagsEval (DEFAULT_REGEX_BACKEND,
				   backendFlagDefs,
				   ARRAY_SIZE(backendFlagDefs),
				   &desc);
	}
	return desc;
}

static regexCompiledCode compileRegex (enum regexParserType regptype,
									   const char* const regexp, const char* const flags)
{
	struct flagDefsDescriptor desc = choose_backend (flags, regptype, false);

	/* Evaluate backend specific flags */
	flagsEval (flags,
			   desc.backend->fdefs,
			   desc.backend->fdef_count,
			   &desc.flags);

	flagsEval (flags,
			   backendCommonRegexFlagDefs,
			   ARRAY_SIZE (backendCommonRegexFlagDefs),
			   &desc);

	return desc.backend->compile (desc.backend, regexp, desc.flags);
}


/* If a letter and/or a name are defined in kindSpec, return true. */
static bool parseKinds (
		const char* const kindSpec, char* const kindLetter, char** const kindName,
		char **description)
{
	*description = NULL;

	if (kindSpec == NULL  ||  kindSpec [0] == '\0')
	{
		*kindLetter = KIND_REGEX_DEFAULT_LETTER;
		*kindName = eStrdup (KIND_REGEX_DEFAULT_NAME);
		return false;
	}
	else
	{
		bool explicitly_defined = false;
		const char* k = kindSpec;

		if (k [0] != ','  &&  (k [1] == ','  ||  k [1] == '\0'))
		{
			*kindLetter = *k++;
			explicitly_defined = true;
		}
		else
			*kindLetter = KIND_REGEX_DEFAULT_LETTER;

		if (*k == ',')
			++k;

		if (k [0] == '\0')
			*kindName = eStrdup (KIND_REGEX_DEFAULT_NAME);
		else
		{
			const char *const comma = strchr (k, ',');

			if (comma == NULL)
			{
				if (strlen (k) == 0)
					*kindName = eStrdup (KIND_REGEX_DEFAULT_NAME);
				else
				{
					*kindName = eStrdup (k);
					explicitly_defined = true;
				}
			}
			else
			{
				if (comma - k == 0)
					*kindName = eStrdup (KIND_REGEX_DEFAULT_NAME);
				else
				{
					*kindName = eStrndup (k, comma - k );
					explicitly_defined = true;
				}
				k = comma + 1;
				if (k [0] != '\0')
					*description = eStrdup (k);
			}
		}
		return explicitly_defined;
	}
}

/*
*   Regex pattern matching
*/


static vString* substitute (
		const char* const in, const char* out,
		const int nmatch, const regmatch_t* const pmatch)
{
	vString* result = vStringNew ();
	const char* p;
	for (p = out  ;  *p != '\0'  ;  p++)
	{
		if (*p == '\\'  &&  isdigit ((unsigned char) *++p))
		{
			const int dig = *p - '0';
			if (0 < dig  &&  dig < nmatch  &&  pmatch [dig].rm_so != -1)
			{
				const int diglen = pmatch [dig].rm_eo - pmatch [dig].rm_so;
				vStringNCatSUnsafe (result, in + pmatch [dig].rm_so, diglen);
			}
		}
		else if (*p != '\n'  &&  *p != '\r')
			vStringPut (result, *p);
	}
	return result;
}

static unsigned long getInputLineNumberInRegPType (enum regexParserType regptype,
												   off_t offset)
{
	return (regptype == REG_PARSER_MULTI_LINE || regptype == REG_PARSER_MULTI_TABLE)
		? getInputLineNumberForFileOffset (offset)
		: getInputLineNumber ();
}

static void fillEndLineFieldOfUpperScopes (struct lregexControlBlock *lcb, unsigned long endline)
{
	tagEntryInfo *entry;
	int n = lcb->currentScope;

	while ((entry = getEntryInCorkQueue (n))
		   && (entry->extensionFields._endLine == 0))
	{
		setTagEndLine (entry, endline);
		n = entry->extensionFields.scopeIndex;
	}
}

static bool hasNameSlot (const regexPattern* const patbuf)
{
	return (patbuf->u.tag.name_pattern[0] != '\0'
			|| patbuf->anonymous_tag_prefix);
}

static int scopeActionRef (int currentScope)
{
	int scope = currentScope;
	tagEntryInfo *entry;
	while ((entry = getEntryInCorkQueue (scope)) && entry->placeholder)
		/* Look at parent */
		scope = entry->extensionFields.scopeIndex;
	return scope;
}

static void matchTagPattern (struct lregexControlBlock *lcb,
		const char* line,
		const regexPattern* const patbuf,
		const regmatch_t* const pmatch,
			     off_t offset, scriptWindow *window)
{
	vString *const name =
		(patbuf->u.tag.name_pattern[0] != '\0') ? substitute (line,
															  patbuf->u.tag.name_pattern,
															  BACK_REFERENCE_COUNT, pmatch):
		(patbuf->anonymous_tag_prefix) ? anonGenerateNew (patbuf->anonymous_tag_prefix,
														  patbuf->u.tag.kindIndex):
		vStringNewInit ("");
	bool placeholder = !!((patbuf->scopeActions & SCOPE_PLACEHOLDER) == SCOPE_PLACEHOLDER);
	int scope = CORK_NIL;
	int n;

	vStringStripLeading (name);
	vStringStripTrailing (name);

	if (patbuf->scopeActions & SCOPE_REF)
		scope = scopeActionRef (lcb->currentScope);
	if (patbuf->scopeActions & SCOPE_CLEAR)
	{
		unsigned long endline = getInputLineNumberInRegPType(patbuf->regptype, offset);

		/*
		 * SCOPE_CLEAR|SCOPE_PUSH implies that "set" was specified as the scope action.
		 * If the specified action is "set", getInputLineNumberInRegPType()
		 * returns the start line of the NEW scope. The cleared scopes are ended BEFORE
		 * the new scope. There is a gap. We must adjust the "end:" field here.
		 */
		if (patbuf->scopeActions & SCOPE_PUSH && endline > 0)
			endline--;

		fillEndLineFieldOfUpperScopes (lcb, endline);
		lcb->currentScope = CORK_NIL;
	}
	if (patbuf->scopeActions & SCOPE_POP)
	{
		tagEntryInfo *entry = getEntryInCorkQueue (lcb->currentScope);

		if (entry && (entry->extensionFields._endLine == 0))
		{
			setTagEndLine (entry, getInputLineNumberInRegPType(patbuf->regptype, offset));

			/*
			 * SCOPE_POP|SCOPE_REF_AFTER_POP implies that "replace" was specified as the
			 * scope action. If the specified action is "replace", getInputLineNumberInRegPType()
			 * returns the start line of the NEW scope. The popped scope is ended BEFORE
			 * the new scope. There is a gap. We must adjust the "end:" field here.
			 */
			if ((patbuf->scopeActions & SCOPE_REF_AFTER_POP) &&
				entry->extensionFields._endLine > 1)
				setTagEndLine (entry, entry->extensionFields._endLine - 1);
		}

		lcb->currentScope = entry? entry->extensionFields.scopeIndex: CORK_NIL;
	}
	if (patbuf->scopeActions & SCOPE_REF_AFTER_POP)
		scope = scopeActionRef (lcb->currentScope);

	if (vStringLength (name) == 0 && (placeholder == false))
	{
		if (patbuf->accept_empty_name == false)
			error (WARNING, "%s:%lu: null expansion of name pattern \"%s\"",
			       getInputFileName (),
				   getInputLineNumberInRegPType(patbuf->regptype, offset),
			       patbuf->u.tag.name_pattern);
		n = CORK_NIL;
	}
	else
	{
		static TrashBox* field_trashbox;
		unsigned long ln = 0;
		MIOPos pos;
		tagEntryInfo e;
		int kind;
		roleBitsType roleBits;

		if ((patbuf->regptype == REG_PARSER_MULTI_LINE)
			|| (patbuf->regptype == REG_PARSER_MULTI_TABLE))
		{
			ln = getInputLineNumberForFileOffset (offset);
			pos = getInputFilePositionForLine (ln);
		}

		n = CORK_NIL;
		kind = patbuf->u.tag.kindIndex;
		roleBits = patbuf->u.tag.roleBits;

		if (scope == CORK_NIL && (patbuf->scopeActions & SCOPE_INTERVALTAB))
		{
			unsigned long ln0 = ln;
			if (ln0 == 0)
				ln0 = getInputLineNumber();
			if (ln0)
				scope = queryIntervalTabByLine(ln0);
		}

		initRegexTag (&e, vStringValue (name), kind, ROLE_DEFINITION_INDEX, scope, placeholder,
					  ln, ln == 0? NULL: &pos, patbuf->xtagType, patbuf->foreign_lang);

		if (field_trashbox == NULL)
		{
			field_trashbox = trashBoxNew();
			DEFAULT_TRASH_BOX (field_trashbox, trashBoxDelete);
		}

		if (patbuf->fieldPatterns)
		{
			for (unsigned int i = 0; i < ptrArrayCount(patbuf->fieldPatterns); i++)
			{
				struct fieldPattern *fp = ptrArrayItem(patbuf->fieldPatterns, i);
				if (isFieldEnabled (fp->ftype))
				{
					vString * const value = substitute (line, fp->template,
														BACK_REFERENCE_COUNT, pmatch);
					attachParserField (&e, fp->ftype, vStringValue (value));
					trashBoxPut (field_trashbox, value,
								 (TrashBoxDestroyItemProc)vStringDelete);
				}
			}
		}

		if (roleBits)
		{
			unsigned int roleIndex;

			for (roleIndex = 0;
				 roleIndex < countLanguageRoles(e.langType, kind);
				 roleIndex++)
			{
				if (roleBits & makeRoleBit(roleIndex))
					assignRole (&e, roleIndex);
			}
		}

		if (patbuf->anonymous_tag_prefix)
			markTagExtraBit (&e, XTAG_ANONYMOUS);

		n = makeTagEntry (&e);

		trashBoxMakeEmpty(field_trashbox);
	}

	if (patbuf->scopeActions & SCOPE_PUSH)
		lcb->currentScope = n;

	if (n != CORK_NIL && window)
	{
		scriptSetup (optvm, lcb, n, window);
		EsObject *e = scriptEval (optvm, patbuf->optscript);
		if (es_error_p (e))
			error (WARNING, "error when evaluating: %s %% input: %s, line:%lu", patbuf->optscript_src,
				   getInputFileName (),
				   getInputLineNumberInRegPType(patbuf->regptype, offset));
		es_object_unref (e);
		scriptTeardown (optvm, lcb);
	}

	vStringDelete (name);
}

static bool matchCallbackPattern (
		const vString* const line, const regexPattern* const patbuf,
		const regmatch_t* const pmatch)
{
	regexMatch matches [BACK_REFERENCE_COUNT];
	unsigned int count = 0;
	int i;
	for (i = 0  ;  i < BACK_REFERENCE_COUNT  ;  ++i)
	{
		matches [i].start  = pmatch [i].rm_so;
		matches [i].length = pmatch [i].rm_eo - pmatch [i].rm_so;
		/* a valid match may have both offsets == -1,
		 * e.g. (foo)*(bar) matching "bar" - see CTags bug 271.
		 * As POSIX regex doesn't seem to have a way to count matches,
		 * we return the count up to the last non-empty match. */
		if (pmatch [i].rm_so != -1)
			count = i + 1;
	}
	return patbuf->u.callback.function (vStringValue (line), matches, count,
				     patbuf->u.callback.userData);
}


static void printMessage(const langType language,
						 const regexPattern *const ptrn,
						 const off_t offset,
						 const char *const line,
						 const regmatch_t* const pmatch)
{
	vString *msg;

	Assert (ptrn);
	Assert (ptrn->message.selection > 0);
	Assert (ptrn->message.message_string);

	msg = substitute (line, ptrn->message.message_string, BACK_REFERENCE_COUNT, pmatch);

	error (ptrn->message.selection, "%sMessage from regex<%s>: %s (%s:%lu)",
		   (ptrn->message.selection == FATAL ? "Fatal: " : ""),
		   getLanguageName (language),
		   vStringValue (msg),
		   getInputFileName (),
		   getInputLineNumberInRegPType (ptrn->regptype, offset));

	vStringDelete (msg);
}

static bool isGuestRequestConsistent (struct guestRequest *guest_req)
{
	return (guest_req->lang != LANG_IGNORE)
		&& (guest_req->boundary[GUEST_BOUNDARY_START].offset
			< guest_req->boundary[GUEST_BOUNDARY_END].offset);
}

static bool fillGuestRequest (const char *start,
							  const char *current,
							  regmatch_t pmatch [BACK_REFERENCE_COUNT],
							  struct guestSpec *guest_spec,
							  struct guestRequest *guest_req)
{
	if (guest_spec->lang.type == GUEST_LANG_UNKNOWN)
		return false;
	else if (guest_spec->lang.type == GUEST_LANG_PLACEHOLDER)
		;
	else if (guest_spec->lang.type == GUEST_LANG_STATIC_LANGNAME)
	{
		guest_req->lang = guest_spec->lang.spec.lang;
		guest_req->lang_set = true;
	}
	else if (guest_spec->lang.type == GUEST_LANG_PTN_GROUP_FOR_LANGNAME)
	{
		const char * name = current + pmatch [guest_spec->lang.spec.patternGroup].rm_so;
		int size = pmatch [guest_spec->lang.spec.patternGroup].rm_eo
			- pmatch [guest_spec->lang.spec.patternGroup].rm_so;
		if (size > 0)
		{
			guest_req->lang = getNamedLanguageOrAlias (name, size);
			guest_req->lang_set = true;
		}
	}
	else if (guest_spec->lang.type == GUEST_LANG_PTN_GROUP_FOR_FILEMAP)
	{
		const char * name = current + pmatch [guest_spec->lang.spec.patternGroup].rm_so;
		int size = pmatch [guest_spec->lang.spec.patternGroup].rm_eo
			- pmatch [guest_spec->lang.spec.patternGroup].rm_so;
		char *fname = (size > 0)? eStrndup (name, size): NULL;

		if (fname)
		{
			guest_req->lang = getLanguageForFilename (fname, LANG_AUTO);
			guest_req->lang_set = true;
			eFree (fname);
		}
	}

	for (int i = 0; i < 2; i++)
	{
		struct boundarySpec *boundary_spec = guest_spec->boundary + i;
		struct boundaryInRequest *boundary = guest_req->boundary + i;
		if (!boundary_spec->placeholder)
		{
			boundary->offset =  current - start + (boundary_spec->fromStartOfGroup
												   ? pmatch [boundary_spec->patternGroup].rm_so
												   : pmatch [boundary_spec->patternGroup].rm_eo);
			boundary->offset_set = true;
		}
	}
	return guestRequestIsFilled (guest_req);
}

static bool matchRegexPattern (struct lregexControlBlock *lcb,
							   const vString* const line,
							   regexTableEntry *entry)
{
	bool result = false;
	regmatch_t pmatch [BACK_REFERENCE_COUNT];
	int match;
	regexPattern* patbuf = entry->pattern;
	struct guestSpec  *guest = &patbuf->guest;

	if (patbuf->disabled && *(patbuf->disabled))
		return false;

	match = patbuf->pattern.backend->match (patbuf->pattern.backend,
											patbuf->pattern.code, vStringValue (line),
											vStringLength (line),
											pmatch);

	if (match == 0)
	{
		result = true;
		entry->statistics.match++;
		scriptWindow window = {
			.line = vStringValue (line),
			.start = 0,
			.patbuf = patbuf,
			.pmatch = pmatch,
			.nmatch = BACK_REFERENCE_COUNT,
			.advanceto = false,
		};

		if (patbuf->optscript && (! hasNameSlot (patbuf)))
		{
			scriptSetup (optvm, lcb, CORK_NIL, &window);
			EsObject *e = scriptEval (optvm, patbuf->optscript);
			if (es_error_p (e))
				error (WARNING, "error when evaluating: %s %% input: %s", patbuf->optscript_src,
					   getInputFileName ());
			es_object_unref (e);
			scriptTeardown (optvm, lcb);
		}

		if (hasMessage(patbuf))
			printMessage(lcb->owner, patbuf, 0, vStringValue (line), pmatch);

		if (patbuf->type == PTRN_TAG)
		{
			matchTagPattern (lcb, vStringValue (line), patbuf, pmatch, 0,
							 (patbuf->optscript && hasNameSlot (patbuf))? &window: NULL);

			if (guest->lang.type != GUEST_LANG_UNKNOWN)
			{
				unsigned long ln = getInputLineNumber ();
				long current = getInputFileOffsetForLine (ln);
				if (fillGuestRequest (vStringValue (line) - current,
									  vStringValue (line), pmatch, guest, lcb->guest_req))
				{
					Assert (lcb->guest_req->lang != LANG_AUTO);
					if (isGuestRequestConsistent(lcb->guest_req))
						guestRequestSubmit (lcb->guest_req);
					guestRequestClear (lcb->guest_req);
				}
			}
		}
		else if (patbuf->type == PTRN_CALLBACK)
			result = matchCallbackPattern (line, patbuf, pmatch);
		else
		{
			Assert ("invalid pattern type" == NULL);
			result = false;
		}
	}
	else
		entry->statistics.unmatch++;
	return result;
}

static bool matchMultilineRegexPattern (struct lregexControlBlock *lcb,
										const vString* const allLines,
										regexTableEntry *entry)
{
	const char *start;
	const char *current;
	regexPattern* patbuf = entry->pattern;
	struct mGroupSpec *mgroup = &patbuf->mgroup;
	struct guestSpec  *guest = &patbuf->guest;

	bool result = false;
	regmatch_t pmatch [BACK_REFERENCE_COUNT];
	int match = 0;
	unsigned int delta = 1;

	Assert (patbuf);

	if (patbuf->disabled && *(patbuf->disabled))
		return false;

	current = start = vStringValue (allLines);
	do
	{
		match = patbuf->pattern.backend->match (patbuf->pattern.backend,
												patbuf->pattern.code, current,
												vStringLength (allLines) - (current - start),
												pmatch);

		if (match != 0)
		{
			entry->statistics.unmatch++;
			break;
		}

		if (hasMessage(patbuf))
			printMessage(lcb->owner, patbuf, (current + pmatch[0].rm_so) - start, current, pmatch);

		entry->statistics.match++;
		scriptWindow window = {
			.line = current,
			.start = start,
			.patbuf = patbuf,
			.pmatch = pmatch,
			.nmatch = BACK_REFERENCE_COUNT,
			.advanceto = false,
		};

		if (patbuf->optscript && (! hasNameSlot (patbuf)))
		{
			scriptSetup (optvm, lcb, CORK_NIL, &window);
			EsObject *e = scriptEval (optvm, patbuf->optscript);
			if (es_error_p (e))
				error (WARNING, "error when evaluating: %s %% input: %s", patbuf->optscript_src,
					   getInputFileName ());
			es_object_unref (e);
			scriptTeardown (optvm, lcb);
		}

		if (patbuf->type == PTRN_TAG)
		{
			Assert (mgroup->forLineNumberDetermination != NO_MULTILINE);
			off_t offset = (current + pmatch [mgroup->forLineNumberDetermination].rm_so)
				- start;
			matchTagPattern (lcb, current, patbuf, pmatch, offset,
							 (patbuf->optscript && hasNameSlot (patbuf))? &window: NULL);
			result = true;
		}
		else if (patbuf->type == PTRN_CALLBACK)
			;	/* Not implemented yet */
		else
		{
			Assert ("invalid pattern type" == NULL);
			result = false;
			break;
		}

		if (fillGuestRequest (start, current, pmatch, guest, lcb->guest_req))
		{
			Assert (lcb->guest_req->lang != LANG_AUTO);
			if (isGuestRequestConsistent(lcb->guest_req))
				guestRequestSubmit (lcb->guest_req);
			guestRequestClear (lcb->guest_req);
		}

		delta = (mgroup->nextFromStart
				 ? pmatch [mgroup->forNextScanning].rm_so
				 : pmatch [mgroup->forNextScanning].rm_eo);
		if (delta == 0)
		{
			unsigned int pos = current - start;
			error (WARNING,
				   "a multi line regex pattern doesn't advance the input cursor: %s",
				   patbuf->pattern_string);
			error (WARNING, "Language: %s, input file: %s, pos: %u",
				   getLanguageName (lcb->owner), getInputFileName(), pos);
			break;
		}
		current += delta;

	} while (current < start + vStringLength (allLines));

	return result;
}

/* PUBLIC INTERFACE */

/* Match against all patterns for specified language. Returns true if at least
 * on pattern matched.
 */
extern bool regexIsPostRun (struct lregexControlBlock *lcb)
{
	for (unsigned int i = 0  ;  i < ptrArrayCount(lcb->entries[REG_PARSER_SINGLE_LINE])  ;  ++i)
	{
		regexTableEntry *entry = ptrArrayItem(lcb->entries[REG_PARSER_SINGLE_LINE], i);
		regexPattern *ptrn = entry->pattern;

		if (ptrn->postrun)
			return true;
	}

	return false;
}

extern bool matchRegex (struct lregexControlBlock *lcb, const vString* const line, bool postrun)
{
	bool result = false;
	unsigned int i;
	for (i = 0  ;  i < ptrArrayCount(lcb->entries[REG_PARSER_SINGLE_LINE])  ;  ++i)
	{
		regexTableEntry *entry = ptrArrayItem(lcb->entries[REG_PARSER_SINGLE_LINE], i);
		regexPattern *ptrn = entry->pattern;

		Assert (ptrn);

		if (postrun != ptrn->postrun)
			continue;

		if ((ptrn->xtagType != XTAG_UNKNOWN)
			&& (!isXtagEnabled (ptrn->xtagType)))
				continue;

		if (matchRegexPattern (lcb, line, entry))
		{
			result = true;
			if (ptrn->exclusive)
				break;
		}
	}
	return result;
}

extern void notifyRegexInputStart (struct lregexControlBlock *lcb)
{
	lcb->currentScope = CORK_NIL;

	ptrArrayClear (lcb->tstack);
	guestRequestClear (lcb->guest_req);

	opt_vm_dstack_push (optvm, lregex_dict);

	if (es_null (lcb->local_dict))
		lcb->local_dict = opt_dict_new (23);
	opt_vm_dstack_push (optvm, lcb->local_dict);
	set_current_lcb (optvm, lcb);
	scriptEvalHook (optvm, lcb, SCRIPT_HOOK_PRELUDE);
}

extern void notifyRegexInputEnd (struct lregexControlBlock *lcb)
{
	scriptEvalHook (optvm, lcb, SCRIPT_HOOK_SEQUEL);
	set_current_lcb (optvm, NULL);
	opt_vm_clear (optvm, false);
	opt_dict_clear (lcb->local_dict);
	unsigned long endline = getInputLineNumber ();
	fillEndLineFieldOfUpperScopes (lcb, endline);
}

extern void findRegexTagsMainloop (int (* driver)(void))
{
	/* merely read all lines of the file */
	while (driver () != EOF)
		;
}

static int fileReadLineDriver(void)
{
	return (readLineFromInputFile () == NULL)? EOF: 1;
}

extern void findRegexTags (void)
{
	findRegexTagsMainloop (fileReadLineDriver);
}

static bool doesExpectCorkInRegex0(ptrArray *entries)
{
	for (unsigned int i = 0; i < ptrArrayCount(entries); i++)
	{
		regexTableEntry *entry = ptrArrayItem(entries, i);
		Assert (entry && entry->pattern);
		if (entry->pattern->scopeActions
			|| entry->pattern->optscript
			)
			return true;
	}
	return false;
}

extern bool doesExpectCorkInRegex (struct lregexControlBlock *lcb)
{
	ptrArray *entries;

	entries = lcb->entries[REG_PARSER_SINGLE_LINE];
	if (doesExpectCorkInRegex0 (entries))
		return true;

	entries = lcb->entries[REG_PARSER_MULTI_LINE];
	if (doesExpectCorkInRegex0 (entries))
		return true;

	for (unsigned int i = 0; i < ptrArrayCount(lcb->tables); i++)
	{
		struct regexTable *table = ptrArrayItem(lcb->tables, i);
		if (doesExpectCorkInRegex0 (table->entries))
			return true;
	}

	return false;
}

static char *escapeRegexPattern (const char* pattern)
{
	vString *p = vStringNew ();

	while (*pattern != '\0')
	{
		char c = *pattern;
		if (c == '\n')
			vStringCatS(p, "\\n");
		else if (c == '\t')
			vStringCatS(p, "\\t");
		else if (c == '\\')
			vStringCatS(p, "\\\\");
		else
			vStringPut(p, c);

		pattern++;
	}

	return vStringDeleteUnwrap (p);
}

static regexPattern *addTagRegexInternal (struct lregexControlBlock *lcb,
										  int table_index,
					  enum regexParserType regptype,
					  const char* const regex,
					  const char* const name,
					  const char* const kinds,
					  const char* const flags,
					  bool *disabled)
{
	Assert (regex != NULL);
	Assert (name != NULL);

	if (!regexAvailable)
		return NULL;

	regexCompiledCode cp = compileRegex (regptype, regex, flags);
	if (cp.code == NULL)
	{
		error (WARNING, "pattern: %s", regex);
		if (table_index != TABLE_INDEX_UNUSED)
		{
			struct regexTable *table = ptrArrayItem (lcb->tables, table_index);
			error (WARNING, "table: %s[%u]", table->name, ptrArrayCount (table->entries));
			error (WARNING, "language: %s", getLanguageName (lcb->owner));
		}
		else
			error (WARNING, "language: %s[%u]", getLanguageName (lcb->owner),
				   ptrArrayCount (lcb->entries[regptype]));
		return NULL;
	}

	char kindLetter;
	char* kindName;
	char* description;
	kindDefinition* fileKind;

	bool explictly_defined =  parseKinds (kinds, &kindLetter, &kindName, &description);
	fileKind = getLanguageKind (lcb->owner, KIND_FILE_INDEX);
	if (kindLetter == fileKind->letter)
		error (FATAL,
			   "Kind letter \'%c\' used in regex definition \"%s\" of %s language is reserved in ctags main",
			   kindLetter,
			   regex,
			   getLanguageName (lcb->owner));
	else if (!isalpha ((unsigned char)kindLetter))
		error (FATAL,
			   "Kind letter must be an alphabetical character: \"%c\"",
			   kindLetter);

	if (strcmp (kindName, fileKind->name) == 0)
		error (FATAL,
			   "Kind name \"%s\" used in regex definition \"%s\" of %s language is reserved in ctags main",
			   kindName,
			   regex,
			   getLanguageName (lcb->owner));

	const char *option_base = (regptype == REG_PARSER_SINGLE_LINE? "regex"        :
							   regptype == REG_PARSER_MULTI_LINE ? "mline-regex"  :
							   regptype == REG_PARSER_MULTI_TABLE? "_mtable-regex":
							   NULL);
	Assert (option_base);

	for (const char * p = kindName; *p; p++)
	{
		if (p == kindName)
		{
			if (!isalpha((unsigned char) *p))
				error (FATAL,
					   "A kind name doesn't start with an alphabetical character: "
					   "'%s' in \"--%s-%s\" option",
					   kindName,
					   option_base,
					   getLanguageName (lcb->owner));
		}
		else
		{
			/*
			 * People may object to this error.
			 * Searching github repositories, I found not a few .ctags files
			 * in which Exuberant-ctags users define kind names with whitespaces.
			 * "FATAL" error breaks the compatibility.
			 */
			if (!isalnum((unsigned char) *p))
				error (/* regptype == REG_PARSER_SINGLE_LINE? WARNING: */ FATAL,
					   "Non-alphanumeric char is used in kind name: "
					   "'%s' in \"--%s-%s\" option",
					   kindName,
					   option_base,
					   getLanguageName (lcb->owner));

		}
	}

	regexPattern *rptr = addCompiledTagPattern (lcb, table_index,
												regptype, &cp, name,
												kindLetter, kindName, description, flags,
												explictly_defined,
												disabled);
	rptr->pattern_string = escapeRegexPattern(regex);

	eFree (kindName);
	if (description)
		eFree (description);

	if (*name == '\0')
	{
		if (rptr->exclusive || rptr->scopeActions & SCOPE_PLACEHOLDER
			|| rptr->anonymous_tag_prefix
			|| regptype == REG_PARSER_MULTI_TABLE
			|| rptr->guest.lang.type != GUEST_LANG_UNKNOWN
			|| rptr->optscript
			)
			rptr->accept_empty_name = true;
		else
			error (WARNING, "%s: regexp missing name pattern", regex);
	}

	return rptr;
}

extern void addTagRegex (struct lregexControlBlock *lcb,
			 const char* const regex,
			 const char* const name,
			 const char* const kinds,
			 const char* const flags,
			 bool *disabled)
{
	addTagRegexInternal (lcb, TABLE_INDEX_UNUSED,
						 REG_PARSER_SINGLE_LINE, regex, name, kinds, flags, disabled);
}

extern void addTagMultiLineRegex (struct lregexControlBlock *lcb, const char* const regex,
								  const char* const name, const char* const kinds, const char* const flags,
								  bool *disabled)
{
	regexPattern *ptrn = addTagRegexInternal (lcb, TABLE_INDEX_UNUSED,
											  REG_PARSER_MULTI_LINE, regex, name, kinds, flags, disabled);
	if (ptrn->mgroup.forLineNumberDetermination == NO_MULTILINE)
	{
		if (hasNameSlot(ptrn))
			error (WARNING, "%s: no {mgroup=N} flag given in --mline-regex-<LANG>=/%s/... (%s)",
				   regex,
				   getLanguageName (lcb->owner), ASSERT_FUNCTION);
		ptrn->mgroup.forLineNumberDetermination = 0;
	}
}

extern void addTagMultiTableRegex(struct lregexControlBlock *lcb,
								  const char* const table_name,
								  const char* const regex,
								  const char* const name, const char* const kinds, const char* const flags,
								  bool *disabled)
{
	int table_index = getTableIndexForName (lcb, table_name);

	if (table_index < 0)
		error (FATAL, "unknown table name: %s", table_name);

	addTagRegexInternal (lcb, table_index, REG_PARSER_MULTI_TABLE, regex, name, kinds, flags,
						 disabled);
}

extern void addCallbackRegex (struct lregexControlBlock *lcb,
			      const char* const regex,
			      const char* const flags,
			      const regexCallback callback,
			      bool *disabled,
			      void * userData)
{
	Assert (regex != NULL);

	if (!regexAvailable)
		return;


	regexCompiledCode cp = compileRegex (REG_PARSER_SINGLE_LINE, regex, flags);
	if (cp.code == NULL)
	{
		error (WARNING, "pattern: %s", regex);
		error (WARNING, "language: %s", getLanguageName (lcb->owner));
		return;
	}

	regexPattern *rptr = addCompiledCallbackPattern (lcb, &cp, callback, flags,
													 disabled, userData);
	rptr->pattern_string = escapeRegexPattern(regex);
}

static void addTagRegexOption (struct lregexControlBlock *lcb,
							   enum regexParserType regptype,
							   const char* const pattern)
{
	if (!regexAvailable)
		return;

	int table_index = TABLE_INDEX_UNUSED;
	char * regex_pat = NULL;
	char *name, *kinds, *flags;


	if (regptype == REG_PARSER_MULTI_TABLE)
	{
		const char *c;
		for (c = pattern; *c; c++)
		{
			if (! (isalnum((unsigned char) *c) || *c == '_'))
			{
				if (*c &&  (*(c + 1) != '^'))
				{
					vString *tmp = vStringNew ();

					/* Put '^' as prefix for the pattern */
					vStringPut(tmp, *c);
					vStringPut(tmp, '^');
					vStringCatS(tmp, c + 1);
					regex_pat = vStringDeleteUnwrap(tmp);
				}
				else
					regex_pat = eStrdup (c);
				break;
			}
		}

		if (regex_pat == NULL || *regex_pat == '\0')
			error (FATAL, "wrong mtable pattern specification: %s", pattern);

		char *table_name = eStrndup(pattern, c - pattern);
		table_index = getTableIndexForName (lcb, table_name);
		if (table_index < 0)
			error (FATAL, "unknown table name: %s (in %s)", table_name, pattern);
		eFree(table_name);
	}
	else
		regex_pat = eStrdup (pattern);

	if (parseTagRegex (regptype, regex_pat, &name, &kinds, &flags))
	{
		regexPattern *ptrn = addTagRegexInternal (lcb, table_index, regptype, regex_pat, name, kinds, flags,
												  NULL);
		if (regptype == REG_PARSER_MULTI_LINE
			&& ptrn->mgroup.forLineNumberDetermination == NO_MULTILINE)
		{
			if (hasNameSlot(ptrn))
				error (WARNING, "%s: no {mgroup=N} flag given in --mline-regex-<LANG>=%s... (%s)",
					   getLanguageName (lcb->owner),
					   pattern, ASSERT_FUNCTION);
			ptrn->mgroup.forLineNumberDetermination = 0;
		}
	}

	eFree (regex_pat);
}

extern void processTagRegexOption (struct lregexControlBlock *lcb,
								   enum regexParserType regptype,
								   const char* const parameter)
{
	if (parameter == NULL  ||  parameter [0] == '\0')
		clearPatternSet (lcb);
	else if (parameter [0] != '@')
		addTagRegexOption (lcb, regptype, parameter);
	else if (! doesFileExist (parameter + 1))
		error (WARNING, "cannot open regex file");
	else
	{
		const char* regexfile = parameter + 1;

		verbose ("open a regex file: %s\n", regexfile);
		MIO* const mio = mio_new_file (regexfile, "r");
		if (mio == NULL)
			error (WARNING | PERROR, "%s", regexfile);
		else
		{
			vString* const regex = vStringNew ();
			while (readLineRaw (regex, mio))
			{
				if (vStringLength (regex) > 1 && vStringValue (regex)[0] != '\n')
					addTagRegexOption (lcb, regptype, vStringValue (regex));
			}
			mio_unref (mio);
			vStringDelete (regex);
		}
	}
}

/*
*   Regex option parsing
*/

extern void printRegexFlags (bool withListHeader, bool machinable, const char *flags, FILE *fp)
{
	struct colprintTable * table = flagsColprintTableNew ();

	if (flags && *flags != '\0')
	{
		/* Print backend specific flags.
		 * This code is just stub because there is no backend having a specific flag.
		 * The help message for this option is not updated. */
		struct flagDefsDescriptor desc = choose_backend (flags, REG_PARSER_SINGLE_LINE, true);
		flagsColprintAddDefinitions (table, desc.backend->fdefs, desc.backend->fdef_count);
	}
	else
	{
		flagsColprintAddDefinitions (table, backendFlagDefs, ARRAY_SIZE(backendFlagDefs));
		flagsColprintAddDefinitions (table, backendCommonRegexFlagDefs, ARRAY_SIZE(backendCommonRegexFlagDefs));
		flagsColprintAddDefinitions (table, prePtrnFlagDef, ARRAY_SIZE (prePtrnFlagDef));
		flagsColprintAddDefinitions (table, guestPtrnFlagDef, ARRAY_SIZE (guestPtrnFlagDef));
		flagsColprintAddDefinitions (table, scopePtrnFlagDef, ARRAY_SIZE (scopePtrnFlagDef));
		flagsColprintAddDefinitions (table, preCommonSpecFlagDef, ARRAY_SIZE (preCommonSpecFlagDef));
		flagsColprintAddDefinitions (table, commonSpecFlagDef, ARRAY_SIZE (commonSpecFlagDef));
	}

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void printMultilineRegexFlags (bool withListHeader, bool machinable, const char *flags, FILE *fp)
{
	struct colprintTable * table = flagsColprintTableNew ();

	if (flags && *flags != '\0')
	{
		/* Print backend specific flags.
		 * This code is just stub because there is no backend having a specific flag.
		 * The help message for this option is not updated. */
		struct flagDefsDescriptor desc = choose_backend (flags, REG_PARSER_MULTI_LINE, true);
		flagsColprintAddDefinitions (table, desc.backend->fdefs, desc.backend->fdef_count);
	}
	else
	{
		flagsColprintAddDefinitions (table, backendFlagDefs, ARRAY_SIZE(backendFlagDefs));
		flagsColprintAddDefinitions (table, backendCommonRegexFlagDefs, ARRAY_SIZE(backendCommonRegexFlagDefs));
		flagsColprintAddDefinitions (table, multilinePtrnFlagDef, ARRAY_SIZE (multilinePtrnFlagDef));
		flagsColprintAddDefinitions (table, guestPtrnFlagDef, ARRAY_SIZE (guestPtrnFlagDef));
		flagsColprintAddDefinitions (table, intervaltabPtrnFlagDef, ARRAY_SIZE (intervaltabPtrnFlagDef));
		flagsColprintAddDefinitions (table, preCommonSpecFlagDef, ARRAY_SIZE (preCommonSpecFlagDef));
		flagsColprintAddDefinitions (table, commonSpecFlagDef, ARRAY_SIZE (commonSpecFlagDef));
	}

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void printMultitableRegexFlags (bool withListHeader, bool machinable, const char *flags, FILE *fp)
{
	struct colprintTable * table = flagsColprintTableNew ();

	if (flags && *flags != '\0')
	{
		/* Print backend specific flags.
		 * This code is just stub because there is no backend having a specific flag.
		 * The help message for this option is not updated. */
		struct flagDefsDescriptor desc = choose_backend (flags, REG_PARSER_MULTI_TABLE, true);
		flagsColprintAddDefinitions (table, desc.backend->fdefs, desc.backend->fdef_count);
	}
	else
	{
		flagsColprintAddDefinitions (table, backendFlagDefs, ARRAY_SIZE(backendFlagDefs));
		flagsColprintAddDefinitions (table, backendCommonRegexFlagDefs, ARRAY_SIZE(backendCommonRegexFlagDefs));
		flagsColprintAddDefinitions (table, multilinePtrnFlagDef, ARRAY_SIZE (multilinePtrnFlagDef));
		flagsColprintAddDefinitions (table, multitablePtrnFlagDef, ARRAY_SIZE (multitablePtrnFlagDef));
		flagsColprintAddDefinitions (table, guestPtrnFlagDef, ARRAY_SIZE (guestPtrnFlagDef));
		flagsColprintAddDefinitions (table, scopePtrnFlagDef, ARRAY_SIZE (scopePtrnFlagDef));
		flagsColprintAddDefinitions (table, preCommonSpecFlagDef, ARRAY_SIZE (preCommonSpecFlagDef));
		flagsColprintAddDefinitions (table, commonSpecFlagDef, ARRAY_SIZE (commonSpecFlagDef));
	}

	flagsColprintTablePrint (table, withListHeader, machinable, fp);
	colprintTableDelete(table);
}

extern void freeRegexResources (void)
{
	es_object_unref (lregex_dict);
	void *d = opt_vm_set_app_data(optvm, NULL);
	if (d)
		eFree (d);
	opt_vm_delete (optvm);
}

extern bool lregexControlBlockHasAny(struct lregexControlBlock *lcb)
{
	return (ptrArrayCount(lcb->entries [REG_PARSER_MULTI_LINE])
			|| ptrArrayCount(lcb->tables)
			|| ptrArrayCount(lcb->entries[REG_PARSER_SINGLE_LINE]));
}

extern bool regexNeedsMultilineBuffer (struct lregexControlBlock *lcb)
{
	if  (ptrArrayCount(lcb->entries [REG_PARSER_MULTI_LINE]) > 0)
		return true;
	else if (ptrArrayCount(lcb->tables) > 0)
		return true;
	else
		return false;
}

extern bool matchMultilineRegex (struct lregexControlBlock *lcb, const vString* const allLines)
{
	bool result = false;

	unsigned int i;

	for (i = 0; i < ptrArrayCount(lcb->entries [REG_PARSER_MULTI_LINE]); ++i)
	{
		regexTableEntry *entry = ptrArrayItem(lcb->entries [REG_PARSER_MULTI_LINE], i);
		Assert (entry && entry->pattern);

		if ((entry->pattern->xtagType != XTAG_UNKNOWN)
			&& (!isXtagEnabled (entry->pattern->xtagType)))
			continue;

		result = matchMultilineRegexPattern (lcb, allLines, entry) || result;
	}
	return result;
}

static int getTableIndexForName (const struct lregexControlBlock *const lcb, const char *name)
{
	unsigned int i;

	for (i = 0; i < ptrArrayCount(lcb->tables); i++)
	{
		struct regexTable *table = ptrArrayItem(lcb->tables, i);
		if (strcmp (table->name, name) == 0)
			return (int)i;
	}

	return TABLE_INDEX_UNUSED;
}

extern void addRegexTable (struct lregexControlBlock *lcb, const char *name)
{
	const char *c;
	for (c = name; *c; c++)
		if (! (isalnum((unsigned char) *c) || *c == '_'))
			error (FATAL, "`%c' in \"%s\" is not acceptable as part of table name", *c, name);

	if (getTableIndexForName(lcb, name) >= 0)
	{
		error (WARNING, "regex table \"%s\" is already defined", name);
		return;
	}

	struct regexTable *table = xCalloc(1, struct regexTable);
	table->name = eStrdup (name);
	table->entries = ptrArrayNew(deleteTableEntry);

	ptrArrayAdd (lcb->tables, table);
}

static void dumpSstack(FILE* fp, int scope)
{
	tagEntryInfo *entry;
	fprintf (fp, "scope : ");
	while ((entry = getEntryInCorkQueue (scope)))
	{
		fprintf(fp, "%s", entry->name);

		scope = entry->extensionFields.scopeIndex;
		if (scope != CORK_NIL)
			fprintf(fp, "%c", '/');
	}
	fprintf (fp, "\n");
}

static void dumpTstack(FILE* fp, ptrArray *tstack)
{
	for (unsigned int i = ptrArrayCount(tstack); i > 0; i--)
	{
		char tmp[2];
		struct regexTable *t = ptrArrayItem(tstack, i - 1);
		if (i == 1)
			tmp[0] = '\0';
		else
		{
			tmp[0] = '/';
			tmp[1] = '\0';
		}
		fprintf(fp, "%s%s", t->name, tmp);
	}
	fprintf(fp, "\n");
}

static void printInputLine(FILE* vfp, const char *c, const off_t offset)
{
	vString *v = vStringNew ();

	for (; *c && (*c != '\n'); c++)
		vStringPut(v, *c);

	if (vStringLength (v) == 0 && *c == '\n')
		vStringCatS (v, "\\n");

	fprintf (vfp, "\ninput : \"%s\" L%lu\n",
			 vStringValue (v),
			 getInputLineNumberForFileOffset(offset));
	vStringDelete(v);
}

static void printMultitableMessage(const langType language,
								   const char *const tableName,
								   const unsigned int index,
								   const regexPattern *const ptrn,
								   const off_t offset,
								   const char *const current,
								   const regmatch_t* const pmatch)
{
	vString *msg;

	Assert (ptrn);
	Assert (ptrn->message.selection > 0);
	Assert (ptrn->message.message_string);

	msg = substitute (current, ptrn->message.message_string, BACK_REFERENCE_COUNT, pmatch);

	error (ptrn->message.selection, "%sMessage from mtable<%s/%s[%2u]>: %s (%s:%lu)",
		   (ptrn->message.selection == FATAL ? "Fatal: " : ""),
		   getLanguageName (language),
		   tableName,
		   index,
		   vStringValue (msg),
		   getInputFileName (),
		   getInputLineNumberForFileOffset (offset));

	vStringDelete (msg);
}

static struct regexTable * matchMultitableRegexTable (struct lregexControlBlock *lcb,
													  struct regexTable *table, const vString *const start, unsigned int *offset)
{
	struct regexTable *next = NULL;
	const char *current;
	regmatch_t pmatch [BACK_REFERENCE_COUNT];
	const char *cstart = vStringValue(start);
	unsigned int delta;


 restart:
	current = cstart + *offset;

	/* Accept the case *offset == vStringLength(start)
	   because we want an empty regex // still matches empty input. */
	if (*offset > vStringLength(start))
	{
		*offset = vStringLength(start);
		goto out;
	}

	BEGIN_VERBOSE(vfp);
	{
		printInputLine(vfp, current, *offset);
	}
	END_VERBOSE();

	for (unsigned int i = 0; i < ptrArrayCount(table->entries); i++)
	{
		regexTableEntry *entry = ptrArrayItem(table->entries, i);
		if ((entry->pattern->xtagType != XTAG_UNKNOWN)
			&& (!isXtagEnabled (entry->pattern->xtagType)))
			continue;

		regexPattern *ptrn = entry->pattern;
		struct guestSpec  *guest = &ptrn->guest;

		Assert (ptrn);

		BEGIN_VERBOSE(vfp);
		{
			char s[3];
			if (*current == '\n')
			{
				s [0] = '\\';
				s [1] = 'n';
				s [2] = '\0';
			}
			else if (*current == '\t')
			{
				s [0] = '\\';
				s [1] = 't';
				s [2] = '\0';
			}
			else if (*current == '\\')
			{
				s [0] = '\\';
				s [1] = '\\';
				s [2] = '\0';
			}
			else
			{
				s[0] = *current;
				s[1] = '\0';
			}

			if (s[1] == '\0')
				fprintf (vfp, "match : '%s' %15s[%2u] /", s, table->name, i);
			else if (s[0] == '\0')
				fprintf (vfp, "match :  '' %15s[%2u] /", table->name, i);
			else
				fprintf (vfp, "match :'%s' %15s[%2u] / ", s, table->name, i);
			fprintf (vfp, "%s/\n", ptrn->pattern_string);
		}
		END_VERBOSE();

		int match = 0;

		if (ptrn->disabled && *(ptrn->disabled))
			continue;

		match = ptrn->pattern.backend->match (ptrn->pattern.backend,
											  ptrn->pattern.code, current,
											  vStringLength(start) - (current - cstart),
											  pmatch);
		if (match == 0)
		{
			entry->statistics.match++;
			scriptWindow window = {
				.line = current,
				.start = cstart,
				.patbuf = ptrn,
				.pmatch = pmatch,
				.nmatch = BACK_REFERENCE_COUNT,
				.advanceto = false,
			};
			initTaction (&window.taction);

			if (ptrn->optscript && (! hasNameSlot (ptrn)))
			{
				scriptSetup (optvm, lcb, CORK_NIL, &window);
				EsObject *e = scriptEval (optvm, ptrn->optscript);
				if (es_error_p (e))
					error (WARNING, "error when evaluating: %s", ptrn->optscript_src);
				es_object_unref (e);
				scriptTeardown (optvm, lcb);
			}

			if (ptrn->type == PTRN_TAG)
			{
				Assert (ptrn->mgroup.forLineNumberDetermination != NO_MULTILINE);
				off_t offset_for_tag = (current
										+ pmatch [ptrn->mgroup.forLineNumberDetermination].rm_so)
					- cstart;
				matchTagPattern (lcb, current, ptrn, pmatch, offset_for_tag,
								 (ptrn->optscript && hasNameSlot (ptrn))? &window: NULL);

				struct mTableActionSpec *taction = (window.taction.action == TACTION_NOP)
					? &(ptrn->taction)
					: &window.taction;

				BEGIN_VERBOSE(vfp);
				{
					fprintf(vfp, "result: matched %d bytes\n", (int)(pmatch[0].rm_eo));
					dumpSstack (vfp, lcb->currentScope);
				}
				END_VERBOSE();

				if (hasMessage(ptrn))
					printMultitableMessage (lcb->owner, table->name, i, ptrn,
											*offset, current, pmatch);

				if (fillGuestRequest (cstart, current, pmatch, guest, lcb->guest_req))
				{
					Assert (lcb->guest_req->lang != LANG_AUTO);
					if (isGuestRequestConsistent(lcb->guest_req))
						guestRequestSubmit (lcb->guest_req);
					guestRequestClear (lcb->guest_req);
				}

				if (window.advanceto)
					delta = window.advanceto_delta;
				else
					delta = (ptrn->mgroup.nextFromStart
							 ? pmatch [ptrn->mgroup.forNextScanning].rm_so
							 : pmatch [ptrn->mgroup.forNextScanning].rm_eo);
				*offset += delta;

				switch (taction->action)
				{
				case TACTION_NOP:
					BEGIN_VERBOSE(vfp);
					{
						fprintf(vfp, "action: NOP in {%s}, stack: /", table->name);
						dumpTstack(vfp, lcb->tstack);
					}
					END_VERBOSE();
					break;
				case TACTION_ENTER:
					/* TODO: Limit the depth of tstack.  */
					ptrArrayAdd (lcb->tstack,
								 taction->continuation_table
								 ? taction->continuation_table
								 : table);
					next = taction->table;
					BEGIN_VERBOSE(vfp);
					{
						if (taction->continuation_table)
							fprintf(vfp, "action: [enter] to {%s}, cont: {%s}, stack: /",
									next->name,
									taction->continuation_table->name);
						else
							fprintf(vfp, "action: [enter] to {%s}, stack: /", next->name);
						dumpTstack(vfp, lcb->tstack);
					}
					END_VERBOSE();
					break;
				case TACTION_LEAVE:
					BEGIN_VERBOSE(vfp);
					{
						fprintf(vfp, "action: [leave] from {%s}, stack: /", table->name);
						dumpTstack(vfp, lcb->tstack);
					}
					END_VERBOSE();
					if (ptrArrayCount (lcb->tstack) == 0)
					{
						error (WARNING, "leave is specified as regex table action but the table stack is empty");
						return NULL;
					}
					next = ptrArrayLast(lcb->tstack);
					ptrArrayRemoveLast (lcb->tstack);
					break;
				case TACTION_JUMP:
					next = taction->table;
					BEGIN_VERBOSE(vfp);
					{
						fprintf(vfp, "action: [jump] from {%s} to {%s}, stack: /", table->name, next->name);
						dumpTstack(vfp, lcb->tstack);
					}
					END_VERBOSE();

					break;
				case TACTION_RESET:
					next = taction->table;
					BEGIN_VERBOSE(vfp);
					{
						fprintf(vfp, "action: [reset] to {%s}, stack: /", next->name);
					}
					END_VERBOSE();

					ptrArrayClear (lcb->tstack);
					break;
				case TACTION_QUIT:
					BEGIN_VERBOSE(vfp);
					{
						fprintf(vfp, "action: [quit], stack: /");
						dumpTstack(vfp, lcb->tstack);
					}
					END_VERBOSE();
					return NULL;
				}

				if (next)
					break;

				if (delta == 0)
				{
					error (WARNING, "Forcefully advance the input pos because");
					error (WARNING, "following conditions for entering infinite loop are satisfied:");
					error (WARNING, "+ matching the pattern succeeds,");
					error (WARNING, "+ the next table is not given, and");
					error (WARNING, "+ the input file pos doesn't advance.");
					error (WARNING, "Language: %s, input file: %s, pos: %u",
						   getLanguageName (lcb->owner), getInputFileName(), *offset);
					++*offset;
				}
			}
			else if (ptrn->type == PTRN_CALLBACK)
				;	/* Not implemented yet */
			else
			{
				Assert ("invalid pattern type" == NULL);
				break;
			}
			goto restart;
		}
		else
			entry->statistics.unmatch++;
	}
 out:
	if (next == NULL && ptrArrayCount (lcb->tstack) > 0)
	{
		static int apop_count = 0;
		next = ptrArrayLast(lcb->tstack);
		verbose("result: no match - autopop<%d> from {%s} to {%s} @ %lu\n", apop_count++, table->name, next->name,
				getInputLineNumberForFileOffset(*offset));
		ptrArrayRemoveLast (lcb->tstack);
	}
	return next;
}

extern void extendRegexTable (struct lregexControlBlock *lcb, const char *src, const char *dist)
{

	int i;
	struct regexTable * src_table;
	struct regexTable * dist_table;

	verbose ("extend regex table  \"%s\" with \"%s\"\n", dist, src);

	i = getTableIndexForName (lcb, src);
	if (i < 0)
		error (FATAL, "no such regex table in %s: %s", getLanguageName(lcb->owner), src);
	src_table = ptrArrayItem(lcb->tables, i);

	i = getTableIndexForName (lcb, dist);
	if (i < 0)
		error (FATAL, "no such regex table in %s: %s", getLanguageName(lcb->owner), dist);
	dist_table = ptrArrayItem(lcb->tables, i);

	for (unsigned int n = 0; n < ptrArrayCount(src_table->entries); n++)
	{
		regexTableEntry *entry = ptrArrayItem (src_table->entries, n);
		ptrArrayAdd(dist_table->entries, newRefPatternEntry(entry));
	}
}

extern void printMultitableStatistics (struct lregexControlBlock *lcb)
{
	if (ptrArrayCount(lcb->tables) == 0)
		return;

	fprintf(stderr, "\nMTABLE REGEX STATISTICS of %s\n", getLanguageName (lcb->owner));
	fputs("==============================================\n", stderr);
	for (unsigned int i = 0; i < ptrArrayCount(lcb->tables); i++)
	{
		struct regexTable *table = ptrArrayItem (lcb->tables, i);
		fprintf(stderr, "%s\n", table->name);
		fputs("-----------------------\n", stderr);
		for (unsigned int j = 0; j < ptrArrayCount(table->entries); j++)
		{
			regexTableEntry *entry = ptrArrayItem (table->entries, j);
			Assert (entry && entry->pattern);
			fprintf(stderr, "%10u/%-10u%-40s ref: %d\n",
					entry->statistics.match,
					entry->statistics.unmatch + entry->statistics.match,
					entry->pattern->pattern_string,
					entry->pattern->refcount);
		}
		fputc('\n', stderr);
	}
}

extern bool matchMultitableRegex (struct lregexControlBlock *lcb, const vString* const allLines)
{
	if (ptrArrayCount (lcb->tables) == 0)
		return false;

	struct regexTable *table = ptrArrayItem (lcb->tables, 0);
	unsigned int offset = 0;

	int motionless_counter = 0;
	unsigned int last_offset;


	while (table)
	{
		last_offset = offset;
		table = matchMultitableRegexTable(lcb, table, allLines, &offset);

		if (last_offset == offset)
			motionless_counter++;
		else
			motionless_counter = 0;

		if (motionless_counter > MTABLE_MOTIONLESS_MAX)
		{
			error (WARNING, "mtable<%s/%s>: the input cursor stays at %u in %s so long though the tables are switched",
				   getLanguageName (lcb->owner),
				   table->name, offset, getInputFileName ());
			break;
		}

		if (table && (ptrArrayCount (lcb->tstack) > MTABLE_STACK_MAX_DEPTH))
		{
			unsigned int i;
			struct regexTable *t;

			error (WARNING, "mtable<%s/%s>: the tenter/tleave stack overflows at %u in %s",
				   getLanguageName (lcb->owner),
				   table->name, offset, getInputFileName ());
			error (WARNING, "DUMP FROM THE TOP:");
			/* TODO: use dumpTstack */
			for (i = ptrArrayCount(lcb->tstack); 0 < i; --i)
			{
				t = ptrArrayItem (lcb->tstack, i - 1);
				error (WARNING, "%3u %s", i - 1, t->name);
			}

			break;
		}
	}

	return true;
}

static int  makePromiseForAreaSpecifiedWithOffsets (const char *parser,
													off_t startOffset,
													off_t endOffset)
{
	unsigned long startLine = getInputLineNumberForFileOffset(startOffset);
	unsigned long endLine = getInputLineNumberForFileOffset(endOffset);
	unsigned long startLineOffset = getInputFileOffsetForLine (startLine);
	unsigned long endLineOffset = getInputFileOffsetForLine (endLine);

	Assert(startOffset >= startLineOffset);
	Assert(endOffset >= endLineOffset);

	return makePromise (parser,
						startLine, startOffset - startLineOffset,
						endLine, endOffset - endLineOffset,
						startOffset - startLineOffset);
}

static struct guestRequest *guestRequestNew (void)
{
	struct guestRequest *r = xMalloc (1, struct guestRequest);


	guestRequestClear (r);
	return r;
}

static void   guestRequestDelete (struct guestRequest *r)
{
	eFree (r);
}

static bool   guestRequestIsFilled(struct guestRequest *r)
{
	return (r->lang_set && (r->boundary + 0)->offset_set && (r->boundary + 1)->offset_set);
}

static void   guestRequestClear (struct guestRequest *r)
{
	r->lang_set = false;
	r->boundary[GUEST_BOUNDARY_START].offset_set = false;
	r->boundary[GUEST_BOUNDARY_END].offset_set = false;
}

static void   guestRequestSubmit (struct guestRequest *r)
{
	const char *langName = getLanguageName (r->lang);
	verbose ("guestRequestSubmit: %s; "
			 "range: %"PRId64" - %"PRId64"\n",
			 langName,
			 (int64_t)r->boundary[GUEST_BOUNDARY_START].offset,
			 (int64_t)r->boundary[GUEST_BOUNDARY_END].offset);
	makePromiseForAreaSpecifiedWithOffsets (langName,
											r->boundary[GUEST_BOUNDARY_START].offset,
											r->boundary[GUEST_BOUNDARY_END].offset);
}

/*
 * Script related functions
 */

/* This functions expects { code }} as input.
 * Be care that curly brackets must be unbalanced.
 */
static EsObject *scriptRead (OptVM *vm, const char *src)
{
	size_t len = strlen (src);
	Assert (len > 2);
	Assert (src[len - 1] == '}');
	Assert (src[len - 2] == '}');

	EsObject *obj = optscriptRead (vm, src + 1, len - 1 - 1);
	if (es_error_p (obj))
		error (FATAL, "failed in loading an optscript: %s", src);
	return obj;
}

extern EsObject* scriptEval (OptVM *vm, EsObject *optscript)
{
	return optscriptEval (vm, optscript);
}

static void scriptEvalHook (OptVM *vm, struct lregexControlBlock *lcb, enum scriptHook hook)
{
	if (ptrArrayCount (lcb->hook_code[hook]) == 0)
	{
		for (unsigned int i = 0; i < ptrArrayCount (lcb->hook[hook]); i++)
		{
			const char *src = ptrArrayItem (lcb->hook[hook], i);
			EsObject *code = scriptRead (vm, src);
			if (es_error_p (code))
				error (FATAL, "error when reading hook[%d] code: %s", hook, src);
			ptrArrayAdd (lcb->hook_code[hook], es_object_ref (code));
			es_object_unref (code);
		}
	}
	for (unsigned int i = 0; i < ptrArrayCount (lcb->hook_code[hook]); i++)
	{
		EsObject *code = ptrArrayItem (lcb->hook_code[hook], i);
		EsObject * e = optscriptEval (vm, code);
		if (es_error_p (e))
			error (WARNING, "error when evaluating hook[%d] code: %s",
				   hook, (char *)ptrArrayItem (lcb->hook[i], i));
	}
}

static void scriptSetup (OptVM *vm, struct lregexControlBlock *lcb, int corkIndex, scriptWindow *window)
{
	set_current_lcb (vm, lcb);
	set_current_window (vm, window);
	optscriptSetup (vm, lcb->local_dict, corkIndex);
}

static void scriptTeardown (OptVM *vm, struct lregexControlBlock *lcb)
{
	optscriptTeardown (vm, lcb->local_dict);
	set_current_lcb (vm, NULL);
	set_current_window (vm, NULL);
}

extern void	addOptscriptToHook (struct lregexControlBlock *lcb, enum scriptHook hook, const char *code)
{
	ptrArrayAdd (lcb->hook[hook], eStrdup (code));
}

extern void propagateParamToOptscript (struct lregexControlBlock *lcb, const char *param, const char *value)
{
	Assert (param);
	Assert (value);
	hashTablePutItem (lcb->param_dict, eStrdup (param), eStrdup (value));
}

/* Return true if available. */
extern bool checkRegex (void)
{
#if defined (CHECK_REGCOMP)
	{
		/* Check for broken regcomp() on Cygwin */
		regex_t patbuf;
		int errcode;
		if (regcomp (&patbuf, "/hello/", 0) != 0)
			error (WARNING, "Disabling broken regex");
		else
			regexAvailable = true;
	}
#else
	/* We are using bundled regex engine. */
	regexAvailable = true;
#endif

	return regexAvailable;
}

static EsObject *OPTSCRIPT_ERR_UNKNOWNKIND;
static EsObject *OPTSCRIPT_ERR_UNKNOWNROLE;

static EsObject* lrop_make_foreignreftag (OptVM *vm, EsObject *name)
{
	matchLoc *loc;

	if (opt_vm_ostack_count (vm) < 1)
		return OPT_ERR_UNDERFLOW;

	int index;
	EsObject *top = opt_vm_ostack_top (vm);
	if (es_object_get_type (top) == OPT_TYPE_MATCHLOC)
	{
		if (opt_vm_ostack_count (vm) < 5)
			return OPT_ERR_UNDERFLOW;
		loc = es_pointer_get (top);
		index = 1;
	}
	else
	{
		scriptWindow *window = get_current_window(vm);
		if (window->patbuf->regptype != REG_PARSER_SINGLE_LINE)
			return OPT_ERR_TYPECHECK;
		if (opt_vm_ostack_count (vm) < 4)
			return OPT_ERR_UNDERFLOW;
		loc = NULL;
		index = 0;
	}

	EsObject *role_obj = opt_vm_ostack_peek (vm, index++);
	if (es_nil != role_obj
		&& es_object_get_type (role_obj) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	EsObject *kind_obj = opt_vm_ostack_peek (vm, index++);
	if (es_object_get_type (kind_obj) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	EsObject *lang_obj = opt_vm_ostack_peek (vm, index++);
	langType lang;
	if (es_nil == lang_obj)
		lang = getInputLanguage ();
	else if (es_object_get_type (lang_obj) == OPT_TYPE_NAME)
	{
		EsObject *lang_sym = es_pointer_get (lang_obj);
		const char *lang_str = es_symbol_get (lang_sym);
		lang = getNamedLanguage (lang_str, 0);
		if (lang == LANG_IGNORE)
			return OPTSCRIPT_ERR_UNKNOWNLANGUAGE;
	}
	else
		return OPT_ERR_TYPECHECK;

	EsObject *kind_sym = es_pointer_get (kind_obj);
	const char *kind_str = es_symbol_get (kind_sym);
	kindDefinition* kind_def = getLanguageKindForName (lang, kind_str);
	if (!kind_def)
		return OPTSCRIPT_ERR_UNKNOWNKIND;
	int kind_index = kind_def->id;

	int role_index;
	if (es_nil == role_obj)
		role_index = ROLE_DEFINITION_INDEX;
	else
	{
		EsObject *role_sym = es_pointer_get (role_obj);
		const char *role_str = es_symbol_get (role_sym);
		roleDefinition* role_def = getLanguageRoleForName (lang, kind_index, role_str);
		if (!role_def)
			return OPTSCRIPT_ERR_UNKNOWNROLE;
		role_index = role_def->id;
	}

	EsObject *tname = opt_vm_ostack_peek (vm, index++);
	if (es_object_get_type (tname) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;
	const char *n = opt_string_get_cstr (tname);
	if (n [0] == '\0')
		return OPT_ERR_RANGECHECK; /* TODO */

	tagEntryInfo *e = xMalloc (1, tagEntryInfo);
	initRegexTag (e, eStrdup (n),
				  kind_index, role_index, CORK_NIL, false,
				  loc? loc->line: 0, loc? &loc->pos: NULL,
				  role_index == ROLE_DEFINITION_INDEX
				  ? XTAG_UNKNOWN
				  : XTAG_REFERENCE_TAGS, lang);
	EsObject *obj = es_pointer_new (OPT_TYPE_TAG, e);
	if (es_error_p (obj))
		return obj;

	while (index-- > 0)
		opt_vm_ostack_pop (vm);

	opt_vm_ostack_push (vm, obj);
	es_object_unref (obj);
	return es_false;
}

static EsObject* lrop_assign_role_common (OptVM *vm, EsObject *name, bool assign)
{
	EsObject *tag = opt_vm_ostack_peek (vm, 1);
	tagEntryInfo *e;
	if (es_integer_p (tag))
	{
		int n0 = es_integer_get (tag);
		if (n0 < 0)
			return OPT_ERR_RANGECHECK;
		unsigned int n = n0;
		if (! (CORK_NIL < n && n < countEntryInCorkQueue()))
			return OPT_ERR_RANGECHECK;
		e = getEntryInCorkQueue (n);
	}
	else if (es_object_get_type (tag) == OPT_TYPE_TAG)
		e = es_pointer_get (tag);
	else
		return OPT_ERR_TYPECHECK;

	if (e == NULL)
		return OPTSCRIPT_ERR_NOTAGENTRY;

	langType lang = e->langType;
	int kind_index = e->kindIndex;
	EsObject *role = opt_vm_ostack_top (vm);
	if (es_object_get_type (role) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;
	EsObject *role_sym = es_pointer_get (role);
	const char *role_str = es_symbol_get (role_sym);
	roleDefinition* role_def = getLanguageRoleForName (lang, kind_index, role_str);
	if (!role_def)
		return OPTSCRIPT_ERR_UNKNOWNROLE;
	int role_index = role_def->id;

	(assign? assignRole: unassignRole) (e, role_index);

	opt_vm_ostack_pop (vm);
	opt_vm_ostack_pop (vm);

	return es_false;
}

static EsObject* lrop_assign_role (OptVM *vm, EsObject *name)
{
	return lrop_assign_role_common (vm, name, true);
}

static EsObject* lrop_unassign_role (OptVM *vm, EsObject *name)
{
	return lrop_assign_role_common (vm, name, false);
}

/* tag COMMIT int */
static EsObject* lrop_commit_tag (OptVM *vm, EsObject *name)
{
	EsObject *tag = opt_vm_ostack_top (vm);
	if (es_object_get_type (tag) != OPT_TYPE_TAG)
		return OPT_ERR_TYPECHECK;

	tagEntryInfo *e = es_pointer_get (tag);
	int corkIndex = makeTagEntry (e);
	EsObject *n = es_integer_new (corkIndex);
	if (es_error_p (n))
		return n;
	opt_vm_ostack_pop (vm);
	opt_vm_ostack_push (vm, n);
	es_object_unref (n);
	return es_false;
}

static EsObject* lrop_get_match_loc (OptVM *vm, EsObject *name)
{

	bool start;
	EsObject *group;

	if (opt_vm_ostack_count (vm) < 1)
		return OPT_ERR_UNDERFLOW;

	EsObject *tmp = opt_vm_ostack_top (vm);

	if (es_object_get_type (tmp) == ES_TYPE_INTEGER)
	{
		group = tmp;
		start = true;
	}
	else
	{
		EsObject *pos = tmp;

		static EsObject *start_name, *end_name;
		if (!start_name)
		{
			start_name = opt_name_new_from_cstr ("start");
			end_name = opt_name_new_from_cstr ("end");
		}

		if (es_object_equal (pos, start_name))
			start = true;
		else if (es_object_equal (pos, end_name))
			start = false;
		else
			return OPT_ERR_TYPECHECK;

		if (opt_vm_ostack_count (vm) < 2)
			return OPT_ERR_UNDERFLOW;

		group = opt_vm_ostack_peek (vm, 1);
		if (es_object_get_type (group) != ES_TYPE_INTEGER)
			return OPT_ERR_TYPECHECK;
	}

	int g = es_integer_get (group);
	if (g < 1)
		return OPT_ERR_RANGECHECK;

	scriptWindow *window = get_current_window (vm);

	matchLoc *mloc = make_mloc (window, g, start);
	if (mloc == NULL)
		return OPT_ERR_RANGECHECK;

	EsObject * mlocobj = es_pointer_new (OPT_TYPE_MATCHLOC, mloc);
	if (es_error_p (mlocobj))
	{
		eFree (mloc);
		return mlocobj;
	}

	if (group != tmp)
		opt_vm_ostack_pop (vm);
	opt_vm_ostack_pop (vm);
	opt_vm_ostack_push (vm, mlocobj);
	es_object_unref (mlocobj);
	return es_false;
}

static EsObject* ldrop_get_line_from_matchloc (OptVM *vm, EsObject *name)
{
	EsObject *mlocobj = opt_vm_ostack_top (vm);
	if (es_object_get_type (mlocobj) != OPT_TYPE_MATCHLOC)
		return OPT_ERR_TYPECHECK;

	matchLoc *mloc = es_pointer_get (mlocobj);
	EsObject *lineobj = es_integer_new (mloc->line);
	if (es_error_p (lineobj))
		return lineobj;

	opt_vm_ostack_pop (vm);
	opt_vm_ostack_push (vm, lineobj);
	es_object_unref (lineobj);
	return es_false;
}

static matchLoc* make_mloc_from_tagEntryInfo(tagEntryInfo *e)
{
	matchLoc *mloc = xMalloc (1, matchLoc);
	mloc->delta = 0;
	mloc->line = e->lineNumber;
	mloc->pos = e->filePosition;

	return mloc;
}

static EsObject* lrop_get_tag_loc (OptVM *vm, EsObject *name)
{
	EsObject *nobj = opt_vm_ostack_top (vm);

	if (es_object_get_type (nobj) != ES_TYPE_INTEGER)
		return OPT_ERR_TYPECHECK;

	int n0 = es_integer_get(nobj);
	if (n0 < 0)
		return OPT_ERR_RANGECHECK;
	unsigned int n = n0;
	if (! (CORK_NIL < n && n < countEntryInCorkQueue()))
			return OPT_ERR_RANGECHECK;

	tagEntryInfo *e = getEntryInCorkQueue (n);
	if (e == NULL)
		return OPT_ERR_TYPECHECK; /* ??? */

	matchLoc *mloc = make_mloc_from_tagEntryInfo (e);
	EsObject * mlocobj = es_pointer_new (OPT_TYPE_MATCHLOC, mloc);
	if (es_error_p (mlocobj))
	{
		eFree (mloc);
		return mlocobj;
	}

	opt_vm_ostack_pop (vm);
	opt_vm_ostack_push (vm, mlocobj);
	es_object_unref (mlocobj);
	return es_false;
}

static EsObject* lrop_get_match_string_common (OptVM *vm, int i, int npop)
{
	scriptWindow *window = get_current_window (vm);
	const char *cstr = make_match_string (window, i);
	if (!cstr)
	{
		for (; npop > 0; npop--)
			opt_vm_ostack_pop (vm);
		opt_vm_ostack_push (vm, es_false);
		return es_false;
	}
	EsObject *str = opt_string_new_from_cstr (cstr);
	eFree ((void *)cstr);

	for (; npop > 0; npop--)
		opt_vm_ostack_pop (vm);

	opt_vm_ostack_push (vm, str);
	es_object_unref (str);
	return es_false;
}

/* Handles \1, \2, ... */
static EsObject* lrop_get_match_string_named_group (OptVM *vm, EsObject *name)
{
	void * data = es_symbol_get_data (name);
	int i = HT_PTR_TO_INT (data);

	return lrop_get_match_string_common (vm, i, 0);
}

static EsObject* lrop_get_match_string_group_on_stack (OptVM *vm, EsObject *name)
{
	EsObject *group = opt_vm_ostack_top (vm);
	if (!es_integer_p (group))
		return OPT_ERR_TYPECHECK;

	int g = es_integer_get (group);
	if (g < 1)
		return OPT_ERR_RANGECHECK;

	EsObject *r = lrop_get_match_string_common (vm, g, 1);
	if (es_error_p (r))
		return r;

	r = opt_vm_ostack_top (vm);
	if (es_object_get_type (r) == OPT_TYPE_STRING)
		opt_vm_ostack_push (vm, es_true);
	return es_false;
}

static char* make_match_string (scriptWindow *window, int group)
{
	if (window == NULL
		|| 0 >= group
		|| window->nmatch <= group
		|| window->pmatch [group].rm_so == -1)
		return NULL;

	const int len = window->pmatch [group].rm_eo - window->pmatch [group].rm_so;
	const char *start = window->line + window->pmatch [group].rm_so;

	return eStrndup (start, len);
}

static matchLoc *make_mloc (scriptWindow *window, int group, bool start)
{
	if (window == NULL
		|| 0 > group
		|| window->nmatch <= group
		|| window->pmatch [group].rm_so == -1)
		return NULL;

	matchLoc *mloc = xMalloc (1, matchLoc);
	if (window->patbuf->regptype == REG_PARSER_SINGLE_LINE)
	{
		mloc->base  = 0;
		mloc->delta = 0;
		mloc->line = getInputLineNumber ();
		mloc->pos = getInputFilePosition ();
	}
	else
	{
		mloc->base  = window->line - window->start;
		mloc->delta = (start
					   ? window->pmatch [group].rm_so
					   : window->pmatch [group].rm_eo);
		mloc->line = getInputLineNumberForFileOffset (mloc->base + mloc->delta);
		mloc->pos  = getInputFilePositionForLine (mloc->line);
	}
	return mloc;
}

static EsObject* lrop_set_scope (OptVM *vm, EsObject *name)
{
	EsObject *corkIndex = opt_vm_ostack_top (vm);
	if (!es_integer_p (corkIndex))
		return OPT_ERR_TYPECHECK;

	int n0 = es_integer_get (corkIndex);
	if (n0 < 0)
		return OPT_ERR_RANGECHECK;
	unsigned int n = n0;
	if (n >= countEntryInCorkQueue())
		return OPT_ERR_RANGECHECK;

	struct lregexControlBlock *lcb = get_current_lcb (vm);
	lcb->currentScope = n0;

	opt_vm_ostack_pop (vm);

	return es_false;
}

static EsObject* lrop_pop_scope (OptVM *vm, EsObject *name)
{
	struct lregexControlBlock *lcb = get_current_lcb (vm);
	if (lcb->currentScope != CORK_NIL)
	{
		tagEntryInfo *e = getEntryInCorkQueue (lcb->currentScope);
		if (e)
			lcb->currentScope = e->extensionFields.scopeIndex;
	}
	return es_false;
}

static EsObject* lrop_clear_scope (OptVM *vm, EsObject *name)
{
	struct lregexControlBlock *lcb = get_current_lcb (vm);
	lcb->currentScope = CORK_NIL;
	return es_false;
}

static EsObject* lrop_ref0_scope (OptVM *vm, EsObject *name)
{
	struct lregexControlBlock *lcb = get_current_lcb (vm);

	if (lcb->currentScope == 0)
	{
		opt_vm_ostack_push (vm, es_false);
		return es_false;
	}

	EsObject *q = es_integer_new (lcb->currentScope);

	if (es_error_p (q))
		return q;

	opt_vm_ostack_push (vm, q);
	es_object_unref (q);
	opt_vm_ostack_push (vm, es_true);
	return es_false;
}

static EsObject* lrop_refN_scope (OptVM *vm, EsObject *name)
{
	EsObject *nobj = opt_vm_ostack_top (vm);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get(nobj);

	struct lregexControlBlock *lcb = get_current_lcb (vm);
	int scope = lcb->currentScope;

	while (n--)
	{
		if (scope == CORK_NIL)
			break;
		tagEntryInfo *e = getEntryInCorkQueue (scope);
		if (e == NULL)
			break;

		scope = e->extensionFields.scopeIndex;
	}

	EsObject *q = es_integer_new (scope);
	if (es_error_p(q))
		return q;

	opt_vm_ostack_pop (vm);
	opt_vm_ostack_push (vm, q);
	es_object_unref (q);

	return es_false;
}

static EsObject* lrop_get_scope_depth (OptVM *vm, EsObject *name)
{
	struct lregexControlBlock *lcb = get_current_lcb (vm);
	int scope = lcb->currentScope;
	int depth = 0;

	while (scope != CORK_NIL)
	{
		tagEntryInfo *e = getEntryInCorkQueue (scope);
		if (!e)
			break;
		depth++;
		scope = e->extensionFields.scopeIndex;
	}

	EsObject *q = es_integer_new (depth);
	if (es_error_p(q))
		return q;

	opt_vm_ostack_push (vm, q);
	es_object_unref (q);
	return es_false;
}

static EsObject* lrop_repl (OptVM *vm, EsObject *name)
{
	char *old_prompt = opt_vm_set_prompt (vm, "\n% type \"quit\" for exiting from repl\nOPT");

	opt_vm_print_prompt (vm);
	opt_vm_set_prompt (vm, "OPT");

	while (true)
	{
		EsObject *o = opt_vm_read (vm, NULL);
		if (es_object_equal (o, ES_READER_EOF))
		{
			es_object_unref (o);
			break;
		}
		EsObject *e = opt_vm_eval (vm, o);
		es_object_unref (o);

		if (es_error_p (e))
		{
			if (!es_object_equal (e, OPT_ERR_QUIT))
				opt_vm_report_error (vm, e, NULL);
			break;
		}
	}

	opt_vm_set_prompt (vm, old_prompt);
	return es_false;
}

static EsObject *OPTSCRIPT_ERR_UNKNOWNTABLE;
static EsObject *OPTSCRIPT_ERR_NOTMTABLEPTRN;

static struct regexTable *getRegexTableForOptscriptName (struct lregexControlBlock *lcb,
														 EsObject *tableName)
{
	EsObject *table_sym = es_pointer_get (tableName);
	const char *table_str = es_symbol_get (table_sym);
	int n = getTableIndexForName (lcb, table_str);
	if (n < 0)
		return NULL;
	return ptrArrayItem (lcb->tables, n);
}

static EsObject* lrop_tenter_common (OptVM *vm, EsObject *name, enum tableAction action)
{
	scriptWindow *window = get_current_window (vm);
	if (window->patbuf->regptype != REG_PARSER_MULTI_TABLE)
	{
		error (WARNING, "Use table related operators only with mtable regular expression");
		return OPTSCRIPT_ERR_NOTMTABLEPTRN;
	}

	EsObject *table = opt_vm_ostack_top (vm);
	if (es_object_get_type (table) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	struct lregexControlBlock *lcb = get_current_lcb(vm);
	struct regexTable *t = getRegexTableForOptscriptName (lcb, table);
	if (t == NULL)
		return OPTSCRIPT_ERR_UNKNOWNTABLE;

	window->taction = (struct mTableActionSpec){
		.action             = action,
		.table              = t,
		.continuation_table = NULL,
	};

	opt_vm_ostack_pop (vm);
	return es_false;
}

static EsObject* lrop_tenter (OptVM *vm, EsObject *name)
{
	return lrop_tenter_common (vm, name, TACTION_ENTER);
}

static EsObject* lrop_tenter_with_continuation (OptVM *vm, EsObject *name)
{
	scriptWindow *window = get_current_window (vm);
	if (window->patbuf->regptype != REG_PARSER_MULTI_TABLE)
	{
		error (WARNING, "Use table related operators only with mtable regular expression");
		return OPTSCRIPT_ERR_NOTMTABLEPTRN;
	}

	EsObject *cont = opt_vm_ostack_top (vm);
	EsObject *table = opt_vm_ostack_peek (vm, 1);

	if (es_object_get_type (table) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;
	if (es_object_get_type (cont) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	struct lregexControlBlock *lcb = get_current_lcb (vm);
	struct regexTable *t = getRegexTableForOptscriptName (lcb, table);
	if (t == NULL)
		return OPTSCRIPT_ERR_UNKNOWNTABLE;
	struct regexTable *c = getRegexTableForOptscriptName (lcb, cont);
	if (c == NULL)
		return OPTSCRIPT_ERR_UNKNOWNTABLE;

	window->taction = (struct mTableActionSpec){
		.action             = TACTION_ENTER,
		.table              = t,
		.continuation_table = c,
	};

	opt_vm_ostack_pop (vm);
	opt_vm_ostack_pop (vm);
	return es_false;
}

static EsObject* lrop_tleave (OptVM *vm, EsObject *name)
{
	scriptWindow *window = get_current_window (vm);
	if (window->patbuf->regptype != REG_PARSER_MULTI_TABLE)
	{
		error (WARNING, "Use table related operators only with mtable regular expression");
		return OPTSCRIPT_ERR_NOTMTABLEPTRN;
	}

	window->taction.action = TACTION_LEAVE;
	return es_false;
}

static EsObject* lrop_tjump (OptVM *vm, EsObject *name)
{
	return lrop_tenter_common (vm, name, TACTION_JUMP);
}

static EsObject* lrop_treset (OptVM *vm, EsObject *name)
{
	return lrop_tenter_common (vm, name, TACTION_RESET);
}

static EsObject* lrop_tquit (OptVM *vm, EsObject *name)
{
	scriptWindow *window = get_current_window (vm);
	if (window->patbuf->regptype != REG_PARSER_MULTI_TABLE)
	{
		error (WARNING, "Use table related operators only with mtable regular expression");
		return OPTSCRIPT_ERR_NOTMTABLEPTRN;
	}

	window->taction.action = TACTION_QUIT;
	return es_false;
}

static EsObject* lrop_traced (OptVM *vm, EsObject *name)
{
#ifdef DO_TRACING
	langType lang = getInputLanguage ();
	if (isLanguageTraced (lang))
		opt_vm_ostack_push (vm, es_true);
	else
		opt_vm_ostack_push (vm, es_false);
#else
	opt_vm_ostack_push (vm, es_false);
#endif
	return false;
}

EsObject *OPTSCRIPT_ERR_UNKNOWNEXTRA;
static EsObject* lrop_extraenabled (OptVM *vm, EsObject *name)
{
	EsObject *extra = opt_vm_ostack_top (vm);
	if (es_object_get_type (extra) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	xtagType xt = optscriptGetXtagType (extra);
	if (xt == XTAG_UNKNOWN)
		return OPTSCRIPT_ERR_UNKNOWNEXTRA;

	EsObject *r = isXtagEnabled (xt)? es_true: es_false;
	opt_vm_ostack_pop (vm);
	opt_vm_ostack_push (vm, r);
	return es_false;
}

static EsObject *lrop_markextra (OptVM *vm, EsObject *name)
{
	EsObject *tag = opt_vm_ostack_peek (vm, 1);
	tagEntryInfo *e;
	if (es_integer_p (tag))
	{
		int n0 = es_integer_get (tag);
		if (n0 < 0)
			return OPT_ERR_RANGECHECK;
		unsigned int n = n0;
		if (! (CORK_NIL < n && n < countEntryInCorkQueue()))
			return OPT_ERR_RANGECHECK;
		e = getEntryInCorkQueue (n0);
	}
	else if (es_object_get_type (tag) == OPT_TYPE_TAG)
		e = es_pointer_get (tag);
	else
		return OPT_ERR_TYPECHECK;

	if (e == NULL)
		return OPTSCRIPT_ERR_NOTAGENTRY;

	EsObject *extra = opt_vm_ostack_top (vm);
	if (es_object_get_type (extra) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	xtagType xt = optscriptGetXtagType (extra);
	if (xt == XTAG_UNKNOWN)
		return OPTSCRIPT_ERR_UNKNOWNEXTRA;

	langType lang = getXtagLanguage (xt);
	if (lang != LANG_IGNORE && e->langType != lang)
	{
		error (WARNING,
			   "mismatch in the language of the tag (%s) and the language of field (%s)",
			   getLanguageName (e->langType), getLanguageName (lang));
		return OPTSCRIPT_ERR_UNKNOWNEXTRA;
	}

	markTagExtraBit (e, xt);

	opt_vm_ostack_pop (vm);
	opt_vm_ostack_pop (vm);

	return es_false;
}

static EsObject *lrop_advanceto (OptVM *vm, EsObject *name)
{
	scriptWindow *window = get_current_window (vm);
	if (window->patbuf->regptype == REG_PARSER_SINGLE_LINE)
	{
		error (WARNING, "don't use `%s' operator in --regex-<LANG> option",
			   es_symbol_get (name));
		return OPTSCRIPT_ERR_NOTMTABLEPTRN; /* TODO */
	}

	EsObject *mlocobj = opt_vm_ostack_top (vm);
	if (es_object_get_type (mlocobj) != OPT_TYPE_MATCHLOC)
		return OPT_ERR_TYPECHECK;

	matchLoc *loc = es_pointer_get (mlocobj);
	window->advanceto = true;
	window->advanceto_delta = loc->delta;

	return es_true;
}

static EsObject *lrop_markplaceholder (OptVM *vm, EsObject *name)
{
	EsObject *tag = opt_vm_ostack_top (vm);

	if (!es_integer_p (tag))
		return OPT_ERR_TYPECHECK;

	int n0 = es_integer_get (tag);
	if (n0 < 0)
		return OPT_ERR_RANGECHECK;

	unsigned int n = n0;
	if (! (CORK_NIL < n && n < countEntryInCorkQueue()))
		return OPT_ERR_RANGECHECK;

	tagEntryInfo *e = getEntryInCorkQueue (n0);
	if (e == NULL)
		return OPTSCRIPT_ERR_NOTAGENTRY;

	markTagAsPlaceholder (e, true);

	opt_vm_ostack_pop (vm);
	return es_false;
}

static EsObject *lrop_makepromise (OptVM *vm, EsObject *name)
{
	scriptWindow *window = get_current_window (vm);
	if (window->patbuf->regptype == REG_PARSER_SINGLE_LINE)
	{
		error (WARNING, "don't use `%s' operator in --regex-<LANG> option",
			   es_symbol_get (name));
		return OPTSCRIPT_ERR_NOTMTABLEPTRN; /* TODO */
	}

	EsObject *endobj = opt_vm_ostack_top (vm);
	if (es_object_get_type (endobj) != OPT_TYPE_MATCHLOC)
		return OPT_ERR_TYPECHECK;
	matchLoc *end = es_pointer_get (endobj);
	off_t end_off = (off_t)(end->base + end->delta);

	EsObject *startobj = opt_vm_ostack_peek (vm, 1);
	if (es_object_get_type (startobj) != OPT_TYPE_MATCHLOC)
		return OPT_ERR_TYPECHECK;
	matchLoc *start = es_pointer_get (startobj);
	off_t start_off = (off_t)(start->base + start->delta);

	if (! (start_off < end_off))
		return OPT_ERR_RANGECHECK;

	EsObject *lang = opt_vm_ostack_peek (vm, 2);
	const char *langc = opt_string_get_cstr (lang);
	langType t = getNamedLanguageOrAlias (langc, 0);
	if (t == LANG_IGNORE)
		return OPTSCRIPT_ERR_UNKNOWNLANGUAGE;

	if (start_off == end_off)
	{
		opt_vm_ostack_pop (vm);
		opt_vm_ostack_pop (vm);
		opt_vm_ostack_pop (vm);
		opt_vm_ostack_push (vm, es_false);
		return es_false;
	}

	int promise = makePromiseForAreaSpecifiedWithOffsets (langc,
														  start_off,
														  end_off);
	opt_vm_ostack_pop (vm);
	opt_vm_ostack_pop (vm);
	opt_vm_ostack_pop (vm);

	if (promise >= 0)
	{
		EsObject *promise_obj = es_integer_new (promise);
		opt_vm_ostack_push (vm, promise_obj);
		opt_vm_ostack_push (vm, es_true);
		es_object_unref(promise_obj);
	}
	else
		opt_vm_ostack_push (vm, es_false);

	return es_false;
}

static EsObject *lrop_param (OptVM *vm, EsObject *name)
{
	struct lregexControlBlock *lcb = get_current_lcb (vm);
	EsObject *key = opt_vm_ostack_top (vm);
	if (es_object_get_type (key) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;
	EsObject *key_sym = es_pointer_get (key);
	const char *keyc = es_symbol_get (key_sym);
	const char *valuec = hashTableGetItem (lcb->param_dict, keyc);

	if (valuec)
	{
		opt_vm_ostack_pop (vm);
		EsObject *value = opt_string_new_from_cstr (valuec);
		opt_vm_ostack_push (vm, value);
		es_object_unref (value);
		opt_vm_ostack_push (vm, es_true);
	}
	else
	{
		opt_vm_ostack_pop (vm);
		opt_vm_ostack_push (vm, es_false);
	}
	return false;
}

static EsObject *lrop_intervaltab (OptVM *vm, EsObject *name)
{
	EsObject *nobj = opt_vm_ostack_top (vm);
	int parent;

	if (es_object_get_type (nobj) == ES_TYPE_INTEGER)
	{
		int index = es_integer_get(nobj);
		if (index < 0 || index == CORK_NIL)
			return OPT_ERR_RANGECHECK;
		parent = queryIntervalTabByCorkEntry (index);
	}
	else if (es_object_get_type (nobj) == OPT_TYPE_TAG)
	{
		tagEntryInfo *e = es_pointer_get (nobj);
		if (e->extensionFields._endLine)
			parent =  queryIntervalTabByRange(e->lineNumber,
											  e->extensionFields._endLine);
		else
			parent = queryIntervalTabByLine(e->lineNumber);
	}
	else if (es_object_get_type (nobj) == OPT_TYPE_MATCHLOC)
	{
		matchLoc *mloc = es_pointer_get (nobj);
		parent = queryIntervalTabByLine(mloc->line);
	}
	else if (es_object_get_type (nobj) == OPT_TYPE_ARRAY)
	{
		unsigned long start, end;

		if (opt_array_length(nobj) == 0)
			return OPT_ERR_RANGECHECK;

		ptrArray *a = es_pointer_get (nobj);
		EsObject *nobj0 = ptrArrayItem (a, 0);
		if (es_object_get_type (nobj0) != ES_TYPE_INTEGER)
			return OPT_ERR_TYPECHECK;
		int n = es_integer_get (nobj0);
		if (n <= 0)
			return OPT_ERR_RANGECHECK;

		start = (unsigned long)n;
		if (ptrArrayCount(a) == 1)
			parent = queryIntervalTabByLine (start);
		else
		{
			nobj0 = ptrArrayItem (a, 1);
			if (es_object_get_type (nobj0) != ES_TYPE_INTEGER)
				return OPT_ERR_TYPECHECK;
			int n = es_integer_get (nobj0);
			if (n <= 0)
				return OPT_ERR_RANGECHECK;

			end = (unsigned long)n;
			if (end < start)
				return OPT_ERR_RANGECHECK;
			parent = queryIntervalTabByRange (start, end);
		}
	}
	else
		return OPT_ERR_TYPECHECK;

	opt_vm_ostack_pop (vm);
	if (parent == CORK_NIL)
		opt_vm_ostack_push (vm, es_false);
	else
	{
		EsObject *parent_obj = es_integer_new (parent);
		opt_vm_ostack_push (vm, parent_obj);
		opt_vm_ostack_push (vm, es_true);
		es_object_unref (parent_obj);
	}
	return false;
}

static EsObject *lrop_anongen (OptVM *vm, EsObject *name)
{
	int n_pop = 2;

	if (opt_vm_ostack_count (vm) < 2)
		return OPT_ERR_UNDERFLOW;

	EsObject *kind_obj = opt_vm_ostack_top (vm);
	if (es_object_get_type (kind_obj) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;
	EsObject *kind_sym = es_pointer_get (kind_obj);
	const char *kind_str = es_symbol_get (kind_sym);

	EsObject *tmp_obj = opt_vm_ostack_peek (vm, 1);
	langType lang = LANG_IGNORE;
	EsObject *prefix_obj = es_nil;
	if (es_object_get_type (tmp_obj) == OPT_TYPE_NAME)
	{
		EsObject *lang_sym = es_pointer_get (tmp_obj);
		const char *lang_str = es_symbol_get (lang_sym);
		lang = getNamedLanguageOrAlias (lang_str, 0);
		if (lang == LANG_IGNORE)
			return OPTSCRIPT_ERR_UNKNOWNLANGUAGE;
	}
	else
	{
		lang = getInputLanguage ();
		prefix_obj = tmp_obj;
	}
	Assert(lang != LANG_IGNORE);
	Assert(lang != LANG_AUTO);

	kindDefinition* kind_def = getLanguageKindForName (lang, kind_str);
	if (!kind_def)
		return OPTSCRIPT_ERR_UNKNOWNKIND;
	int kind_index = kind_def->id;

	if (es_null(prefix_obj))
	{
		n_pop++;
		if (opt_vm_ostack_count (vm) < 3)
			return OPT_ERR_UNDERFLOW;
		prefix_obj = opt_vm_ostack_peek (vm, 2);
	}
	if (es_object_get_type (prefix_obj) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;
	const char *prefix = opt_string_get_cstr (prefix_obj);

	vString *anon_vstr = anonGenerateNewFull (prefix, lang, kind_index);
	EsObject *anon_obj = opt_string_new_from_cstr (vStringValue(anon_vstr));
	vStringDelete(anon_vstr);

	for (; n_pop > 0; n_pop--)
		opt_vm_ostack_pop (vm);

	opt_vm_ostack_push (vm, anon_obj);
	es_object_unref (anon_obj);
	return es_false;
}

static struct optscriptOperatorRegistration lropOperators [] = {
	{
		.name     = "_matchstr",
		.fn       = lrop_get_match_string_group_on_stack,
		.arity    = 1,
		.help_str = "group:int _MATCHSTR string true%"
		"group:int _MATCHSTR false",
	},
	{
		.name     = "_matchloc",
		.fn       = lrop_get_match_loc,
		.arity    = -1,
		.help_str = "group:int /start|/end _MATCHLOC matchloc%"
		"group:int _MATCHLOC matchloc",
	},
	{
		.name     = "_matchloc2line",
		.fn       = ldrop_get_line_from_matchloc,
		.arity    = 1,
		.help_str = "matchloc _MATCHLOC2LINE int:line",
	},
	{
		.name     = "_tagloc",
		.fn       = lrop_get_tag_loc,
		.arity    = 1,
		.help_str = "index:int _TAGLOC matchloc",
	},
	{
		.name     = "_foreignreftag",
		.fn       = lrop_make_foreignreftag,
		.arity    = -1,
		.help_str = "name:str lang:name kind:name role:name|null matchloc _FOREIGNREFTAG tag%"
		"name:str lang:name|null kind:name role:name|null _FOREIGNREFTAG tag%",
	},
	{
		.name     = "_commit",
		.fn       = lrop_commit_tag,
		.arity    = 1,
		.help_str = "tag _COMMIT int",
	},
	{
		.name     = "_scopeset",
		.fn       = lrop_set_scope,
		.arity    = 1,
		.help_str = "int _SCOPESET -",
	},
	{
		.name     = "_scopepop",
		.fn       = lrop_pop_scope,
		.arity    = 0,
		.help_str = "- _SCOPEPOP -",
	},
	{
		.name     = "_scopeclear",
		.fn       = lrop_clear_scope,
		.arity    = 0,
		.help_str = "- _SCOPECLEAR -",
	},
	{
		.name     = "_scopetop",
		.fn       = lrop_ref0_scope,
		.arity    = 0,
		.help_str = "- _SCOPETOP int true%"
		"- _SCOPETOP false",
	},
	{
		.name     = "_scopeNth",
		.fn       = lrop_refN_scope,
		.arity    = 1,
		.help_str = "index:int _SCOPENTH int",
	},
	{
		.name     = "_scopedepth",
		.fn       = lrop_get_scope_depth,
		.arity    = 0,
		.help_str = "- _SCOPEDEPTH int",
	},
	{
		.name     = "_repl",
		.fn       = lrop_repl,
		.arity    = 0,
		.help_str = "- _repl -",
	},
	{
		.name     = "_tenter",
		.fn       = lrop_tenter,
		.arity    = 1,
		.help_str = "table:name _TENTER -",
	},
	{
		.name     = "_tentercont",
		.fn       = lrop_tenter_with_continuation,
		.arity    = 2,
		.help_str = "table:name cont:name _TENTERCONT -",
	},
	{
		.name     = "_tleave",
		.fn       = lrop_tleave,
		.arity    = 0,
		.help_str = "- _TLEAVE -",
	},
	{
		.name     = "_tjump",
		.fn       = lrop_tjump,
		.arity    = 1,
		.help_str = "table:name _TJUMP -",
	},
	{
		.name     = "_treset",
		.fn       = lrop_treset,
		.arity    = 1,
		.help_str = "table:name _TRESET -",
	},
	{
		.name     = "_tquit",
		.fn       = lrop_tquit,
		.arity    = 0,
		.help_str = "- _TQUIT -",
	},
	{
		.name     = "_extraenabled",
		.fn       = lrop_extraenabled,
		.arity    = 1,
		.help_str = "extra:name _extraenabled bool%"
		"lang.extra:name _extraenabled bool",
	},
	{
		.name     = "_markextra",
		.fn       = lrop_markextra,
		.arity    = 2,
		.help_str = "tag:int|tag:tag extra:name _MARKEXTRA -%"
		"tag:int|tag:tag lang.extra:name _MARKEXTRA -",
	},
	{
		.name     = "_advanceto",
		.fn       = lrop_advanceto,
		.arity    = 1,
		.help_str = "matchloc _ADVANCETO -%"
	},
	{
		.name     = "_traced",
		.fn       = lrop_traced,
		.arity    = 0,
		.help_str = "- _TRACED true|false",
	},
	{
		.name     = "_markplaceholder",
		.fn       = lrop_markplaceholder,
		.arity    = 1,
		.help_str = "tag:int _MARKPLACEHOLDER -",
	},
	{
		.name     = "_makepromise",
		.fn       = lrop_makepromise,
		.arity    = 3,
		.help_str = "lang:string start:matchloc end:matchloc _MAKEPROMISE promise:int true%"
		"lang:string start:matchloc end:matchloc _MAKEPROMISE false",
	},
	{
		.name     = "_param",
		.fn       = lrop_param,
		.arity    = 1,
		.help_str = "param:name _PARAM value:string true%"
		"param:name _PARAM false",
	},
	{
		.name     = "_assignrole",
		.fn       = lrop_assign_role,
		.arity    = 2,
		.help_str = "tag:int|tag:tag role:name _ASSIGNROLE -",
	},
	{
		.name     = "_unassignrole",
		.fn       = lrop_unassign_role,
		.arity    = 2,
		.help_str = "tag:int|tag:tag role:name _UNASSIGNROLE -",
	},
	{
		.name     = "_intervaltab",
		.fn       = lrop_intervaltab,
		.arity    = 1,
		.help_str = "tag:int|tag:tag|matchloc|[line:int]|[startline:int endline:int] _INTERVALTAB parent:int true%"
		"tag:int|tag:tag|matchloc|[startline:int endline:int] _INTERVALTAB false",
	},
	{
		.name     = "_anongen",
		.fn       = lrop_anongen,
		.arity    = -1,
		.help_str = "prefix:string kind:name _ANONGEN anon:string%"
		"prefix:string lang:name kind:name _ANONGEN anon:string%",
	},
};

extern bool installOptscriptFieldAccessor (fieldType ftype)
{
	/* This function is called via defineField().
	   defineField() is called via --fielddef option of built-in parsers
	   defining their specific fields.
	   built-in parsers are initialized lazily in the current implementation.
	   optvm is initialized earlier than parsers. */
	Assert (!es_null(lregex_dict));

	optscriptInstallFieldAccessor (lregex_dict, ftype);
	return true;
}

extern void initRegexOptscript (void)
{
	if (!regexAvailable)
		return;

	if (optvm)
		return;

	optvm = optscriptInit ();
	appData *d = xCalloc (1, appData);
	opt_vm_set_app_data (optvm, d);
	lregex_dict = opt_dict_new (17);

	OPTSCRIPT_ERR_UNKNOWNTABLE = es_error_intern ("unknowntable");
	OPTSCRIPT_ERR_NOTMTABLEPTRN = es_error_intern ("notmtableptrn");
	OPTSCRIPT_ERR_UNKNOWNEXTRA = es_error_intern ("unknownextra");
	OPTSCRIPT_ERR_UNKNOWNLANGUAGE = es_error_intern ("unknownlanguage");
	OPTSCRIPT_ERR_UNKNOWNKIND = es_error_intern ("unknownkind");
	OPTSCRIPT_ERR_UNKNOWNROLE = es_error_intern ("unknownrole");
	OPTSCRIPT_ERR_FIELDRESET = es_error_intern ("fieldreset");

	optscriptInstallProcs (lregex_dict, lrop_get_match_string_named_group);

	optscriptRegisterOperators (lregex_dict,
								lropOperators, ARRAY_SIZE(lropOperators));

	extern const char ctagsCommonPrelude[];
	opt_vm_dstack_push (optvm, lregex_dict);
	MIO *mio = mio_new_memory ((unsigned char*)ctagsCommonPrelude, strlen (ctagsCommonPrelude), NULL, NULL);
	EsObject *e = optscriptLoad (optvm, mio);
	if (es_error_p (e))
		error (FATAL, "failed in loading built-in procedures");
	mio_unref (mio);
	opt_vm_dstack_pop (optvm);
}

extern void	listRegexOpscriptOperators (FILE *fp)
{
	EsObject *procdocs;
	if (!opt_dict_known_and_get_cstr (lregex_dict,
									  "__procdocs",
									  &procdocs))
		procdocs = NULL;

	opt_vm_dstack_push (optvm, lregex_dict);
	optscriptHelp (optvm, fp, procdocs);
	opt_vm_dstack_pop (optvm);
}
