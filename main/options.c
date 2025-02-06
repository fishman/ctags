/*
*   Copyright (c) 1996-2003, Darren Hiebert
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions to process command line options.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#define OPTION_WRITE
#include "options_p.h"

#ifndef _GNU_SOURCE
# define _GNU_SOURCE   /* for asprintf */
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>  /* to declare isspace () */

#include "ctags.h"
#include "debug.h"
#include "entry_p.h"
#include "field_p.h"
#include "gvars.h"
#include "keyword_p.h"
#include "parse_p.h"
#include "ptag_p.h"
#include "routines_p.h"
#include "xtag_p.h"
#include "param_p.h"
#include "param.h"
#include "error_p.h"
#include "interactive_p.h"
#include "writer_p.h"
#include "trace.h"

#ifdef HAVE_JANSSON
#include <jansson.h>
#endif

/*
*   MACROS
*/
#define INVOCATION  "Usage: %s [options] [file(s)]\n"

#define CTAGS_ENVIRONMENT  "CTAGS"
#define ETAGS_ENVIRONMENT  "ETAGS"

#ifndef ETAGS
# define ETAGS	"etags"  /* name which causes default use of to -e */
#endif

/*  The following separators are permitted for list options.
 */
#define EXTENSION_SEPARATOR '.'
#define PATTERN_START '('
#define PATTERN_STOP  ')'
#define IGNORE_SEPARATORS   ", \t\n"

#ifndef DEFAULT_FILE_FORMAT
# define DEFAULT_FILE_FORMAT  2
#endif

#if defined (HAVE_OPENDIR) || defined (HAVE__FINDFIRST)
# define RECURSE_SUPPORTED
#endif

#define isCompoundOption(c)  (bool) (strchr ("fohiILpdDb", (c)) != NULL)

#define ENTER(STAGE) do {												\
		Assert (Stage <= OptionLoadingStage##STAGE);					\
		if (Stage != OptionLoadingStage##STAGE)							\
		{																\
			Stage = OptionLoadingStage##STAGE;							\
			verbose ("Entering configuration stage: loading %s\n", StageDescription[Stage]); \
		}																\
	} while (0)

/*
*   Data declarations
*/

enum eOptionLimits {
	MaxHeaderExtensions	= 100,  /* maximum number of extensions in -h option */
	MaxSupportedTagFormat = 2
};

typedef struct sOptionDescription {
	int usedByEtags;
	int experimentalOption;
	const char *description;
} optionDescription;

typedef void (*parametricOptionHandler) (const char *const option, const char *const parameter);

typedef const struct {
	const char* name;   /* name of option as specified by user */
	parametricOptionHandler handler;  /* routine to handle option */
	bool initOnly;   /* option must be specified before any files */
	unsigned long acceptableStages;
} parametricOption;

typedef const struct sBooleanOption {
	const char* name;   /* name of option as specified by user */
	bool* pValue;    /* pointer to option value */
	bool initOnly;   /* option must be specified before any files */
	unsigned long acceptableStages;
	void (* set) (const struct sBooleanOption *const option, bool value);
} booleanOption;

/*
*   DATA DEFINITIONS
*/

static bool NonOptionEncountered = false;
static stringList *OptionFiles;

typedef stringList searchPathList;
static searchPathList *OptlibPathList;

static stringList *Excluded, *ExcludedException;
static bool FilesRequired = true;
static bool SkipConfiguration;

static const char *const HeaderExtensions [] = {
	"h", "H", "hh", "hpp", "hxx", "h++", "inc", "def", NULL
};

long ctags_debugLevel = 0L;
bool ctags_verbose = false;

optionValues Option = {
	.append = false,
	.backward = false,
	.etags = false,
	.locate =
#ifdef MACROS_USE_PATTERNS
	EX_PATTERN
#else
	EX_MIX
#endif
	,
	.recurse = false,
	.sorted = SO_SORTED,
	.xref = false,
	.customXfmt = NULL,
	.fileList = NULL,
	.tagFileName = NULL,
	.headerExt = NULL,
	.etagsInclude = NULL,
	.tagFileFormat = DEFAULT_FILE_FORMAT,
#ifdef HAVE_ICONV
	.inputEncoding= NULL,
	.outputEncoding = NULL,
#endif
	.language = LANG_AUTO,
	.followLinks = true,
	.filter = false,
	.filterTerminator = NULL,
	.tagRelative = TREL_NO,
	.printTotals = 0,
	.lineDirectives = false,
	.printLanguage =false,
	.guessLanguageEagerly = false,
	.quiet = false,
	.fatalWarnings = false,
	.patternLengthLimit = 96,
	.putFieldPrefix = false,
	.maxRecursionDepth = 0xffffffff,
	.interactive = false,
	.fieldsReset = false,
#ifdef _WIN32
	.useSlashAsFilenameSeparator = FILENAME_SEP_UNSET,
#endif
#ifdef DEBUG
	.breakLine = 0,
#endif
};

struct localOptionValues {
	bool machinable;			/* --machinable */
	bool withListHeader;		/* --with-list-header */
} localOption = {
	.machinable = false,
	.withListHeader = true,
};

typedef enum eOptionLoadingStage {
	OptionLoadingStageNone,
	OptionLoadingStageCustom,
	OptionLoadingStageXdg,
	OptionLoadingStageHomeDir,
	OptionLoadingStageCurrentDir,
	OptionLoadingStageEnvVar,
	OptionLoadingStageCmdline,
} OptionLoadingStage;

static OptionLoadingStage Stage = OptionLoadingStageNone;
#define STAGE_ANY ~0UL

/*
-   Locally used only
*/

static optionDescription LongOptionDescription [] = {
 {1,0,"Input/Output Options"},
 {1,0,"  --exclude=<pattern>"},
 {1,0,"       Exclude files and directories matching <pattern>."},
 {1,0,"       See also --exclude-exception option."},
 {1,0,"  --exclude-exception=<pattern>"},
 {1,0,"      Don't exclude files and directories matching <pattern> even if"},
 {1,0,"      they match the pattern specified with --exclude option."},
 {1,0,"  --filter[=(yes|no)]"},
 {1,0,"       Behave as a filter, reading file names from standard input and"},
 {1,0,"       writing tags to standard output [no]."},
 {1,0,"  --filter-terminator=<string>"},
 {1,0,"       Specify <string> to print to stdout following the tags for each file"},
 {1,0,"       parsed when --filter is enabled."},
 {1,0,"  --links[=(yes|no)]"},
 {1,0,"       Indicate whether symbolic links should be followed [yes]."},
 {1,0,"  --maxdepth=<N>"},
#ifdef RECURSE_SUPPORTED
 {1,0,"       Specify maximum recursion depth."},
#else
 {1,0,"       Not supported on this platform."},
#endif
 {1,0,"  --recurse[=(yes|no)]"},
#ifdef RECURSE_SUPPORTED
 {1,0,"       Recurse into directories supplied on command line [no]."},
 {1,0,"  -R   Equivalent to --recurse."},
#else
 {1,0,"       Not supported on this platform."},
 {1,0,"  -R   Not supported on this platform."},
#endif
 {1,0,"  -L <file>"},
 {1,0,"       A list of input file names is read from the specified <file>."},
 {1,0,"       If specified as \"-\", then standard input is read."},
 {1,0,"  --append[=(yes|no)]"},
 {1,0,"       Should tags should be appended to existing tag file [no]?"},
 {1,0,"  -a   Append the tags to an existing tag file."},
 {1,0,"  -f <tagfile>"},
 {1,0,"       Write tags to specified <tagfile>. Value of \"-\" writes tags to stdout"},
 {1,0,"       [\"tags\"; or \"TAGS\" when -e supplied]."},
 {1,0,"  -o   Alternative for -f."},
 {1,0,""},
 {1,0,"Output Format Options"},
 {0,0,"  --format=(1|2)"},
#if DEFAULT_FILE_FORMAT == 1
 {0,0,"       Force output of specified tag file format [1]."},
#else
 {0,0,"       Force output of specified tag file format [2]."},
#endif
#ifdef HAVE_JANSSON
 {0,0,"  --output-format=(u-ctags|e-ctags|etags|xref|json)"},
#else
 {0,0,"  --output-format=(u-ctags|e-ctags|etags|xref)"},
#endif
 {0,0,"      Specify the output format. [u-ctags]"},
 {0,0,"  -e   Output tag file for use with Emacs."},
 {1,0,"  -x   Print a tabular cross reference file to standard output."},
 {0,0,"  --sort=(yes|no|foldcase)"},
 {0,0,"       Should tags be sorted (optionally ignoring case) [yes]?"},
 {0,0,"  -u   Equivalent to --sort=no."},
 {1,0,"  --etags-include=<file>"},
 {1,0,"       Include reference to <file> in Emacs-style tag file (requires -e)."},
#ifdef HAVE_ICONV
 {1,0,"  --input-encoding=<encoding>"},
 {1,0,"       Specify <encoding> of all input files."},
 {1,0,"  --input-encoding-<LANG>=<encoding>"},
 {1,0,"       Specify <encoding> of the <LANG> input files."},
 {1,0,"  --output-encoding=<encoding>"},
 {1,0,"       The <encoding> to write the tag file in. Defaults to UTF-8 if --input-encoding"},
 {1,0,"       is specified, otherwise no conversion is performed."},
#endif
 {1,1,"  --_xformat=<field_format>"},
 {1,1,"       Specify custom format for tabular cross reference (-x)."},
 {1,1,"       Fields can be specified with letter listed in --list-fields. [%-16N %-10K %4n %-16F %C]"},
 {1,1,"       e.g. --_xformat=%10N %10l:%K @ %-20F:%-20n"},
 {1,0,""},
 {1,0,"Language Selection and Mapping Options"},
 {1,0,"  --language-force=(<language>|auto)"},
 {1,0,"       Force all files to be interpreted using specified <language>."},
 {1,0,"  --languages=[+|-](<list>|all)"},
 {1,0,"       Restrict files scanned for tags to those mapped to languages"},
 {1,0,"       specified in the comma-separated <list>. The list can contain any"},
 {1,0,"       built-in or user-defined language [all]."},
 {1,0,"  --alias-<LANG>=[+|-](<pattern>|default)"},
 {1,0,"       Add a <pattern> detecting a name, can be used as an alternative name"},
 {1,0,"       for <LANG>."},
 {1,0,"  --guess-language-eagerly"},
 {1,0,"       Guess the language of input file more eagerly"},
 {1,0,"       (but taking longer time for guessing):"},
 {1,0,"       o shebang, even if the input file is not executable,"},
 {1,0,"       o emacs mode specification at the beginning and end of input file, and"},
 {1,0,"       o vim syntax specification at the end of input file."},
 {1,0,"  -G   Equivalent to --guess-language-eagerly."},
 {1,0,"  --langmap=<map>[,<map>[...]]"},
 {1,0,"       Override default mapping of language to input file extension."},
 {1,0,"       e.g. --langmap=c:.c.x,java:+.j,make:([Mm]akefile).mak"},
 {1,0,"  --map-<LANG>=[+|-]<extension>|<pattern>"},
 {1,0,"       Set, add(+) or remove(-) the map for <LANG>."},
 {1,0,"       Unlike --langmap, this doesn't take a list; only one file name <pattern>"},
 {1,0,"       or one file <extension> can be specified at once."},
 {1,0,"       Unlike --langmap the change with this option affects mapping of <LANG> only."},
 {1,0,""},
 {1,0,"Tags File Contents Options"},
 {0,0,"  --excmd=(number|pattern|mix|combine)"},
#ifdef MACROS_USE_PATTERNS
 {0,0,"       Uses the specified type of EX command to locate tags [pattern]."},
#else
 {0,0,"       Uses the specified type of EX command to locate tags [mix]."},
#endif
 {0,0,"  -n   Equivalent to --excmd=number."},
 {0,0,"  -N   Equivalent to --excmd=pattern."},
 {1,0,"  --extras=[+|-][<flags>|*]"},
 {1,0,"       Include extra tag entries for selected information (<flags>: \"fFgpqrs\") [F]."},
 {1,0,"  --extras-(<LANG>|all)=[+|-][<flags>|*]"},
 {1,0,"       Include <LANG> own extra tag entries for selected information"},
 {1,0,"       (<flags>: see the output of --list-extras=<LANG> option)."},
 {1,0,"  --fields=[+|-][<flags>|*]"},
 {1,0,"       Include selected extension fields (<flags>: \"aCeEfFikKlmnNpPrRsStxzZ\") [fks]."},
 {1,0,"  --fields-(<LANG>|all)=[+|-][<flags>|*]"},
 {1,0,"       Include selected <LANG> own extension fields"},
 {1,0,"       (<flags>: see the output of --list-fields=<LANG> option)."},
 {1,0,"  --kinds-(<LANG>|all)=[+|-](<kinds>|*)"},
 {1,0,"       Enable/disable tag <kinds> for language <LANG>."},
 {0,0,"  --pattern-length-limit=<N>"},
 {0,0,"      Cutoff patterns of tag entries after <N> characters. Disable by setting to 0. [96]"},
 {0,0,"  --pseudo-tags=[+|-](<pseudo-tag>|*)"},
 {0,0,"       Enable/disable emitting pseudo tag named <pseudo-tag>."},
 {0,0,"       if '*' is given, enable emitting all pseudo tags."},
 {0,0,"  --put-field-prefix"},
 {0,0,"       Put \"" CTAGS_FIELD_PREFIX "\" as prefix for the name of fields newly introduced in"},
 {0,0,"       universal-ctags."},
 {1,0,"  --roles-(<LANG>|all).(<kind>|*)=[+|-][<roles>|*]"},
 {1,0,"       Enable/disable tag roles for kinds of language <LANG>."},
 {0,0,"  --tag-relative=(yes|no|always|never)"},
 {0,0,"       Should paths be relative to location of tag file [no; yes when -e]?"},
 {0,0,"       always: be relative even if input files are passed in with absolute paths" },
 {0,0,"       never:  be absolute even if input files are passed in with relative paths" },
#ifdef _WIN32
 {1,0,"  --use-slash-as-filename-separator[=(yes|no)]"},
 {1,0,"       Use slash as filename separator [yes] for u-ctags output format."},
#endif
 {0,0,"  -B   Use backward searching patterns (?...?)."},
 {0,0,"  -F   Use forward searching patterns (/.../; default)."},
 {1,0,""},
 {1,0,"Option File Options"},
 {1,0,"  --options=<pathname>"},
 {1,0,"       Specify file (or directory) <pathname> from which command line options should be read."},
 {1,0,"  --options-maybe=<pathname>"},
 {1,0,"       Do the same as --options but this doesn't make an error for non-existing file."},
 {1,0,"  --optlib-dir=[+]<directory>"},
 {1,0,"       Add or set <directory> to optlib search path."},
 {1,1,"  --_echo=<msg>"},
 {1,1,"       Echo <msg> to standard error. Useful to debug the chain"},
 {1,1,"       of loading option files."},
 {1,1,"  --_force-quit[=<num>]"},
 {1,1,"       Quit when loading the option files is processed."},
 {1,1,"       Useful to debug the chain of loading option files."},
 {1,0,""},
 {1,0,"optlib Options"},
 {1,0,"  --kinddef-<LANG>=<letter>,<name>,<description>"},
 {1,0,"       Define new kind for <LANG>."},
 {1,0,"  --langdef=<name>"},
 {1,0,"       Define a new language to be parsed with regular expressions."},
 {1,0,"  --mline-regex-<LANG>=/<line_pattern>/<name_pattern>/<kind-spec>/[<flags>]"},
 {1,0,"       Define multiline regular expression for locating tags in specific language."},
 {1,0,"  --regex-<LANG>=/<line_pattern>/<name_pattern>/<kind-spec>/[<flags>]"},
 {1,0,"       Define single-line regular expression for locating tags in specific language."},
 {1,1,"  --_extradef-<LANG>=<name>,<description>"},
 {1,1,"       Define new extra for <LANG>. --extras-<LANG>=+{name} enables it."},
 {1,1,"  --_fielddef-<LANG>=<name>,<description>"},
 {1,1,"       Define new field for <LANG>."},
 {1,1,"  --_makeTagEntryReflection-<LANG>={{ optscript-code }}"},
 {1,1,"       Specify code run when <LANG> parser makes a tag."},
 {1,1,"  --_mtable-extend-<LANG>=disttable+srctable."},
 {1,1,"       Copy patterns of a regex table to another regex table."},
 {1,1,"  --_mtable-regex-<LANG>=<table>/<line_pattern>/<name_pattern>/[<flags>]"},
 {1,1,"       Define multitable regular expression for locating tags in specific language."},
 {1,1,"  --_paramdef-<LANG>=<name>,<description>"},
 {1,1,"       Define new param for <LANG>."},
 {1,1,"  --_prelude-<LANG>={{ optscript-code }}"},
 {1,1,"       Specify code run before parsing with <LANG> parser."},
 {1,1,"  --_pretend-<NEWLANG>=<OLDLANG>"},
 {1,1,"       Make NEWLANG parser pretend OLDLANG parser in lang: field."},
 {1,1,"  --_roledef-<LANG>.<kind>=<name>,<description>"},
 {1,1,"       Define new role for the kind in <LANG>."},
 {1,1,"  --_scopesep-<LANG>=[<parent_kind_letter>|*]/(<child_kind_letter>|*):<separator>"},
 {1,1,"       Specify scope separator between <PARENT_KIND> and <KIND>."},
 {1,1,"  --_sequel-<LANG>={{ optscript-code }}"},
 {1,1,"       Specify code run after parsing with <LANG> parser."},
 {1,1,"  --_tabledef-<LANG>=<name>"},
 {1,1,"       Define new regex table for <LANG>."},
 {1,0,""},
 {1,0,"Language Specific Options"},
 {1,0,"  --if0[=(yes|no)]"},
 {1,0,"       Should code within #if 0 conditional branches be parsed [no]?"},
 {0,0,"  --line-directives[=(yes|no)]"},
 {0,0,"       Should '#line' directives be processed [no]?"},
 {1,0,"  -D <macro>=<definition>"},
 {1,0,"       (CPreProcessor) Give <definition> for <macro>."},
 {1,0,"  -h (<list>|default)"},
 {1,0,"       Specify a <list> of file extensions to be treated as include files"},
 {1,0,"       [\".h.H.hh.hpp.hxx.h++.inc.def\"]."},
 {1,0,"  -I [+|-]<list>|@<file>"},
 {1,0,"       A <list> of tokens to be specially handled is read from either the"},
 {1,0,"       command line or the specified <file>."},
 {1,0,"  --param-<LANG>.<name>=<argument>"},
 {1,0,"       Set <LANG> specific parameter. Available parameters can be listed with --list-params."},
 {1,0,""},
 {1,0,"Listing Options"},
 {1,0,"  --list-aliases[=(<language>|all)]"},
 {1,0,"       Output list of alias patterns."},
 {1,0,"  --list-excludes"},
 {1,0,"       Output list of exclude patterns for excluding files/directories."},
 {1,0,"  --list-extras[=(<language>|all)]"},
 {1,0,"       Output list of extra tag flags."},
 {1,0,"  --list-features"},
 {1,0,"       Output list of compiled features."},
 {1,0,"  --list-fields[=(<language>|all)]"},
 {1,0,"       Output list of fields."},
 {1,0,"  --list-kinds[=(<language>|all)]"},
 {1,0,"       Output a list of all tag kinds for specified <language> or all."},
 {1,0,"  --list-kinds-full[=(<language>|all)]"},
 {1,0,"       List the details of all tag kinds for specified <language> or all"},
 {1,0,"       For each line, associated language name is printed when \"all\" is"},
 {1,0,"       specified as language."},
 {1,0,"  --list-languages"},
 {1,0,"       Output list of supported languages."},
 {1,0,"  --list-map-extensions[=(<language>|all)]"},
 {1,0,"       Output list of language extensions in mapping."},
 {1,0,"  --list-map-patterns[=(<language>|all)]"},
 {1,0,"       Output list of language patterns in mapping."},
 {1,0,"  --list-maps[=(<language>|all)]"},
 {1,0,"       Output list of language mappings (both extensions and patterns)."},
 {1,0,"  --list-mline-regex-flags"},
 {1,0,"       Output list of flags which can be used in a multiline regex parser definition."},
 {1,0,"  --list-output-formats"},
 {1,0,"       Output list of output formats."},
 {1,0,"  --list-params[=(<language>|all)]"},
 {1,0,"       Output list of language parameters. This works with --machinable."},
 {0,0,"  --list-pseudo-tags"},
 {0,0,"       Output list of pseudo tags."},
 {1,0,"  --list-regex-flags"},
 {1,0,"       Output list of flags which can be used in a regex parser definition."},
 {1,0,"  --list-roles[=(<language>|all)[.(<kindspecs>|*)]]"},
 {1,0,"       Output list of all roles of tag kind(s) specified for <language>."},
 {1,0,"       Both letters and names can be used in <kindspecs>."},
 {1,0,"       e.g. --list-roles=C.{header}d"},
 {1,0,"  --list-subparsers[=(<baselang>|all)]"},
 {1,0,"       Output list of subparsers for the base language."},
 {1,0,"  --machinable[=(yes|no)]"},
 {1,0,"       Use tab separated representation in --list-* option output. [no]"},
 {1,0,"       --list-{aliases,extras,features,fields,kind-full,langdef-flags,params," },
 {1,0,"       pseudo-tags,regex-flags,roles,subparsers} support this option."},
 {1,0,"       Suitable for scripting. Specify before --list-* option."},
 {1,0,"  --with-list-header[=(yes|no)]"},
 {1,0,"       Prepend the column descriptions in --list- output. [yes]"},
 {1,0,"       --list-{aliases,extras,features,fields,kind-full,langdef-flags,params," },
 {1,0,"       pseudo-tags,regex-flags,roles,subparsers} support this option."},
 {1,0,"       Specify before --list-* option."},
 {1,1,"  --_list-fielddef-flags"},
 {1,1,"       Output list of flags which can be used with --fielddef option."},
 {1,1,"  --_list-kinddef-flags"},
 {1,1,"       Output list of flags which can be used with --kinddef option."},
 {1,1,"  --_list-langdef-flags"},
 {1,1,"       Output list of flags which can be used with --langdef option."},
 {1,1,"  --_list-mtable-regex-flags"},
 {1,1,"       Output list of flags which can be used in a multitable regex parser definition."},
 {1,1,"  --_list-operators"},
 {1,1,"       Output list of optscript operators."},
 {1,0,""},
 {1,0,"Miscellaneous Options"},
 {1,0,"  --help"},
 {1,0,"       Print this option summary."},
 {1,0,"  -?   Print this option summary."},
 {1,0,"  --help-full"},
 {1,0,"       Print this option summary including experimental features."},
 {1,0,"  --license"},
 {1,0,"       Print details of software license."},
 {0,0,"  --print-language"},
 {0,0,"       Don't make tags file but just print the guessed language name for"},
 {0,0,"       input file."},
 {1,0,"  --quiet[=(yes|no)]"},
 {0,0,"       Don't print NOTICE class messages [no]."},
 {1,0,"  --totals[=(yes|no|extra)]"},
 {1,0,"       Print statistics about input and tag files [no]."},
 {1,0,"  --verbose[=(yes|no)]"},
 {1,0,"       Enable verbose messages describing actions on each input file."},
 {1,0,"  --version[=<language>]"},
 {1,0,"       Print version identifier of the program to standard output."},
 {1,0,"       Print version identifier of the parser for <language>."},
 {1,0,"  -V   Equivalent to --verbose."},
#ifdef DEBUG
 {1,0,"  -b <line>"},
 {1,0,"       Set break line. (for DEBUG)"},
 {1,0,"  -d <level>"},
 {1,0,"       Set debug level. (for DEBUG)"},
#endif

 {1,1,"  --_anonhash=<fname>"},
 {1,1,"       Used in u-ctags test harness"},
 {1,1,"  --_dump-keywords"},
 {1,1,"       Dump keywords of initialized parser(s)."},
 {1,1,"  --_dump-options"},
 {1,1,"       Dump options."},
 {1,1,"  --_dump-prelude"},
 {1,1,"       Dump contents of optscript prelude."},
 {1,1,"  --_fatal-warnings"},
 {1,1,"       Make all warnings fatal."},
 {1,1,"  --_force-initializing"},
 {1,1,"       Initialize all parsers in early stage"},
#ifdef HAVE_JANSSON
 {0,1,"  --_interactive"
#ifdef HAVE_SECCOMP
  "[=(default|sandbox)]"
#endif
 },
 {0,1,"       Enter interactive mode (JSON over stdio)."},
#ifdef HAVE_SECCOMP
 {0,1,"       Enter file I/O limited interactive mode if sandbox is specified. [default]"},
#endif
#endif
#ifdef DO_TRACING
 {1,1,"  --_trace=<list>"},
 {1,1,"       Trace parsers for the languages."},
#endif
 {1,1, NULL}
};

static const char* const License1 =
"This program is free software; you can redistribute it and/or\n"
"modify it under the terms of the GNU General Public License\n"
"as published by the Free Software Foundation; either version 2"
"of the License, or (at your option) any later version.\n"
"\n";
static const char* const License2 =
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
"GNU General Public License for more details.\n"
"\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program; if not, write to the Free Software\n"
"Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.\n";

/*  Contains a set of strings describing the set of "features" compiled into
 *  the code.
 */
static struct Feature {
	const char *name;
	const char *description;
} Features [] = {
#ifdef _WIN32
	{"win32", "TO BE WRITTEN"},
#endif
	/* Following two features are always available on universal ctags */
	{"wildcards", "can use glob matching"},
	{"regex", "can use regular expression based pattern matching"},
#ifdef USE_GNULIB_FNMATCH
	{"gnulib_fnmatch", "linked with the Gnulib fnmatch library"},
#endif
/* https://lists.gnu.org/archive/html/bug-gnulib/2011-07/msg00435.html */
#ifdef _REGEX_INCLUDE_LIMITS_H
	{"gnulib_regex", "linked with the Gnulib regular expression library"},
#endif
#ifndef EXTERNAL_SORT
	{"internal-sort", "uses internal sort routine instead of invoking sort command"},
#endif
#ifdef CUSTOM_CONFIGURATION_FILE
	{"custom-conf", "read \"" CUSTOM_CONFIGURATION_FILE "\" as config file"},
#endif
#if defined (_WIN32)
	{"unix-path-separator", "can use '/' as file name separator"},
#endif
#ifdef HAVE_ICONV
	{"iconv", "can convert input/output encodings"},
#endif
#ifdef DEBUG
	{"debug", "TO BE WRITTEN"},
#endif
#if defined (HAVE_DIRENT_H) || defined (_MSC_VER)
	{"option-directory", "TO BE WRITTEN"},
#endif
#ifdef HAVE_LIBXML
	{"xpath", "linked with library for parsing xml input"},
#endif
#ifdef HAVE_JANSSON
	{"json", "supports json format output"},
	{"interactive", "accepts source code from stdin"},
#endif
#ifdef HAVE_SECCOMP
	{"sandbox", "linked with code for system call level sandbox"},
#endif
#ifdef HAVE_LIBYAML
	{"yaml", "linked with library for parsing yaml input"},
#endif
#ifdef CASE_INSENSITIVE_FILENAMES
	{"case-insensitive-filenames", "TO BE WRITTEN"},
#endif
#ifdef ENABLE_GCOV
	{"gcov", "linked with code for coverage analysis"},
#endif
#ifdef HAVE_PACKCC
	/* The test harnesses use this as hints for skipping test cases */
	{"packcc", "has peg based parser(s)"},
#ifdef HAVE_PEGOF
	{"pegof", "makes peg based parser(s) optimized (experimental)"},
#endif
#endif
	{"optscript", "can use the interpreter"},
#ifdef HAVE_PCRE2
	{"pcre2", "has pcre2 regex engine"},
#endif
	{NULL,}
};

static const char *const StageDescription [] = {
	[OptionLoadingStageNone]   = "not initialized",
	[OptionLoadingStageCustom] = "custom file",
	[OptionLoadingStageXdg] = "file(s) under $XDG_CONFIG_HOME and $HOME/.config",
	[OptionLoadingStageHomeDir] = "file(s) under $HOME",
	[OptionLoadingStageCurrentDir] = "file(s) under the current directory",
	[OptionLoadingStageCmdline] = "command line",
};

/*
*   FUNCTION PROTOTYPES
*/
static bool parseFileOptions (const char *const fileName);
static bool parseAllConfigurationFilesOptionsInDirectory (const char *const fileName,
							     stringList* const already_loaded_files);
static bool getBooleanOption (const char *const option, const char *const parameter);

/*
*   FUNCTION DEFINITIONS
*/

#ifndef HAVE_ASPRINTF

/* Some versions of MinGW are missing _vscprintf's declaration, although they
 * still provide the symbol in the import library.
 */
#ifdef __MINGW32__
_CRTIMP int _vscprintf(const char *format, va_list argptr);
#endif

#ifndef va_copy
#define va_copy(dest, src) (dest = src)
#endif

int asprintf(char **strp, const char *fmt, ...)
{
	va_list args;
	va_list args_copy;
	int length;
	size_t size;

	va_start(args, fmt);

	va_copy(args_copy, args);

#ifdef _WIN32
	/* We need to use _vscprintf to calculate the length as vsnprintf returns -1
	 * if the number of characters to write is greater than count.
	 */
	length = _vscprintf(fmt, args_copy);
#else
	char dummy;
	length = vsnprintf(&dummy, sizeof dummy, fmt, args_copy);
#endif

	va_end(args_copy);

	Assert(length >= 0);
	size = length + 1;

	*strp = malloc(size);
	if (!*strp) {
		return -1;
	}

	va_start(args, fmt);
	vsnprintf(*strp, size, fmt, args);
	va_end(args);

	return length;
}
#endif

extern void verbose (const char *const format, ...)
{
	if (ctags_verbose)
	{
		va_list ap;
		va_start (ap, format);
		vfprintf (stderr, format, ap);
		va_end (ap);
	}
}

static char *stringCopy (const char *const string)
{
	char* result = NULL;
	if (string != NULL)
		result = eStrdup (string);
	return result;
}

static void freeString (char **const pString)
{
	if (*pString != NULL)
	{
		eFree (*pString);
		*pString = NULL;
	}
}

extern void freeList (stringList** const pList)
{
	if (*pList != NULL)
	{
		stringListDelete (*pList);
		*pList = NULL;
	}
}

extern void setDefaultTagFileName (void)
{
	if (Option.filter || Option.interactive)
		return;

	if (Option.tagFileName == NULL)
	{
		const char *tmp = outputDefaultFileName ();

		if (tmp == NULL)
			tmp = "-";

		Option.tagFileName = stringCopy (tmp);
	}
}

extern bool filesRequired (void)
{
	bool result = FilesRequired;
	if (Option.recurse)
		result = false;
	return result;
}

extern void checkOptions (void)
{
	const char* notice;
	if (Option.xref && (Option.customXfmt == NULL))
	{
		notice = "xref output";
		if (isXtagEnabled(XTAG_FILE_NAMES))
		{
			error (WARNING, "%s disables file name tags", notice);
			enableXtag (XTAG_FILE_NAMES, false);
		}
	}
	if (Option.append)
	{
		notice = "append mode is not compatible with";
		if (isDestinationStdout ())
			error (FATAL, "%s tags to stdout", notice);
	}
	if (Option.filter)
	{
		notice = "filter mode";
		if (Option.printTotals)
		{
			error (WARNING, "%s disables totals", notice);
			Option.printTotals = 0;
		}
		if (Option.tagFileName != NULL)
			error (WARNING, "%s ignores output tag file name", notice);
	}
	writerCheckOptions (Option.fieldsReset);
}

extern langType getLanguageComponentInOptionFull (const char *const option,
												  const char *const prefix,
												  bool noPretending)
{
	size_t prefix_len;
	langType language;
	const char *lang;
	char *sep = NULL;
	size_t lang_len = 0;

	Assert (prefix && prefix[0]);
	Assert (option);

	prefix_len = strlen (prefix);
	if (strncmp (option, prefix, prefix_len) != 0)
		return LANG_IGNORE;
	else
	{
		lang = option + prefix_len;
		if (lang [0] == '\0')
			return LANG_IGNORE;
	}

	/* Extract <LANG> from
	 * --param-<LANG>.<PARAM>=..., and
	 * --_roledef-<LANG>.<KIND>=... */

	/*  `:' is only for keeping self compatibility. */
	sep = strpbrk (lang, ":.");
	if (sep)
	{
		if (*sep == ':')
			error (WARNING, "using `:' as a separator is obsolete; use `.' instead: --%s", option);
		lang_len = sep - lang;
	}
	language = getNamedLanguageFull (lang, lang_len, noPretending, false);
	if (language == LANG_IGNORE)
	{
		const char *langName = (lang_len == 0)? lang: eStrndup (lang, lang_len);
		error (FATAL, "Unknown language \"%s\" in \"%s\" option", langName, option);
	}

	return language;
}

extern langType getLanguageComponentInOption (const char *const option,
											  const char *const prefix)
{
	return getLanguageComponentInOptionFull (option, prefix, false);
}

static void setEtagsMode (void)
{
	Option.etags = true;
	Option.sorted = SO_UNSORTED;
	Option.lineDirectives = false;
	Option.tagRelative = TREL_YES;
	enableLanguage (LANG_FALLBACK, true);
	setTagWriter (WRITER_ETAGS, NULL);
}

extern void testEtagsInvocation (void)
{
	char* const execName = eStrdup (getExecutableName ());
	char* const etags = eStrdup (ETAGS);
#ifdef CASE_INSENSITIVE_FILENAMES
	toLowerString (execName);
	toLowerString (etags);
#endif
	if (strstr (execName, etags) != NULL)
	{
		verbose ("Running in etags mode\n");
		setEtagsMode ();
	}
	eFree (execName);
	eFree (etags);
}

static void setXrefMode (void)
{
	Option.xref = true;
	setTagWriter (WRITER_XREF, NULL);
}

#ifdef HAVE_JANSSON
static void setJsonMode (void)
{
	enablePtag (PTAG_JSON_OUTPUT_VERSION, true);
	enablePtag (PTAG_OUTPUT_MODE, false);
	enablePtag (PTAG_FILE_FORMAT, false);
	setTagWriter (WRITER_JSON, NULL);
}
#endif

/*
 *  Cooked argument parsing
 */

static void parseShortOption (cookedArgs *const args)
{
	args->simple [0] = *args->shortOptions++;
	args->simple [1] = '\0';
	args->item = eStrdup (args->simple);
	if (! isCompoundOption (*args->simple))
		args->parameter = "";
	else if (*args->shortOptions == '\0')
	{
		argForth (args->args);
		if (argOff (args->args))
			args->parameter = NULL;
		else
			args->parameter = argItem (args->args);
		args->shortOptions = NULL;
	}
	else
	{
		args->parameter = args->shortOptions;
		args->shortOptions = NULL;
	}
}

static void parseLongOption (cookedArgs *const args, const char *item)
{
	const char* const equal = strchr (item, '=');
	if (equal == NULL)
	{
		args->item = eStrdup (item);
		args->parameter = "";
	}
	else
	{
		args->item = eStrndup (item, equal - item);
		args->parameter = equal + 1;
	}
	Assert (args->item != NULL);
	Assert (args->parameter != NULL);
}

static void cArgRead (cookedArgs *const current)
{
	char* item;

	Assert (current != NULL);
	if (! argOff (current->args))
	{
		item = argItem (current->args);
		current->shortOptions = NULL;
		Assert (item != NULL);
		if (strncmp (item, "--", (size_t) 2) == 0)
		{
			current->isOption = true;
			current->longOption = true;
			parseLongOption (current, item + 2);
			Assert (current->item != NULL);
			Assert (current->parameter != NULL);
		}
		else if (*item == '-')
		{
			current->isOption = true;
			current->longOption = false;
			current->shortOptions = item + 1;
			parseShortOption (current);
		}
		else
		{
			current->isOption = false;
			current->longOption = false;
			current->item = eStrdup (item);
			current->parameter = NULL;
		}
	}
}

extern cookedArgs* cArgNewFromString (const char* string)
{
	cookedArgs* const result = xMalloc (1, cookedArgs);
	memset (result, 0, sizeof (cookedArgs));
	result->args = argNewFromString (string);
	cArgRead (result);
	return result;
}

extern cookedArgs* cArgNewFromArgv (char* const* const argv)
{
	cookedArgs* const result = xMalloc (1, cookedArgs);
	memset (result, 0, sizeof (cookedArgs));
	result->args = argNewFromArgv (argv);
	cArgRead (result);
	return result;
}

extern cookedArgs* cArgNewFromFile (FILE* const fp)
{
	cookedArgs* const result = xMalloc (1, cookedArgs);
	memset (result, 0, sizeof (cookedArgs));
	result->args = argNewFromFile (fp);
	cArgRead (result);
	return result;
}

extern cookedArgs* cArgNewFromLineFile (FILE* const fp)
{
	cookedArgs* const result = xMalloc (1, cookedArgs);
	memset (result, 0, sizeof (cookedArgs));
	result->args = argNewFromLineFile (fp);
	cArgRead (result);
	return result;
}

extern void cArgDelete (cookedArgs* const current)
{
	Assert (current != NULL);
	argDelete (current->args);
	if (current->item != NULL)
		eFree (current->item);
	memset (current, 0, sizeof (cookedArgs));
	eFree (current);
}

static bool cArgOptionPending (cookedArgs* const current)
{
	bool result = false;
	if (current->shortOptions != NULL)
		if (*current->shortOptions != '\0')
			result = true;
	return result;
}

extern bool cArgOff (cookedArgs* const current)
{
	Assert (current != NULL);
	return (bool) (argOff (current->args) && ! cArgOptionPending (current));
}

extern bool cArgIsOption (cookedArgs* const current)
{
	Assert (current != NULL);
	return current->isOption;
}

extern const char* cArgItem (cookedArgs* const current)
{
	Assert (current != NULL);
	return current->item;
}

extern void cArgForth (cookedArgs* const current)
{
	Assert (current != NULL);
	Assert (! cArgOff (current));
	if (current->item != NULL)
		eFree (current->item);
	if (cArgOptionPending (current))
		parseShortOption (current);
	else
	{
		Assert (! argOff (current->args));
		argForth (current->args);
		if (! argOff (current->args))
			cArgRead (current);
		else
		{
			current->isOption = false;
			current->longOption = false;
			current->shortOptions = NULL;
			current->item = NULL;
			current->parameter = NULL;
		}
	}
}

/*
 *  File extension and language mapping
 */

static void addExtensionList (
		stringList *const slist, const char *const elist, const bool clear)
{
	char *const extensionList = eStrdup (elist);
	const char *extension = NULL;
	bool first = true;

	if (clear)
	{
		verbose ("      clearing\n");
		stringListClear (slist);
	}
	verbose ("      adding: ");
	if (elist != NULL  &&  *elist != '\0')
	{
		extension = extensionList;
		if (elist [0] == EXTENSION_SEPARATOR)
			++extension;
	}
	while (extension != NULL)
	{
		char *separator = strchr (extension, EXTENSION_SEPARATOR);
		if (separator != NULL)
			*separator = '\0';
		verbose ("%s%s", first ? "" : ", ",
				*extension == '\0' ? "(NONE)" : extension);
		stringListAdd (slist, vStringNewInit (extension));
		first = false;
		if (separator == NULL)
			extension = NULL;
		else
			extension = separator + 1;
	}
	BEGIN_VERBOSE(vfp);
	{
		fprintf (vfp, "\n      now: ");
		stringListPrint (slist, vfp);
		putc ('\n', vfp);
	}
	END_VERBOSE();
}

static bool isFalse (const char *parameter)
{
	return (bool) (
		strcasecmp (parameter, "0"  ) == 0  ||
		strcasecmp (parameter, "n"  ) == 0  ||
		strcasecmp (parameter, "no" ) == 0  ||
		strcasecmp (parameter, "off") == 0  ||
		strcasecmp (parameter, "false") == 0 );
}

static bool isTrue (const char *parameter)
{
	return (bool) (
		strcasecmp (parameter, "1"  ) == 0  ||
		strcasecmp (parameter, "y"  ) == 0  ||
		strcasecmp (parameter, "yes") == 0  ||
		strcasecmp (parameter, "on" ) == 0  ||
		strcasecmp (parameter, "true" ) == 0);
}

extern bool paramParserBool (const char *value, bool fallback,
							 const char *errWhat, const char *errCategory)
{
	bool r = fallback;

	if (value [0] == '\0')
		r = true;
	else if (isFalse (value))
		r = false;
	else if (isTrue (value))
		r = true;
	else
		error (FATAL, "Invalid value for \"%s\" %s", errWhat, errCategory);

	return r;
}

/*  Determines whether the specified file name is considered to be a header
 *  file for the purposes of determining whether enclosed tags are global or
 *  static.
 */
extern bool isIncludeFile (const char *const fileName)
{
	bool result = false;
	const char *const extension = fileExtension (fileName);
	if (Option.headerExt != NULL)
		result = stringListExtensionMatched (Option.headerExt, extension);
	return result;
}

/*
 *  Specific option processing
 */

static void processEtagsInclude (
		const char *const option, const char *const parameter)
{
	if (! Option.etags)
		error (FATAL, "Etags must be enabled to use \"%s\" option", option);
	else
	{
		vString *const file = vStringNewInit (parameter);
		if (Option.etagsInclude == NULL)
			Option.etagsInclude = stringListNew ();
		stringListAdd (Option.etagsInclude, file);
		FilesRequired = false;
	}
}

static void processExcludeOptionCommon (
	stringList** list, const char *const optname, const char *const parameter)
{
	const char *const fileName = parameter + 1;
	if (parameter [0] == '\0')
		freeList (list);
	else if (parameter [0] == '@')
	{
		stringList* const sl = stringListNewFromFile (fileName);
		if (sl == NULL)
			error (FATAL | PERROR, "cannot open \"%s\"", fileName);
		if (*list == NULL)
			*list = sl;
		else
			stringListCombine (*list, sl);
		verbose ("    adding %s patterns from %s\n", optname, fileName);
	}
	else
	{
		vString *const item = vStringNewInit (parameter);
#if defined (_WIN32)
		vStringTranslate(item, PATH_SEPARATOR, OUTPUT_PATH_SEPARATOR);
#endif
		if (*list == NULL)
			*list = stringListNew ();
		stringListAdd (*list, item);
		verbose ("    adding %s pattern: %s\n", optname, parameter);
	}
}

static void processExcludeOption (
		const char *const option, const char *const parameter)
{
	processExcludeOptionCommon (&Excluded, option, parameter);
}

static void processExcludeExceptionOption (
		const char *const option, const char *const parameter)
{
	processExcludeOptionCommon (&ExcludedException, option, parameter);
}

extern bool isExcludedFile (const char* const name,
							bool falseIfExceptionsAreDefeind)
{
	const char* base = baseFilename (name);
	bool result = false;

	if (falseIfExceptionsAreDefeind
		&& ExcludedException != NULL
		&& stringListCount (ExcludedException) > 0)
		return false;

	if (Excluded != NULL)
	{
		result = stringListFileMatched (Excluded, base);
		if (! result  &&  name != base)
			result = stringListFileMatched (Excluded, name);
	}

	if (result && ExcludedException != NULL)
	{
		bool result_exception;

		result_exception = stringListFileMatched (ExcludedException, base);
		if (! result_exception && name != base)
			result_exception = stringListFileMatched (ExcludedException, name);

		if (result_exception)
			result = false;
	}
	return result;
}

static void processExcmdOption (
		const char *const option, const char *const parameter)
{
	switch (*parameter)
	{
		case 'm': Option.locate = EX_MIX;     break;
		case 'n': Option.locate = EX_LINENUM; break;
		case 'p': Option.locate = EX_PATTERN; break;
		default:
			if (strcmp(parameter, "combine") == 0)
				Option.locate = EX_COMBINE;
			else
				error (FATAL, "Invalid value for \"%s\" option: %s", option, parameter);
			break;
	}
}

static void resetXtags (langType lang, bool mode)
{
	for (unsigned int i = 0; i < countXtags (); i++)
		if ((lang == LANG_AUTO) || (lang == getXtagLanguage (i)))
			enableXtag (i, mode);
}

static void processExtraTagsOption (
		const char *const option, const char *const parameter)
{
	xtagType t;
	const char *p = parameter;
	bool mode = true;
	int c;
	static vString *longName;
	bool inLongName = false;
	const char *x;

	if (strcmp (option, "extra") == 0)
		error(WARNING, "--extra option is obsolete; use --extras instead");

	if (*p == '*')
	{
		resetXtags (LANG_IGNORE, true);
		p++;
	}
	else if (*p != '+'  &&  *p != '-')
		resetXtags (LANG_IGNORE, false);

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
				      "unexpected character in extra specification: \'%c\'",
				      c);
			inLongName = true;
			break;
		case '}':
			if (!inLongName)
				error(FATAL,
				      "unexpected character in extra specification: \'%c\'",
				      c);
			x = vStringValue (longName);
			t = getXtagTypeForNameAndLanguage (x, LANG_IGNORE);

			if (t == XTAG_UNKNOWN)
				error(WARNING, "Unsupported parameter '{%s}' for \"%s\" option",
				      x, option);
			else
				enableXtag (t, mode);

			inLongName = false;
			vStringClear (longName);
			break;
		default:
			if (inLongName)
				vStringPut (longName, c);
			else
			{
				t = getXtagTypeForLetter (c);
				if (t == XTAG_UNKNOWN)
					error(WARNING, "Unsupported parameter '%c' for \"%s\" option",
					      c, option);
				else
					enableXtag (t, mode);
			}
			break;
		}
	}
}

static void resetFieldsOption (langType lang, bool mode)
{
	for (unsigned int i = 0; i < countFields (); ++i)
		if ((lang == LANG_AUTO) || (lang == getFieldLanguage (i)))
			enableField (i, mode);

	if ((lang == LANG_AUTO || lang == LANG_IGNORE)&& !mode)
		Option.fieldsReset = true;
}

static void processFieldsOption (
		const char *const option, const char *const parameter)
{
	const char *p = parameter;
	bool mode = true;
	int c;
	fieldType t;

	static vString * longName;
	bool inLongName = false;

	longName = vStringNewOrClearWithAutoRelease (longName);

	if (*p == '*')
	{
		resetFieldsOption (LANG_IGNORE, true);
		p++;
	}
	else if (*p != '+'  &&  *p != '-')
		resetFieldsOption (LANG_IGNORE, false);

	while ((c = (unsigned char) *p++) != '\0') switch (c)
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
				      "unexpected character in field specification: \'%c\'",
				      c);
			inLongName = true;
			break;
		case '}':
			if (!inLongName)
				error(FATAL,
				      "unexpected character in field specification: \'%c\'",
				      c);

			{
				const char *f = vStringValue (longName);
				t = getFieldTypeForNameAndLanguage (f, LANG_IGNORE);
			}

			if (t == FIELD_UNKNOWN)
				error(FATAL, "no such field: \'%s\'", vStringValue (longName));

			enableField (t, mode);

			inLongName = false;
			vStringClear (longName);
			break;
		default :
			if (inLongName)
				vStringPut (longName, c);
			else
			{
				t = getFieldTypeForLetter (c);
				if (t == FIELD_UNKNOWN)
					error(WARNING, "Unsupported parameter '%c' for \"%s\" option",
					      c, option);
				else
					enableField (t, mode);
			}
			break;
	}
}

static void processFilterTerminatorOption (
		const char *const option CTAGS_ATTR_UNUSED, const char *const parameter)
{
	freeString (&Option.filterTerminator);
	Option.filterTerminator = stringCopy (parameter);
}

static void processFormatOption (
		const char *const option, const char *const parameter)
{
	unsigned int format;

	if (sscanf (parameter, "%u", &format) < 1)
		error (FATAL, "Invalid value for \"%s\" option",option);
	else if (format <= (unsigned int) MaxSupportedTagFormat)
		Option.tagFileFormat = format;
	else
		error (FATAL, "Unsupported value for \"%s\" option", option);
}

#ifdef HAVE_ICONV
static void processInputEncodingOption(const char *const option CTAGS_ATTR_UNUSED,
				const char *const parameter)
{
	if (Option.inputEncoding)
		eFree (Option.inputEncoding);
	else
	{
		if (!Option.outputEncoding)
			Option.outputEncoding = eStrdup("UTF-8");
	}
	Option.inputEncoding = eStrdup(parameter);
}

static void processOutputEncodingOption(const char *const option CTAGS_ATTR_UNUSED,
				const char *const parameter)
{
	if (Option.outputEncoding)
		eFree (Option.outputEncoding);
	Option.outputEncoding = eStrdup(parameter);
}
#endif

static void printInvocationDescription (void)
{
	printf (INVOCATION, getExecutableName ());
}

static int excludesCompare (struct colprintLine *a, struct colprintLine *b)
{
	return strcmp (colprintLineGetColumn (a, 0), colprintLineGetColumn (b, 0));
}

static void processListExcludesOption(const char *const option CTAGS_ATTR_UNUSED,
				      const char *const parameter CTAGS_ATTR_UNUSED)
{
	struct colprintTable *table = colprintTableNew ("L:NAME", NULL);

	const unsigned int max = Excluded ? stringListCount (Excluded) : 0;

	for (unsigned int i = 0; i < max; ++i)
	{
		struct colprintLine * line = colprintTableGetNewLine (table);
		colprintLineAppendColumnVString (line, stringListItem (Excluded, i));
	}

	colprintTableSort (table, excludesCompare);
	colprintTablePrint (table, 0, localOption.withListHeader, localOption.machinable, stdout);
	colprintTableDelete (table);

	/* No line is printed. */
	if (max == 0)
		putchar ('\n');

	exit (0);
}


static void printFeatureList (void)
{
	int i;

	for (i = 0 ; Features [i].name != NULL ; ++i)
	{
		if (i == 0)
			printf ("  Optional compiled features: ");
		if (strcmp (Features [i].name, "regex") != 0 || checkRegex ())
			printf ("%s+%s", (i>0 ? ", " : ""), Features [i].name);
#ifdef CUSTOM_CONFIGURATION_FILE
		if (strcmp (Features [i].name, "custom-conf") == 0)
			printf ("=%s", CUSTOM_CONFIGURATION_FILE);
#endif
	}
	if (i > 0)
		putchar ('\n');
}


static int featureCompare (struct colprintLine *a, struct colprintLine *b)
{
	return strcmp (colprintLineGetColumn (a, 0),
				   colprintLineGetColumn (b, 0));
}

static void processListFeaturesOption(const char *const option CTAGS_ATTR_UNUSED,
				      const char *const parameter CTAGS_ATTR_UNUSED)
{
	int i;

	struct colprintTable *table = colprintTableNew ("L:NAME", "L:DESCRIPTION", NULL);

	for (i = 0 ; Features [i].name != NULL ; ++i)
	{
		struct colprintLine * line = colprintTableGetNewLine (table);
		if (strcmp (Features [i].name, "regex") != 0 || checkRegex ())
		{
			colprintLineAppendColumnCString (line, Features [i].name);
			colprintLineAppendColumnCString (line, Features [i].description);
		}

	}

	colprintTableSort (table, featureCompare);
	colprintTablePrint (table, 0, localOption.withListHeader, localOption.machinable, stdout);
	colprintTableDelete (table);

	if (i == 0)
		putchar ('\n');
	exit (0);
}

static void processListFieldsOption(const char *const option CTAGS_ATTR_UNUSED,
				    const char *const parameter)
{
	/* Before listing, adjust states of enabled/disabled for fixed fields. */
	writerCheckOptions (Option.fieldsReset);

	struct colprintTable * table = fieldColprintTableNew ();

	if (parameter [0] == '\0' || strcasecmp (parameter, RSV_LANG_ALL) == 0)
	{
		fieldColprintAddCommonLines (table);

		initializeParser (LANG_AUTO);
		for (unsigned int i = 0; i < countParsers (); i++)
		{
			if (isLanguageVisible(i))
				fieldColprintAddLanguageLines (table, i);
		}
	}
	else if (strcasecmp (parameter, RSV_NONE) == 0)
		fieldColprintAddCommonLines (table);
	else
	{
		langType language = getNamedLanguage (parameter, 0);
		if (language == LANG_IGNORE)
			error (FATAL, "Unknown language \"%s\" in \"%s\" option", parameter, option);

		initializeParser (language);
		fieldColprintAddLanguageLines (table, language);
	}

	fieldColprintTablePrint (table, localOption.withListHeader, localOption.machinable, stdout);
	colprintTableDelete (table);
	exit (0);
}

static void printProgramIdentification (void)
{
	if ((ctags_repoinfo == NULL)
	    || (strcmp (ctags_repoinfo, PROGRAM_VERSION) == 0))
		printf ("%s %s, %s %s\n",
			PROGRAM_NAME, PROGRAM_VERSION,
			PROGRAM_COPYRIGHT, AUTHOR_NAME);
	else
		printf ("%s %s(%s), %s %s\n",
			PROGRAM_NAME, PROGRAM_VERSION, ctags_repoinfo,
			PROGRAM_COPYRIGHT, AUTHOR_NAME);
	printf ("Universal Ctags is derived from Exuberant Ctags.\n");
	printf ("Exuberant Ctags 5.8, Copyright (C) 1996-2009 Darren Hiebert\n");

	printf ("  Compiled: %s, %s\n", __DATE__, __TIME__);
	printf ("  URL: %s\n", PROGRAM_URL);
	printf ("  Output version: %d.%d\n",
			OUTPUT_VERSION_CURRENT, OUTPUT_VERSION_AGE);

	printFeatureList ();
}

static void processHelpOptionCommon (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter CTAGS_ATTR_UNUSED,
		bool includingExperimentalOptions)
{
	printProgramIdentification ();
	putchar ('\n');
	printInvocationDescription ();
	putchar ('\n');

	int i;
	for (i = 0 ; LongOptionDescription [i].description != NULL ; ++i)
	{
		if ((! Option.etags || LongOptionDescription [i].usedByEtags)
			&& (! LongOptionDescription [i].experimentalOption || includingExperimentalOptions))
			puts (LongOptionDescription [i].description);
	}
}

static void processHelpOption (
		const char *const option,
		const char *const parameter)
{
	processHelpOptionCommon (option, parameter, false);
	exit (0);
}

static void processHelpFullOption (
		const char *const option,
		const char *const parameter)
{
	processHelpOptionCommon (option, parameter, true);
	exit (0);
}

#ifdef HAVE_JANSSON
static void processInteractiveOption (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter)
{
	static struct interactiveModeArgs args;


	if (parameter && (strcmp (parameter, "sandbox") == 0))
	{
		Option.interactive = INTERACTIVE_SANDBOX;
		args.sandbox = true;
	}
	else if (parameter && (strcmp (parameter, "default") == 0))
	{
		Option.interactive = INTERACTIVE_DEFAULT;
		args.sandbox = false;
	}
	else if ((!parameter) || *parameter == '\0')
	{
		Option.interactive = INTERACTIVE_DEFAULT;
		args.sandbox = false;
	}
	else
		error (FATAL, "Unknown option argument \"%s\" for --%s option",
			   parameter, option);

#ifndef HAVE_SECCOMP
	if (args.sandbox)
		error (FATAL, "sandbox submode is not supported on this platform");
#endif

#ifdef ENABLE_GCOV
	if (args.sandbox)
		error (FATAL, "sandbox submode does not work if gcov is instrumented");
#endif

	Option.sorted = SO_UNSORTED;
	setMainLoop (interactiveLoop, &args);
	setErrorPrinter (jsonErrorPrinter, NULL);
	setTagWriter (WRITER_JSON, NULL);
	enablePtag (PTAG_JSON_OUTPUT_VERSION, true);

	json_set_alloc_funcs (eMalloc, eFree);
}
#endif

static void processIf0Option (const char *const option,
							  const char *const parameter)
{
	bool if0 = getBooleanOption (option, parameter);
	langType lang = getNamedLanguage ("CPreProcessor", 0);
	const char *arg = if0? "true": "false";

	applyLanguageParam (lang, "if0", arg);
}

static void processLanguageForceOption (
		const char *const option, const char *const parameter)
{
	langType language;
	if (strcasecmp (parameter, RSV_LANG_AUTO) == 0)
		language = LANG_AUTO;
	else
		language = getNamedLanguage (parameter, 0);

	if (strcmp (option, "lang") == 0  ||  strcmp (option, "language") == 0)
		error (WARNING,
			   "\"--%s\" option is obsolete; use \"--language-force\" instead",
			   option);
	if (language == LANG_IGNORE)
		error (FATAL, "Unknown language \"%s\" in \"%s\" option", parameter, option);
	else
		Option.language = language;
}
static char* skipPastMap (char* p)
{
	while (*p != EXTENSION_SEPARATOR  &&
			*p != PATTERN_START  &&  *p != ','  &&  *p != '\0')
		++p;
	return p;
}

/* Parses the mapping beginning at `map', adds it to the language map, and
 * returns first character past the map.
 */
static char* extractMapFromParameter (const langType language,
				      char* parameter,
				      char** tail,
				      bool* pattern_p,
				      char* (* skip) (char *))
{
	char* p = NULL;
	const char first = *parameter;
	char  tmp;
	char* result;

	if (first == EXTENSION_SEPARATOR)  /* extension map */
	{
		*pattern_p = false;

		++parameter;
		p = (* skip) (parameter);
		if (*p == '\0')
		{
			result = eStrdup (parameter);
			*tail = parameter + strlen (parameter);
			return result;
		}
		else
		{
			tmp = *p;
			*p = '\0';
			result = eStrdup (parameter);
			*p = tmp;
			*tail = p;
			return result;
		}
	}
	else if (first == PATTERN_START)  /* pattern map */
	{
		*pattern_p = true;

		++parameter;
		for (p = parameter  ;  *p != PATTERN_STOP  &&  *p != '\0'  ;  ++p)
		{
			if (*p == '\\'  &&  *(p + 1) == PATTERN_STOP)
				++p;
		}
		if (*p == '\0')
			error (FATAL, "Unterminated file name pattern for %s language",
			   getLanguageName (language));
		else
		{
			tmp = *p;
			*p = '\0';
			result = eStrdup (parameter);
			*p = tmp;
			*tail = p + 1;
			return result;
		}
	}

	return NULL;
}

static char* addLanguageMap (const langType language, char* map_parameter,
			     bool exclusiveInAllLanguages)
{
	char* p = NULL;
	bool pattern_p;
	char* map;

	map = extractMapFromParameter (language, map_parameter, &p, &pattern_p, skipPastMap);
	if (map && pattern_p == false)
		addLanguageExtensionMap (language, map, exclusiveInAllLanguages);
	else if (map && pattern_p == true)
		addLanguagePatternMap (language, map, exclusiveInAllLanguages);
	else
		error (FATAL, "Badly formed language map for %s language",
				getLanguageName (language));

	if (map)
		eFree (map);
	return p;
}

static char* removeLanguageMap (const langType language, char* map_parameter)
{
	char* p = NULL;
	bool pattern_p;
	char* map;

	map = extractMapFromParameter (language, map_parameter, &p, &pattern_p, skipPastMap);
	if (map && pattern_p == false)
		removeLanguageExtensionMap (language, map);
	else if (map && pattern_p == true)
		removeLanguagePatternMap (language, map);
	else
		error (FATAL, "Badly formed language map for %s language",
		       getLanguageName (language));

	if (map)
		eFree (map);
	return p;
}

static char* processLanguageMap (char* map)
{
	char* const separator = strchr (map, ':');
	char* result = NULL;
	if (separator != NULL)
	{
		langType language;
		char *list = separator + 1;
		bool clear = false;
		*separator = '\0';
		language = getNamedLanguage (map, 0);
		if (language != LANG_IGNORE)
		{
			const char *const deflt = RSV_LANGMAP_DEFAULT;
			char* p;
			if (*list == '+')
				++list;
			else
				clear = true;
			for (p = list  ;  *p != ','  &&  *p != '\0'  ;  ++p)  /*no-op*/ ;
			if ((size_t) (p - list) == strlen (deflt) &&
				strncasecmp (list, deflt, p - list) == 0)
			{
				verbose ("    Restoring default %s language map: ", getLanguageName (language));
				installLanguageMapDefault (language);
				list = p;
			}
			else
			{
				if (clear)
				{
					verbose ("    Setting %s language map:", getLanguageName (language));
					clearLanguageMap (language);
				}
				else
					verbose ("    Adding to %s language map:", getLanguageName (language));
				while (list != NULL  &&  *list != '\0'  &&  *list != ',')
					list = addLanguageMap (language, list, true);
				verbose ("\n");
			}
			if (list != NULL  &&  *list == ',')
				++list;
			result = list;
		}
	}
	return result;
}

static void processLanguageMapOption (
		const char *const option, const char *const parameter)
{
	char *const maps = eStrdup (parameter);
	char *map = maps;

	if (strcmp (parameter, RSV_LANGMAP_DEFAULT) == 0)
	{
		verbose ("    Restoring default language maps:\n");
		installLanguageMapDefaults ();
	}
	else while (map != NULL  &&  *map != '\0')
	{
		char* const next = processLanguageMap (map);
		if (next == NULL)
			error (WARNING, "Unknown language \"%s\" in \"%s\" option", parameter, option);
		map = next;
	}
	eFree (maps);
}

static void processLanguagesOption (
		const char *const option, const char *const parameter)
{
	char *const langs = eStrdup (parameter);
	enum { Add, Remove, Replace } mode = Replace;
	bool first = true;
	char *lang = langs;
	const char* prefix = "";
	verbose ("    Enabled languages: ");
	while (lang != NULL)
	{
		char *const end = strchr (lang, ',');
		if (lang [0] == '+')
		{
			++lang;
			mode = Add;
			prefix = "+ ";
		}
		else if (lang [0] == '-')
		{
			++lang;
			mode = Remove;
			prefix = "- ";
		}
		if (mode == Replace)
			enableLanguages (false);
		if (end != NULL)
			*end = '\0';
		if (lang [0] != '\0')
		{
			if (strcmp (lang, RSV_LANG_ALL) == 0)
				enableLanguages ((bool) (mode != Remove));
			else
			{
				const langType language = getNamedLanguage (lang, 0);
				if (language == LANG_IGNORE)
					error (WARNING, "Unknown language \"%s\" in \"%s\" option", lang, option);
				else
					enableLanguage (language, (bool) (mode != Remove));
			}
			verbose ("%s%s%s", (first ? "" : ", "), prefix, lang);
			prefix = "";
			first = false;
			if (mode == Replace)
				mode = Add;
		}
		lang = (end != NULL ? end + 1 : NULL);
	}
	verbose ("\n");
	eFree (langs);
}

extern bool processMapOption (
			const char *const option, const char *const parameter)
{
	langType language;
	const char* spec;
	char* map_parameter;
	bool clear = false;
	char op;

	language = getLanguageComponentInOption (option, "map-");
	if (language == LANG_IGNORE)
		return false;

	if (parameter == NULL || parameter [0] == '\0')
		error (FATAL, "no parameter is given for %s", option);

	spec = parameter;
	if (*spec == '+' || *spec == '-')
	{
		op = *spec;
		spec++;
	}
	else
	{
		op = '\0';
		clear = true;
	}

	if (clear)
	{
		verbose ("    Setting %s language map:", getLanguageName (language));
		clearLanguageMap (language);
		op = '+';
	}
	else
		verbose ("    %s %s %s %s language map:",
			 op == '+'? "Adding": "Removing",
			 spec,
			 op == '+'? "to": "from",
			 getLanguageName (language));
	map_parameter = eStrdup (spec);

	if (op == '+')
		addLanguageMap (language, map_parameter, false);
	else if (op == '-')
		removeLanguageMap (language, map_parameter);
	else
		Assert ("Should not reach here" == NULL);

	eFree (map_parameter);
	verbose ("\n");

	return true;
}

static void processLicenseOption (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter CTAGS_ATTR_UNUSED)
{
	printProgramIdentification ();
	puts ("");
	puts (License1);
	puts (License2);
	exit (0);
}

static void processListAliasesOption (
		const char *const option, const char *const parameter)
{
	if (parameter [0] == '\0' || strcasecmp (parameter, RSV_LANG_ALL) == 0)
		printLanguageAliases (LANG_AUTO,
							  localOption.withListHeader, localOption.machinable, stdout);
	else
	{
		langType language = getNamedLanguage (parameter, 0);
		if (language == LANG_IGNORE)
			error (FATAL, "Unknown language \"%s\" in \"%s\" option", parameter, option);
		else
			printLanguageAliases (language,
								  localOption.withListHeader, localOption.machinable, stdout);
	}
	exit (0);
}

static void processListExtrasOption (
		const char *const option CTAGS_ATTR_UNUSED, const char *const parameter CTAGS_ATTR_UNUSED)
{
	struct colprintTable * table = xtagColprintTableNew ();

	if (parameter [0] == '\0' || strcasecmp (parameter, RSV_LANG_ALL) == 0)
	{
		xtagColprintAddCommonLines (table);

		initializeParser (LANG_AUTO);
		for (unsigned int i = 0; i < countParsers (); i++)
		{
			if (isLanguageVisible(i))
				xtagColprintAddLanguageLines (table, i);
		}
	}
	else if (strcasecmp (parameter, RSV_NONE) == 0)
		xtagColprintAddCommonLines (table);
	else
	{
		langType language = getNamedLanguage (parameter, 0);
		if (language == LANG_IGNORE)
			error (FATAL, "Unknown language \"%s\" in \"%s\" option", parameter, option);

		initializeParser (language);
		xtagColprintAddLanguageLines (table, language);
	}

	xtagColprintTablePrint (table, localOption.withListHeader, localOption.machinable, stdout);
	colprintTableDelete (table);
	exit (0);
}

static void processListKindsOption (
		const char *const option, const char *const parameter)
{
	bool print_all = (strcmp (option, "list-kinds-full") == 0)? true: false;

	if (parameter [0] == '\0' || strcasecmp (parameter, RSV_LANG_ALL) == 0)
		printLanguageKinds (LANG_AUTO, print_all,
							localOption.withListHeader, localOption.machinable, stdout);
	else
	{
		langType language = getNamedLanguage (parameter, 0);
		if (language == LANG_IGNORE)
			error (FATAL, "Unknown language \"%s\" in \"%s\" option", parameter, option);
		else
			printLanguageKinds (language, print_all,
								localOption.withListHeader, localOption.machinable, stdout);
	}
	exit (0);
}

static void processListParametersOption (const char *const option,
										 const char *const parameter)
{
	if (parameter [0] == '\0' || strcasecmp (parameter, RSV_LANG_ALL) == 0)
		printLanguageParams (LANG_AUTO,
							 localOption.withListHeader, localOption.machinable,
							 stdout);
	else
	{
		langType language = getNamedLanguage (parameter, 0);
		if (language == LANG_IGNORE)
			error (FATAL, "Unknown language \"%s\" in \"%s\" option", parameter, option);
		else
			printLanguageParams (language,
								 localOption.withListHeader, localOption.machinable,
								 stdout);
	}
	exit (0);
}


static void processListMapsOptionForType (const char *const option CTAGS_ATTR_UNUSED,
					  const char *const  parameter,
					  langmapType type)
{
	if (parameter [0] == '\0' || strcasecmp (parameter, RSV_LANG_ALL) == 0)
		printLanguageMaps (LANG_AUTO, type,
						   localOption.withListHeader, localOption.machinable,
						   stdout);
	else
	{
		langType language = getNamedLanguage (parameter, 0);
		if (language == LANG_IGNORE)
			error (FATAL, "Unknown language \"%s\" in \"%s\" option", parameter, option);
		else
			printLanguageMaps (language, type,
							   localOption.withListHeader, localOption.machinable,
							   stdout);
	}
	exit (0);
}

static void processListMapExtensionsOption (const char *const option,
					 const char *const parameter)
{
	processListMapsOptionForType (option, parameter, LMAP_EXTENSION|LMAP_TABLE_OUTPUT);
}

static void processListMapPatternsOption (const char *const option,
				       const char *const parameter)
{
	processListMapsOptionForType (option, parameter, LMAP_PATTERN|LMAP_TABLE_OUTPUT);
}

static void processListMapsOption (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter CTAGS_ATTR_UNUSED)
{
	processListMapsOptionForType (option, parameter, LMAP_ALL);
}

static void processListLanguagesOption (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter)
{
	enum parserCategory category = PARSER_CATEGORY_NONE;

	if (parameter)
	{
		if (strcmp(parameter, "_libxml") == 0)
			category = PARSER_CATEGORY_LIBXML;
		else if (strcmp(parameter, "_libyaml") == 0)
			category = PARSER_CATEGORY_LIBYAML;
		else if (strcmp(parameter, "_packcc") == 0)
			category = PARSER_CATEGORY_PACKCC;
	}

	printLanguageList (category);
	exit (0);
}

static void processListPseudoTagsOptions (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter CTAGS_ATTR_UNUSED)
{
	printPtags (localOption.withListHeader, localOption.machinable, stdout);
	exit (0);
}

static void processListRegexFlagsOptions (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter)
{
	printRegexFlags (localOption.withListHeader, localOption.machinable, parameter, stdout);
	exit (0);
}

static void processListMultilineRegexFlagsOptions (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter)
{
	printMultilineRegexFlags (localOption.withListHeader, localOption.machinable, parameter, stdout);
	exit (0);
}

static void processListMultitableRegexFlagsOptions (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter)
{
	printMultitableRegexFlags (localOption.withListHeader, localOption.machinable, parameter, stdout);
	exit (0);
}

static void processListLangdefFlagsOptions (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter CTAGS_ATTR_UNUSED)
{
	printLangdefFlags (localOption.withListHeader, localOption.machinable, stdout);
	exit (0);
}

static void processListKinddefFlagsOptions (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter CTAGS_ATTR_UNUSED)
{
	printKinddefFlags (localOption.withListHeader, localOption.machinable, stdout);
	exit (0);
}

static void processListFielddefFlagsOptions (
		const char *const option CTAGS_ATTR_UNUSED,
		const char *const parameter CTAGS_ATTR_UNUSED)
{
	printFielddefFlags (localOption.withListHeader, localOption.machinable, stdout);
	exit (0);
}

attr__noreturn
static void processListRolesOptions (const char *const option CTAGS_ATTR_UNUSED,
				     const char *const parameter)
{
	const char* sep;
	const char *kindspecs;
	langType lang;


	if (parameter == NULL || parameter[0] == '\0')
	{
		printLanguageRoles (LANG_AUTO, "*",
							localOption.withListHeader,
							localOption.machinable,
							stdout);
		exit (0);
	}

	sep = strchr (parameter, '.');

	if (sep == NULL || sep [1] == '\0')
	{
		vString* vstr = vStringNewInit (parameter);
		vStringCatS (vstr, (sep? "*": ".*"));
		processListRolesOptions (option, vStringValue (vstr));
		/* The control should never reached here. */
	}

	kindspecs = sep + 1;
	if (strncmp (parameter, "all.", 4) == 0
		/* Handle the case if no language is specified.
		 * This case is not documented. */
	    || strncmp (parameter, ".", 1) == 0)
		lang = LANG_AUTO;
	else
	{
		lang = getNamedLanguage (parameter, sep - parameter);
		if (lang == LANG_IGNORE)
		{
			const char *langName = eStrndup (parameter, sep - parameter);
			error (FATAL, "Unknown language \"%s\" in \"%s\"", langName, option);
		}
	}
	printLanguageRoles (lang, kindspecs,
						localOption.withListHeader,
						localOption.machinable,
						stdout);
	exit (0);
}

static void processListSubparsersOptions (const char *const option CTAGS_ATTR_UNUSED,
				     const char *const parameter)
{
	langType lang;


	if (parameter == NULL || parameter[0] == '\0'
		|| (strcmp(parameter, RSV_LANG_ALL) == 0))
	{
		printLanguageSubparsers(LANG_AUTO,
								localOption.withListHeader, localOption.machinable,
								stdout);
		exit (0);
	}

	lang = getNamedLanguage (parameter, 0);
	if (lang == LANG_IGNORE)
		error (FATAL, "Unknown language \"%s\" in \"%s\"", parameter, option);

	printLanguageSubparsers(lang,
							localOption.withListHeader, localOption.machinable,
							stdout);
	exit (0);
}

static void processListOperators (const char *const option CTAGS_ATTR_UNUSED,
								  const char *const parameter)
{
	listRegexOpscriptOperators (stdout);
	exit (0);
}

static void processListOutputFormatsOption(const char *const option CTAGS_ATTR_UNUSED,
										   const char *const parameter CTAGS_ATTR_UNUSED)
{
	printOutputFormats (localOption.withListHeader, localOption.machinable, stdout);
	exit (0);
}

static void freeSearchPathList (searchPathList** pathList)
{
	stringListClear (*pathList);
	stringListDelete (*pathList);
	*pathList = NULL;
}

static vString* expandOnSearchPathList (searchPathList *pathList, const char* leaf,
					bool (* check) (const char *const))
{
	for (unsigned int i = stringListCount (pathList); i > 0; --i)
	{
		const char* const body = vStringValue (stringListItem (pathList, i - 1));
		char* tmp = combinePathAndFile (body, leaf);

		if ((* check) (tmp))
		{
			vString *r = vStringNewOwn (tmp);
			return r;
		}
		else
			eFree (tmp);
	}
	return NULL;
}

static vString* expandOnOptlibPathList (const char* leaf)
{

	return expandOnSearchPathList (OptlibPathList, leaf,
								   doesFileExist);
}

static void processOptionFileCommon (
	const char *const option, const char *const parameter,
	bool allowNonExistingFile)
{
	const char* path;
	vString* vpath = NULL;
	fileStatus *status;

	if (parameter [0] == '\0')
		error (FATAL, "no option file supplied for \"%s\"", option);

	if (parameter [0] != '/' && parameter [0] != '.')
	{
		vpath = expandOnOptlibPathList (parameter);
		path = vpath? vStringValue (vpath): parameter;
	}
	else
		path = parameter;

	status = eStat (path);
	if (!status->exists)
	{
		if (!allowNonExistingFile)
			error (FATAL | PERROR, "cannot stat \"%s\"", path);
	}
	else if (status->isDirectory)
	{
		if (!parseAllConfigurationFilesOptionsInDirectory (path, NULL))
			error (FATAL | PERROR, "cannot open option directory \"%s\"", path);
	}
	else
	{
		if (!parseFileOptions (path))
			error (FATAL | PERROR, "cannot open option file \"%s\"", path);
	}

	eStatFree (status);
	if (vpath)
		vStringDelete (vpath);
}

static void processOptionFile (
	const char *const option, const char *const parameter)
{
	processOptionFileCommon(option, parameter, false);
}

static void processOptionFileMaybe (
	const char *const option, const char *const parameter)
{
	processOptionFileCommon(option, parameter, true);
}

static void processOutputFormat (const char *const option CTAGS_ATTR_UNUSED,
				 const char *const parameter)
{
	if (parameter [0] == '\0')
		error (FATAL, "no output format name supplied for \"%s\"", option);

	writerType t = getWrierForOutputFormat (parameter);
	if (t == WRITER_DEFAULT)
		return;

	switch (t)
	{
	case WRITER_UNAVAILABLE:
		error (FATAL,
			   "the output format \"%s\" is not available on this platform",
			   parameter);
		break;
	case WRITER_UNKNOWN:
		error (FATAL, "unknown output format name supplied for \"%s=%s\"",
			   option, parameter);
		break;

	case WRITER_U_CTAGS:
	case WRITER_E_CTAGS:
		setTagWriter (t, NULL);
		break;
	case WRITER_ETAGS:
		setEtagsMode ();
		break;
	case WRITER_XREF:
		setXrefMode ();
		break;
#ifdef HAVE_JANSSON
	case WRITER_JSON:
		setJsonMode ();
		break;
#endif
	case WRITER_CUSTOM:
	case WRITER_COUNT:			/* Suppress warnings that gcc reports */
		AssertNotReached ();
		break;
	}
}

static void processPseudoTags (const char *const option CTAGS_ATTR_UNUSED,
			       const char *const parameter)
{
	const char *p = parameter;
	bool s = true;
	ptagType t;
	vString *str = vStringNew();

	if (*p == '\0' || !strchr ("*+-", *p))
	{
		for (unsigned int i = 0; i < PTAG_COUNT; i++)
			enablePtag (i, false);
	}

	while (1)
	{
		if (*p == '\0')
			break;

		if (*p == '*')
		{
			int i;
			for (i = 0; i < PTAG_COUNT; i++)
				enablePtag (i, true);
			p++;
			continue;
		}
		else if (*p == '-')
		{
			s= false;
			p++;
			continue;
		}
		else if (*p == '+')
		{
			s = true;
			p++;
			continue;
		}
		else if (*p == '{')
		{
			const char *origin = p;

			p++;
			while (*p != '\0' && *p != '}')
			{
				vStringPut (str, *p);
				p++;
			}
			if (*p != '}')
				error (FATAL, "curly bracket specifying a pseudo tags is unbalanced: %s",
					   origin);
			p++;
		}
		else
		{
			vStringCopyS (str, p);
			p += vStringLength (str);
		}

		char *name = vStringValue (str);
		t = getPtagTypeForName (name);
		if (t == PTAG_UNKNOWN)
			error (FATAL, "Unknown pseudo tag name: %s", name);
		enablePtag (t, s);
		vStringClear (str);
	}
	vStringDelete (str);
}

static void processSortOption (
		const char *const option, const char *const parameter)
{
	if (isFalse (parameter))
		Option.sorted = SO_UNSORTED;
	else if (isTrue (parameter))
		Option.sorted = SO_SORTED;
	else if (strcasecmp (parameter, "f") == 0 ||
			strcasecmp (parameter, "fold") == 0 ||
			strcasecmp (parameter, "foldcase") == 0)
		Option.sorted = SO_FOLDSORTED;
	else
		error (FATAL, "Invalid value for \"%s\" option", option);
}

static void processTagRelative (
		const char *const option, const char *const parameter)
{
	if (isFalse (parameter))
		Option.tagRelative = TREL_NO;
	else if (isTrue (parameter) || *parameter == '\0')
		Option.tagRelative = TREL_YES;
	else if (strcasecmp (parameter, "always") == 0)
		Option.tagRelative = TREL_ALWAYS;
	else if (strcasecmp (parameter, "never") == 0)
		Option.tagRelative = TREL_NEVER;
	else
		error (FATAL, "Invalid value for \"%s\" option", option);
}

static void processTotals (
		const char *const option, const char *const parameter)
{
	if (isFalse (parameter))
		Option.printTotals = 0;
	else if (isTrue (parameter) || *parameter == '\0')
		Option.printTotals = 1;
	else if (strcasecmp (parameter, "extra") == 0)
		Option.printTotals = 2;
	else
		error (FATAL, "Invalid value for \"%s\" option", option);
}

static void installHeaderListDefaults (void)
{
	Option.headerExt = stringListNewFromArgv (HeaderExtensions);

	BEGIN_VERBOSE(vfp);
	{
		fprintf (vfp, "    Setting default header extensions: ");
		stringListPrint (Option.headerExt, vfp);
		putc ('\n', vfp);
	}
	END_VERBOSE();
}

static void processHeaderListOption (const int option, const char *parameter)
{
	/*  Check to make sure that the user did not enter "ctags -h *.c"
	 *  by testing to see if the list is a filename that exists.
	 */
	if (doesFileExist (parameter))
		error (FATAL, "-%c: Invalid list", option);
	if (strcmp (parameter, "default") == 0)
		installHeaderListDefaults ();
	else
	{
		bool clear = true;

		if (parameter [0] == '+')
		{
			++parameter;
			clear = false;
		}
		if (Option.headerExt == NULL)
			Option.headerExt = stringListNew ();
		verbose ("    Header Extensions:\n");
		addExtensionList (Option.headerExt, parameter, clear);
	}
}

/*
 *  Token ignore processing
 */
static void readIgnoreList (langType lang, const char *const list)
{
	char* newList = stringCopy (list);
	const char *token = strtok (newList, IGNORE_SEPARATORS);

	while (token != NULL)
	{
		applyLanguageParam (lang, "ignore", token);
		token = strtok (NULL, IGNORE_SEPARATORS);
	}
	eFree (newList);
}

static void addIgnoreListFromFile (langType lang, const char *const fileName)
{
	stringList* tokens = stringListNewFromFile (fileName);
	if (tokens == NULL)
		error (FATAL | PERROR, "cannot open \"%s\"", fileName);

	int c = stringListCount(tokens);

	for(unsigned int i=0;i<c;i++)
	{
		vString * s = stringListItem(tokens,i);
		applyLanguageParam (lang, "ignore", vStringValue(s));
	}

	stringListDelete(tokens);
}

static void processIgnoreOption (const char *const list, int IgnoreOrDefine)
{
	langType lang = getNamedLanguage ("CPreProcessor", 0);

	if (IgnoreOrDefine == 'D')
		applyLanguageParam (lang, "define", list);
	else if (strchr ("@./\\", list [0]) != NULL)
	{
		const char* fileName = (*list == '@') ? list + 1 : list;
		addIgnoreListFromFile (lang, fileName);
	}
#if defined (_WIN32)
	else if (isalpha ((unsigned char) list [0])  &&  list [1] == ':')
		addIgnoreListFromFile (lang, list);
#endif
	else if (strcmp (list, "-") == 0)
		applyLanguageParam (lang, "ignore", NULL);
	else
		readIgnoreList (lang, list);
}

static void processAnonHashOption (const char *const option CTAGS_ATTR_UNUSED, const char *const parameter CTAGS_ATTR_UNUSED)
{
	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "Something string is needed for \"%s\" option", option);
	char buf [9];

	anonHashString (parameter, buf);
	printf("%s\n", buf);
	exit (0);
}

static void processDumpKeywordsOption (const char *const option CTAGS_ATTR_UNUSED, const char *const parameter CTAGS_ATTR_UNUSED)
{
	dumpKeywordTable (stdout);
}

static void processEchoOption (const char *const option, const char *const parameter)
{
	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "Something message is needed for \"%s\" option", option);
	notice ("%s", parameter);
}

static void processForceInitOption (const char *const option CTAGS_ATTR_UNUSED,
				    const char *const parameter CTAGS_ATTR_UNUSED)
{
	verbose ("force initializing all built-in parsers\n");
	initializeParser (LANG_AUTO);
}

static void processForceQuitOption (const char *const option CTAGS_ATTR_UNUSED,
				    const char *const parameter)
{
	int s;
	if (parameter == NULL || parameter[0] == '\0' || !strToInt(parameter, 0, &s))
		s = 0;
	exit (s);
}

static void processVersionOption (
		const char *const option,
		const char *const parameter)
{
	if (parameter == NULL || *parameter == '\0')
		printProgramIdentification ();
	else if (strcmp (parameter, RSV_NONE) == 0)
	{
		printf("ctags: %s\n", PROGRAM_VERSION);
		if (! ((ctags_repoinfo == NULL)
			   || (strcmp (ctags_repoinfo, PROGRAM_VERSION) == 0)))
			printf("repoinfo: %s\n", ctags_repoinfo);
		printf("output: %d.%d\n", OUTPUT_VERSION_CURRENT, OUTPUT_VERSION_AGE);
	}
	else
	{
		langType language = getNamedLanguage (parameter, 0);
		if (language == LANG_IGNORE)
			error (FATAL, "Unknown language \"%s\" in \"%s\"", parameter, option);
		unsigned int current = getLanguageVersionCurrent (language);
		unsigned int age = getLanguageVersionAge (language);
		printf("parser/%s: %u.%u\n", parameter, current, age);
	}
	exit (0);
}

#ifdef DO_TRACING
static void processTraceOption(const char *const option CTAGS_ATTR_UNUSED,
							   const char *const parameter)
{
	const char *langs = eStrdup (parameter);
	const char *lang = langs;

	traceMain();

	while (lang != NULL)
	{
		if (*lang == '\0')
			break;
		if (*lang == ',')
		{
			lang++;
			continue;
		}

		char *const end = strchr (lang, ',');

		if (end != NULL)
			*end = '\0';

		const langType language = getNamedLanguage (lang, 0);
		if (language == LANG_IGNORE)
			error (WARNING, "Unknown language \"%s\" in \"%s\" option", lang, option);
		else
		{
			traceLanguage (language);
			verbose ("Enable tracing language: %s\n", lang);

		}
		lang = (end != NULL ? end + 1 : NULL);
	}
	eFree ((char *)langs);
}
#endif

static void processXformatOption (const char *const option CTAGS_ATTR_UNUSED,
				  const char *const parameter)
{
	if (Option.customXfmt)
		fmtDelete (Option.customXfmt);

	Option.customXfmt = fmtNew (parameter);
}

static void prependToOptlibPathList (const char *const dir, bool report_in_verbose)
{
	vString *elt = vStringNewInit (dir);

	if (report_in_verbose)
		verbose ("Prepend %s to %s\n",
				 dir, "OptlibPathList");

	stringListAdd (OptlibPathList, elt);
}

static void resetOptlibPathList (bool report_in_verbose)
{
	freeSearchPathList (&OptlibPathList);
	if (report_in_verbose)
		verbose ("Reset OptlibPathList\n");
	OptlibPathList = stringListNew ();
}

static void processOptlibDir (
		const char *const option, const char *const parameter)
{
	const char* path;

	Assert (parameter);


	if (parameter[0] == '\0')
		resetOptlibPathList (true);
	else if (parameter[0] == '+')
	{
		path = parameter + 1;
		if (path[0] == '\0')
			return;
		prependToOptlibPathList (path, true);
	}
	else
	{
		resetOptlibPathList (true);
		path = parameter;
		prependToOptlibPathList (path, true);
	}
}

static void processMaxRecursionDepthOption (const char *const option, const char *const parameter)
{
	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "A parameter is needed after \"%s\" option", option);

	if (atol (parameter) < 1)
		error (FATAL, "-%s: Invalid maximum recursion depth", option);

	Option.maxRecursionDepth = atol(parameter);
}

static void processPatternLengthLimit(const char *const option, const char *const parameter)
{
	if (parameter == NULL || parameter[0] == '\0')
		error (FATAL, "A parameter is needed after \"%s\" option", option);

	if (!strToUInt(parameter, 0, &Option.patternLengthLimit))
		error (FATAL, "-%s: Invalid pattern length limit", option);
}

extern bool ptagMakePatternLengthLimit (ptagDesc *pdesc, langType language CTAGS_ATTR_UNUSED,
										const void *data)
{
	const optionValues *opt = data;
	char buf [21];

	if (snprintf (buf, 21, "%u", opt->patternLengthLimit) >= 0)
		return writePseudoTag (pdesc, buf, "0 for no limit", NULL);
	return false;
}

static void setBooleanToXtagWithWarning(booleanOption *const option, bool value)
{
	/* WARNING/TODO: This function breaks encapsulation. */

	char x = 0;

	if (strcmp (option->name, "file-tags") == 0)
		x = 'f';
	else if (strcmp (option->name, "file-scope") == 0)
		x = 'F';

	if (x)
		error (WARNING, "\"--%s\" option is obsolete; use \"--extras=%c%c\" instead",
			   option->name, value? '+': '-', x);

	xtagType t = (xtagType)option->pValue;
	enableXtag (t, value);
}

/*
 *  Option tables
 */

static void processDumpOptionsOption (const char *const option, const char *const parameter);
static void processDumpPreludeOption (const char *const option, const char *const parameter);

static parametricOption ParametricOptions [] = {
	{ "etags-include",          processEtagsInclude,            false,  STAGE_ANY },
	{ "exclude",                processExcludeOption,           false,  STAGE_ANY },
	{ "exclude-exception",      processExcludeExceptionOption,  false,  STAGE_ANY },
	{ "excmd",                  processExcmdOption,             false,  STAGE_ANY },
	{ "extra",                  processExtraTagsOption,         false,  STAGE_ANY },
	{ "extras",                 processExtraTagsOption,         false,  STAGE_ANY },
	{ "fields",                 processFieldsOption,            false,  STAGE_ANY },
	{ "filter-terminator",      processFilterTerminatorOption,  true,   STAGE_ANY },
	{ "format",                 processFormatOption,            true,   STAGE_ANY },
	{ "help",                   processHelpOption,              true,   STAGE_ANY },
	{ "help-full",              processHelpFullOption,          true,   STAGE_ANY },
	{ "if0",                    processIf0Option,               false,  STAGE_ANY },
#ifdef HAVE_ICONV
	{ "input-encoding",         processInputEncodingOption,     false,  STAGE_ANY },
	{ "output-encoding",        processOutputEncodingOption,    false,  STAGE_ANY },
#endif
	{ "lang",                   processLanguageForceOption,     false,  STAGE_ANY },
	{ "language",               processLanguageForceOption,     false,  STAGE_ANY },
	{ "language-force",         processLanguageForceOption,     false,  STAGE_ANY },
	{ "languages",              processLanguagesOption,         false,  STAGE_ANY },
	{ "langdef",                processLanguageDefineOption,    false,  STAGE_ANY },
	{ "langmap",                processLanguageMapOption,       false,  STAGE_ANY },
	{ "license",                processLicenseOption,           true,   STAGE_ANY },
	{ "list-aliases",           processListAliasesOption,       true,   STAGE_ANY },
	{ "list-excludes",          processListExcludesOption,      true,   STAGE_ANY },
	{ "list-extras",            processListExtrasOption,        true,   STAGE_ANY },
	{ "list-features",          processListFeaturesOption,      true,   STAGE_ANY },
	{ "list-fields",            processListFieldsOption,        true,   STAGE_ANY },
	{ "list-kinds",             processListKindsOption,         true,   STAGE_ANY },
	{ "list-kinds-full",        processListKindsOption,         true,   STAGE_ANY },
	{ "list-languages",         processListLanguagesOption,     true,   STAGE_ANY },
	{ "list-maps",              processListMapsOption,          true,   STAGE_ANY },
	{ "list-map-extensions",    processListMapExtensionsOption, true,   STAGE_ANY },
	{ "list-map-patterns",      processListMapPatternsOption,   true,   STAGE_ANY },
	{ "list-mline-regex-flags", processListMultilineRegexFlagsOptions, true, STAGE_ANY },
	{ "list-output-formats",    processListOutputFormatsOption, true,   STAGE_ANY },
	{ "list-params",            processListParametersOption,    true,   STAGE_ANY },
	{ "list-pseudo-tags",       processListPseudoTagsOptions,   true,   STAGE_ANY },
	{ "list-regex-flags",       processListRegexFlagsOptions,   true,   STAGE_ANY },
	{ "list-roles",             processListRolesOptions,        true,   STAGE_ANY },
	{ "list-subparsers",        processListSubparsersOptions,   true,   STAGE_ANY },
	{ "maxdepth",               processMaxRecursionDepthOption, true,   STAGE_ANY },
	{ "optlib-dir",             processOptlibDir,               false,  STAGE_ANY },
	{ "options",                processOptionFile,              false,  STAGE_ANY },
	{ "options-maybe",          processOptionFileMaybe,         false,  STAGE_ANY },
	{ "output-format",          processOutputFormat,            true,   STAGE_ANY },
	{ "pattern-length-limit",   processPatternLengthLimit,      true,   STAGE_ANY },
	{ "pseudo-tags",            processPseudoTags,              false,  STAGE_ANY },
	{ "sort",                   processSortOption,              true,   STAGE_ANY },
	{ "tag-relative",           processTagRelative,             true,   STAGE_ANY },
	{ "totals",                 processTotals,                  true,   STAGE_ANY },
	{ "version",                processVersionOption,           true,   STAGE_ANY },
	{ "_anonhash",              processAnonHashOption,          false,  STAGE_ANY },
	{ "_dump-keywords",         processDumpKeywordsOption,      false,  STAGE_ANY },
	{ "_dump-options",          processDumpOptionsOption,       false,  STAGE_ANY },
	{ "_dump-prelude",          processDumpPreludeOption,       false,  STAGE_ANY },
	{ "_echo",                  processEchoOption,              false,  STAGE_ANY },
	{ "_force-initializing",    processForceInitOption,         false,  STAGE_ANY },
	{ "_force-quit",            processForceQuitOption,         false,  STAGE_ANY },
#ifdef HAVE_JANSSON
	{ "_interactive",           processInteractiveOption,       true,   STAGE_ANY },
#endif
	{ "_list-fielddef-flags",   processListFielddefFlagsOptions,true,   STAGE_ANY },
	{ "_list-kinddef-flags",    processListKinddefFlagsOptions, true,   STAGE_ANY },
	{ "_list-langdef-flags",    processListLangdefFlagsOptions, true,   STAGE_ANY },
	{ "_list-mtable-regex-flags", processListMultitableRegexFlagsOptions, true, STAGE_ANY },
	{ "_list-operators",        processListOperators,           true,   STAGE_ANY },
#ifdef DO_TRACING
	{ "_trace",                 processTraceOption,             false,  STAGE_ANY },
#endif
	{ "_xformat",               processXformatOption,           false,  STAGE_ANY },
};

static booleanOption BooleanOptions [] = {
	{ "append",         &Option.append,                 true,  STAGE_ANY },
	{ "file-scope",     ((bool *)XTAG_FILE_SCOPE),      false, STAGE_ANY, setBooleanToXtagWithWarning },
	{ "file-tags",      ((bool *)XTAG_FILE_NAMES),      false, STAGE_ANY, setBooleanToXtagWithWarning },
	{ "filter",         &Option.filter,                 true,  STAGE_ANY },
	{ "guess-language-eagerly", &Option.guessLanguageEagerly, false, STAGE_ANY },
	{ "line-directives",&Option.lineDirectives,         false, STAGE_ANY },
	{ "links",          &Option.followLinks,            false, STAGE_ANY },
	{ "machinable",     &localOption.machinable,        true,  STAGE_ANY },
	{ "put-field-prefix", &Option.putFieldPrefix,       false, STAGE_ANY },
	{ "print-language", &Option.printLanguage,          true,  STAGE_ANY },
	{ "quiet",          &Option.quiet,                  false, STAGE_ANY },
#ifdef RECURSE_SUPPORTED
	{ "recurse",        &Option.recurse,                false, STAGE_ANY },
#endif
	{ "verbose",        &ctags_verbose,                 false, STAGE_ANY },
#ifdef _WIN32
	{ "use-slash-as-filename-separator", (bool *)&Option.useSlashAsFilenameSeparator, false, STAGE_ANY },
#endif
	{ "with-list-header", &localOption.withListHeader,  true,  STAGE_ANY },
	{ "_fatal-warnings",&Option.fatalWarnings,          false, STAGE_ANY },
};

/*
 *  Generic option parsing
 */

static void checkOptionOrder (const char* const option, bool longOption)
{
	if (NonOptionEncountered)
		error (FATAL, "-%s%s option may not follow a file name", longOption? "-": "", option);
}

static bool processParametricOption (
		const char *const option, const char *const parameter)
{
	const int count = sizeof (ParametricOptions) / sizeof (parametricOption);
	bool found = false;
	int i;

	for (i = 0  ;  i < count  &&  ! found  ;  ++i)
	{
		parametricOption* const entry = &ParametricOptions [i];
		if (strcmp (option, entry->name) == 0)
		{
			found = true;
			if (!(entry->acceptableStages & (1UL << Stage)))
			{
				error (WARNING, "Cannot use --%s option in %s",
				       option, StageDescription[Stage]);
				break;
			}
			if (entry->initOnly)
				checkOptionOrder (option, true);
			(entry->handler) (option, parameter);
		}
	}
	return found;
}

static bool getBooleanOption (
		const char *const option, const char *const parameter)
{
	return paramParserBool (parameter, true, option, "option");
}

static bool processBooleanOption (
		const char *const option, const char *const parameter)
{
	const int count = sizeof (BooleanOptions) / sizeof (booleanOption);
	bool found = false;
	int i;

	for (i = 0  ;  i < count  &&  ! found  ;  ++i)
	{
		booleanOption* const entry = &BooleanOptions [i];
		if (strcmp (option, entry->name) == 0)
		{
			found = true;
			if (!(entry->acceptableStages & (1UL << Stage)))
			{
				error (WARNING, "Cannot use --%s option in %s",
				       option, StageDescription[Stage]);
				break;
			}
			if (entry->initOnly)
				checkOptionOrder (option, true);

			bool value = getBooleanOption (option, parameter);
			if (entry->set)
				entry->set (entry, value);
			else
				*entry->pValue = value;
		}
	}
	return found;
}

static void enableLanguageField (langType language, const char *field, bool mode)
{

	fieldType t;

	t = getFieldTypeForNameAndLanguage (field, language);
	if (t == FIELD_UNKNOWN)
		error(FATAL, "no such field: \'%s\'", field);
	enableField (t, mode);
	if (language == LANG_AUTO)
	{
		fieldType ftype_next = t;

		while ((ftype_next = nextSiblingField (ftype_next)) != FIELD_UNKNOWN)
			enableField (ftype_next, mode);
	}
}

static void enableLanguageXtag (langType language, const char *xtag, bool mode)
{

	xtagType x;

	x = getXtagTypeForNameAndLanguage (xtag, language);
	if (x == XTAG_UNKNOWN)
		error(FATAL, "no such extra: \'%s\'", xtag);
	enableXtag (x, mode);
	if (language == LANG_AUTO)
	{
		xtagType xtag_next = x;

		while ((xtag_next = nextSiblingXtag (xtag_next)) != XTAG_UNKNOWN)
			enableXtag (xtag_next, mode);
	}
}

static bool processLangSpecificFieldsOption (const char *const option,
						const char *const parameter)
{
#define PREFIX "fields-"
#define PREFIX_LEN strlen(PREFIX)
	const char* lang;
	size_t len;
	langType language = LANG_IGNORE;
	const char *p = parameter;
	int c;
	static vString * longName;
	bool mode = true;
	const char *f;
	bool inLongName = false;

	if ( strncmp (option, PREFIX, PREFIX_LEN) != 0 )
		return false;

	lang = option + PREFIX_LEN;
	len = strlen (lang);
	if (len == 0)
		error (FATAL, "No language given in \"%s\" option", option);
	else if (len == strlen(RSV_LANG_ALL) && (strncmp(lang, RSV_LANG_ALL, len) == 0))
		language = LANG_AUTO;
	else
		language = getNamedLanguage (lang, len);

	if (language == LANG_IGNORE)
	{
		error (WARNING, "Unknown language: %s (ignoring \"--%s\")", lang, option);
		/* The option is consumed in this function. */
		return true;
	}

	initializeParser (language);

	if (*p == '*')
	{
		resetFieldsOption (language, true);
		p++;
	}
	else if (*p == '{' || *p == '\0')
	{
		resetFieldsOption (language, false);
		if (*p == '\0')
			return true;
	}
	else if (*p != '+' && *p != '-')
		error (WARNING, "Wrong per language field specification: %s", p);

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
				error (FATAL,
				       "unexpected character in field specification: \'%c\'",
				       c);
			inLongName = true;
			break;
		case '}':
			if (!inLongName)
				error (FATAL,
				       "unexpected character in field specification: \'%c\'",
				       c);

			f = vStringValue (longName);
			enableLanguageField (language, f, mode);
			inLongName = false;
			vStringClear (longName);
			break;
		default:
			if (inLongName)
				vStringPut (longName, c);
			else
				error (FATAL,
				       "only long name can be used in per language field spec: \'%c\'",
				       c);
			break;
		}
	}
#undef PREFIX_LEN
#undef PREFIX
	return true;
}

static bool processLangSpecificExtraOption (const char *const option,
						const char *const parameter)
{
#define PREFIX "extras-"
#define PREFIX_LEN strlen(PREFIX)
	const char* lang;
	size_t len;
	langType language = LANG_IGNORE;
	const char *p = parameter;
	int c;
	static vString * longName;
	bool mode = true;
	const char *x;
	bool inLongName = false;

	if ( strncmp (option, PREFIX, PREFIX_LEN) != 0 )
		return false;

	lang = option + PREFIX_LEN;
	len = strlen (lang);

	if (len == 0)
		error (FATAL, "No language given in \"%s\" option", option);
	else if (len == strlen(RSV_LANG_ALL) && (strncmp(lang, RSV_LANG_ALL, len) == 0))
		language = LANG_AUTO;
	else
		language = getNamedLanguage (lang, len);

	if (language == LANG_IGNORE)
	{
		error (WARNING, "Unknown language: %s (ignoring \"--%s\")", lang, option);
		/* The option is consumed in this function. */
		return true;
	}

	initializeParser (language);

	if (*p == '*')
	{
		resetXtags (language, true);
		p++;
	}
	else if (*p == '{' || *p == '\0')
	{
		resetXtags (language, false);
		if (*p == '\0')
			return true;
	}
	else if (*p != '+' && *p != '-')
		error (WARNING, "Wrong per language extra specification: %s", p);

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
				error (FATAL,
				       "unexpected character in extra specification: \'%c\'",
				       c);
			inLongName = true;
			break;
		case '}':
			if (!inLongName)
				error (FATAL,
				       "unexpected character in extra specification: \'%c\'",
				       c);

			x = vStringValue (longName);
			enableLanguageXtag (language, x, mode);
			inLongName = false;
			vStringClear (longName);
			break;
		default:
			if (inLongName)
				vStringPut (longName, c);
			else
				error (FATAL,
				       "only long name can be used in per language extra spec: \'%c\'",
				       c);
			break;
		}
	}
	return true;
}

static bool processRegexOption (const char *const option,
								const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "regex-");
	if (language == LANG_IGNORE)
		return false;

	processLanguageRegexOption (language, REG_PARSER_SINGLE_LINE, parameter);

	return true;
}

static bool processMultilineRegexOption (const char *const option,
										 const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "mline-regex-");
	if (language == LANG_IGNORE)
		return false;

	processLanguageRegexOption (language, REG_PARSER_MULTI_LINE, parameter);

	return true;
}

static bool processMultitableRegexOption (const char *const option,
										  const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_mtable-regex-");
	if (language == LANG_IGNORE)
		return false;

	processLanguageRegexOption (language, REG_PARSER_MULTI_TABLE, parameter);

	return true;
}

static bool processMultitableExtendingOption (const char *const option,
											  const char *const parameter)
{
	langType language;

	language = getLanguageComponentInOption (option, "_mtable-extend-");
	if (language == LANG_IGNORE)
		return false;

	processLanguageMultitableExtendingOption (language, parameter);

	return true;
}

static void processLongOption (
		const char *const option, const char *const parameter)
{
	Assert (parameter != NULL);
	Assert (option != NULL);

	if (parameter [0] == '\0')
		verbose ("  Option: --%s\n", option);
	else
		verbose ("  Option: --%s=%s\n", option, parameter);

	if (processBooleanOption (option, parameter))
		;
	else if (processLangSpecificFieldsOption(option, parameter))
		 ;
	else if (processExtradefOption(option, parameter))
		;
	else if (processFielddefOption(option, parameter))
		;
	else if (processLangSpecificExtraOption(option, parameter))
		;
	else if (processParametricOption (option, parameter))
		;
	else if (processKinddefOption (option, parameter))
		;
	else if (processKindsOption (option, parameter))
		;
	else if (processAliasOption (option, parameter))
		;
	else if (processRegexOption (option, parameter))
		;
	else if (processMultilineRegexOption (option, parameter))
		;
	else if (processMapOption (option, parameter))
		;
	else if (processParamdefOption (option, parameter))
		;
	else if (processParamOption (option, parameter))
		;
	else if (processTabledefOption (option, parameter))
		;
	else if (processMultitableRegexOption (option, parameter))
		;
	else if (processMultitableExtendingOption (option, parameter))
		;
#ifdef HAVE_ICONV
	else if (processLanguageEncodingOption (option, parameter))
		;
#endif
#ifndef RECURSE_SUPPORTED
	else if (strcmp (option, "recurse") == 0)
		error (WARNING, "%s option not supported on this host", option);
#endif
	else if (processRoledefOption (option, parameter))
		;
	else if (processScopesepOption (option, parameter))
		;
	else if (processPreludeOption (option, parameter))
		;
	else if (processSequelOption (option, parameter))
		;
	else if (processMakeTagEntryReflectionOption (option, parameter))
		;
	else if (processPretendOption (option, parameter))
		;
	else if (processRolesOption (option, parameter))
		;
	else if (option[0] == '_' && option[1] == '_')
		/* ctags ignores an argument started from --__.
		 * optlib2c may handle the argument. */
		;
	else
		error (FATAL, "Unknown option: --%s", option);
}

static void processShortOption (
		const char *const option, const char *const parameter)
{
	if (parameter == NULL  ||  parameter [0] == '\0')
		verbose ("  Option: -%s\n", option);
	else
		verbose ("  Option: -%s %s\n", option, parameter);

	if (isCompoundOption (*option) && (parameter == NULL  ||  parameter [0] == '\0'))
		error (FATAL, "Missing parameter for \"%s\" option", option);
	else switch (*option)
	{
		case '?':
			processHelpOption ("?", NULL);
			exit (0);
			break;
		case 'a':
			checkOptionOrder (option, false);
			Option.append = true;
			break;
#ifdef DEBUG
		case 'b':
			if (atol (parameter) < 0)
				error (FATAL, "-%s: Invalid line number", option);
			Option.breakLine = atol (parameter);
			break;
		case 'd':
			if (!strToLong(parameter, 0, &ctags_debugLevel))
				error (FATAL, "-%s: Invalid debug level", option);

			if (debug (DEBUG_STATUS))
				ctags_verbose = true;
			break;
#endif
		case 'B':
			Option.backward = true;
			break;
		case 'D':
			processIgnoreOption (parameter, *option);
			break;
		case 'e':
			checkOptionOrder (option, false);
			setEtagsMode ();
			break;
		case 'f':
		case 'o':
			checkOptionOrder (option, false);
			if (Option.tagFileName != NULL)
			{
				error (WARNING,
					"-%s option specified more than once, last value used",
					option);
				freeString (&Option.tagFileName);
			}
			else if (parameter [0] == '-'  &&  parameter [1] != '\0')
				error (FATAL, "output file name may not begin with a '-'");
			Option.tagFileName = stringCopy (parameter);
			break;
		case 'F':
			Option.backward = false;
			break;
		case 'G':
			Option.guessLanguageEagerly = true;
			break;
		case 'h':
			processHeaderListOption (*option, parameter);
			break;
		case 'I':
			processIgnoreOption (parameter, *option);
			break;
		case 'L':
			if (Option.fileList != NULL)
			{
				error (WARNING,
					"-%s option specified more than once, last value used",
					option);
				freeString (&Option.fileList);
			}
			Option.fileList = stringCopy (parameter);
			break;
		case 'n':
			Option.locate = EX_LINENUM;
			break;
		case 'N':
			Option.locate = EX_PATTERN;
			break;
		case 'R':
#ifdef RECURSE_SUPPORTED
			Option.recurse = true;
#else
			error (WARNING, "-%s option not supported on this host", option);
#endif
			break;
		case 'u':
			checkOptionOrder (option, false);
			Option.sorted = SO_UNSORTED;
			break;
		case 'V':
			ctags_verbose = true;
			break;
		case 'w':
			/* silently ignored */
			break;
		case 'x':
			checkOptionOrder (option, false);
			setXrefMode ();
			break;
		default:
			error (FATAL, "Unknown option: -%s", option);
			break;
	}
}

static void parseOption (cookedArgs* const args)
{
	Assert (! cArgOff (args));
	if (args->isOption)
	{
		if (args->longOption)
			processLongOption (args->item, args->parameter);
		else
		{
			const char *parameter = args->parameter;
			while (*parameter == ' ')
				++parameter;
			processShortOption (args->item, parameter);
		}
		cArgForth (args);
	}
}

static void parseOptions (cookedArgs* const args)
{
	while (! cArgOff (args)  &&  cArgIsOption (args))
		parseOption (args);
	if (! cArgOff (args)  &&  ! cArgIsOption (args))
		NonOptionEncountered = true;
}

extern void parseCmdlineOptions (cookedArgs* const args)
{
	ENTER (Cmdline);
	parseOptions (args);
}

static bool checkSameFile (const char *const fileName, void * userData)
{
	return isSameFile ((const char* const) userData, fileName);
}

static bool parseFileOptions (const char* const fileName)
{
	bool fileFound = false;
	const char* const format = "Considering option file %s: %s\n";
	if (stringListHasTest (OptionFiles, checkSameFile, (void *) fileName))
	{
		verbose (format, fileName, "already considered");
		fileFound = true;
	}
	else
	{
		FILE* const fp = fopen (fileName, "r");
		if (fp == NULL)
			verbose (format, fileName, "not found");
		else
		{
			cookedArgs* const args = cArgNewFromLineFile (fp);
			vString* file = vStringNewInit (fileName);
			stringListAdd (OptionFiles, file);
			verbose (format, fileName, "reading...");
			parseOptions (args);
			if (NonOptionEncountered)
				error (WARNING, "Ignoring non-option in %s\n", fileName);
			cArgDelete (args);
			fclose (fp);
			fileFound = true;
		}
	}
	return fileFound;
}

/* Actions to be taken before reading any other options */
extern void previewFirstOption (cookedArgs* const args)
{
	while (cArgIsOption (args))
	{
		if (strcmp (args->item, "V") == 0
		    || strcmp (args->item, "verbose") == 0
		    || strcmp (args->item, "quiet") == 0
			/* Make some fundamental options work even
			 * if a bropen .ctags is given. */
			|| (strcmp (args->item, "version") == 0 &&
				(strcmp (args->parameter, RSV_NONE) == 0
				 ||  (*args->parameter == '\0')))
			|| strcmp (args->item, "help") == 0
			|| strcmp (args->item, "help-full") == 0
			|| strcmp (args->item, "license") == 0)
			parseOption (args);
		else if (strcmp (args->item, "options") == 0  &&
				strcmp (args->parameter, RSV_NONE) == 0)
		{
			notice ("No options will be read from files or environment");
			SkipConfiguration = true;
			cArgForth (args);
		}
		else
			break;
	}
}

#if defined (HAVE_DIRENT_H) || defined (_MSC_VER)
extern int scanDirectory (const char *directory_name,
						  struct dirent ***array_pointer, int (*select_function) (const struct dirent *),
						  int (*compare_function) (const struct dirent **, const struct dirent **));

static void parseConfigurationFileOptionsInDirectoryWithLeafname (const char* directory, const char* leafname)
{
	char* pathname = combinePathAndFile (directory, leafname);
	parseFileOptions (pathname);
	eFree (pathname);
}

static int ignore_dot_file(const struct dirent* dent)
{
	/* Ignore a file which name is started from dot. */
	if (*dent->d_name == '.')
		return 0;
	else
		return 1;
}

static int accept_only_dot_ctags(const struct dirent* dent)
{
	size_t len;

	/* accept only a file ended with ".ctags" */
	len = strlen(dent->d_name);

	if (len < 7)
		return 0;
	if (strcmp(dent->d_name + (len - 6), ".ctags") == 0)
		return 1;

	return 0;
}

static int alphaSort(const struct dirent ** a,
									  const struct dirent ** b)
{
	return strcmp ((*a)->d_name, (*b)->d_name);
}

static bool parseAllConfigurationFilesOptionsInDirectory (const char* const dirName,
							     stringList* const already_loaded_files)
{
	struct dirent **dents;
	int i, n;

	n = scanDirectory (dirName, &dents, ignore_dot_file, alphaSort);
	if (n < 0)
		return false;

	for (i = 0; i < n; i++)
	{
		char* path;
		fileStatus *s;

		if (already_loaded_files && stringListHas (already_loaded_files, dents[i]->d_name))
			continue;
		else if (already_loaded_files)
			stringListAdd (already_loaded_files, vStringNewInit (dents[i]->d_name));

		path = combinePathAndFile (dirName, dents[i]->d_name);
		s = eStat (path);

		if (s->exists && accept_only_dot_ctags(dents[i]))
			parseConfigurationFileOptionsInDirectoryWithLeafname (dirName,
																  dents[i]->d_name);
		eStatFree (s);
		free (dents[i]);
		eFree (path);
	}
	free (dents);
	return true;
}
#else
static bool parseAllConfigurationFilesOptionsInDirectory (const char* const dirName,
							     stringList* const already_loaded_files)
{
	return false;
}
#endif

typedef char* (* preloadMakePathFunc) (const char*, const char*);

struct preloadPathElt {
	const char *path;			/* NULL means the end of list */
	bool isDirectory;
	preloadMakePathFunc makePath;
	const char *extra;
	OptionLoadingStage stage;
};

static char* prependEnvvar (const char *path, const char* envvar)
{
	char *full_path = NULL;

	const char* const envval = getenv (envvar);
	if (envval && strlen (envval))
		full_path = combinePathAndFile(envval, path);

	return full_path;
}

static char *getConfigForXDG (const char *path CTAGS_ATTR_UNUSED,
							  const char* extra CTAGS_ATTR_UNUSED)
{
	char *r = prependEnvvar ("ctags", "XDG_CONFIG_HOME");
	if (r)
		return r;

	return prependEnvvar (".config/ctags", "HOME");
}

#ifdef _WIN32
static char *getConfigAtHomeOnWindows (const char *path,
									   const char* extra CTAGS_ATTR_UNUSED)
{
	/*
	 * Windows users don't usually set HOME.
	 * The OS sets HOMEDRIVE and HOMEPATH for them.
	 */
	const char* homeDrive = getenv ("HOMEDRIVE");
	const char* homePath = getenv ("HOMEPATH");
	if (homeDrive != NULL && homePath != NULL)
	{
		vString* const windowsHome = vStringNew ();
		vStringCatS (windowsHome, homeDrive);
		vStringCatS (windowsHome, homePath);

		char *tmp = vStringIsEmpty (windowsHome)
			? NULL
			: combinePathAndFile (vStringValue(windowsHome), path);

		vStringDelete (windowsHome);
		return tmp;
	}
	return NULL;
}
#endif

static void preload (struct preloadPathElt *pathList)
{
	unsigned int i;
	stringList* loaded;

	loaded = stringListNew ();
	for (i = 0; pathList[i].path != NULL || pathList[i].makePath != NULL; ++i)
	{
		struct preloadPathElt *elt = pathList + i;
		preloadMakePathFunc maker = elt->makePath;
		const char *path = elt->path;


		if (maker)
			path = maker(elt->path, elt->extra);

		if (path == NULL)
			continue;

		Assert (Stage <= elt->stage);
		if (Stage != elt->stage)
		{
			Stage = elt->stage;
			verbose ("Entering configuration stage: loading %s\n", StageDescription[Stage]);
		}

		if (elt->isDirectory)
			parseAllConfigurationFilesOptionsInDirectory (path, loaded);
		else
			parseFileOptions (path);

		if (path != elt->path)
			eFree ((void *)path);
	}
	stringListClear (loaded);
	stringListDelete (loaded);
}

static struct preloadPathElt preload_path_list [] = {
#ifdef CUSTOM_CONFIGURATION_FILE
	{
		.path = CUSTOM_CONFIGURATION_FILE,
		.isDirectory = false,
		.makePath = NULL,
		.stage = OptionLoadingStageCustom,
	},
#endif
	{
		.path = NULL,
		.isDirectory = true,
		.makePath = getConfigForXDG,
		.stage = OptionLoadingStageXdg,
	},
	{
		.path = ".ctags.d",
		.isDirectory = true,
		.makePath = prependEnvvar,
		.extra = "HOME",
		.stage = OptionLoadingStageHomeDir,
	},
#ifdef _WIN32
	{
		.path = "ctags.d",
		.isDirectory = true,
		.makePath = getConfigAtHomeOnWindows,
		.extra = NULL,
		.stage = OptionLoadingStageHomeDir,
	},
#endif
	{
		.path = ".ctags.d",
		.isDirectory = true,
		.makePath = NULL,
		.stage = OptionLoadingStageCurrentDir,
	},
	{
		.path = "ctags.d",
		.isDirectory = true,
		.makePath = NULL,
		.stage = OptionLoadingStageCurrentDir,
	},
	{
		.path = NULL,
		.makePath = NULL,
	},
};

static void parseConfigurationFileOptions (void)
{
	preload (preload_path_list);
}

extern void readOptionConfiguration (void)
{
	if (! SkipConfiguration)
		parseConfigurationFileOptions ();
}

static stringList* optlibPathListNew(struct preloadPathElt *pathList)
{
	stringList * appended = stringListNew ();

	for (size_t i = 0; pathList[i].path != NULL || pathList[i].makePath != NULL; ++i)
	{
		struct preloadPathElt *elt = pathList + i;
		preloadMakePathFunc maker = elt->makePath;
		const char *path = elt->path;

		if (!elt->isDirectory)
			continue;

		if (elt->stage == OptionLoadingStageCurrentDir)
			continue;

		if (maker)
			path = maker(elt->path, elt->extra);

		if (path == NULL)
			continue;

		vString *vpath;
		if (path == elt->path)
			vpath = vStringNewInit (path);
		else
			vpath = vStringNewOwn ((char *)path);
		stringListAdd(appended, vpath);
	}

	return appended;
}

/*
*   Option initialization
*/

extern void initOptions (void)
{
	OptionFiles = stringListNew ();
	OptlibPathList = optlibPathListNew (preload_path_list);

	verbose ("Setting option defaults\n");
	installHeaderListDefaults ();
	verbose ("  Installing default language mappings:\n");
	installLanguageMapDefaults ();
	verbose ("  Installing default language aliases:\n");
	installLanguageAliasesDefaults ();

	/* always excluded by default */
	verbose ("  Installing default exclude patterns:\n");
	processExcludeOption (NULL, "{arch}");
	processExcludeOption (NULL, ".arch-ids");
	processExcludeOption (NULL, ".arch-inventory");
	processExcludeOption (NULL, "autom4te.cache");
	processExcludeOption (NULL, "BitKeeper");
	processExcludeOption (NULL, ".bzr");
	processExcludeOption (NULL, ".bzrignore");
	processExcludeOption (NULL, "CVS");
	processExcludeOption (NULL, ".cvsignore");
	processExcludeOption (NULL, "_darcs");
	processExcludeOption (NULL, ".deps");
	processExcludeOption (NULL, ".dvi");
	processExcludeOption (NULL, ".DS_Store");
	processExcludeOption (NULL, "EIFGEN");
	processExcludeOption (NULL, ".git");
	processExcludeOption (NULL, ".gitignore");
	processExcludeOption (NULL, ".gitattributes");
	processExcludeOption (NULL, ".hg");
	processExcludeOption (NULL, ".hgignore");
	processExcludeOption (NULL, "PENDING");
	processExcludeOption (NULL, "RCS");
	processExcludeOption (NULL, "RESYNC");
	processExcludeOption (NULL, "SCCS");
	processExcludeOption (NULL, ".svn");
	processExcludeOption (NULL, "*~");
	processExcludeOption (NULL, ".*.swp");

	/* Exclude binary files
	 * -----------------------------------------------
	 *
	 * TODO
	 *
	 * It will be interesting if ctags can extract
	 * symbols from these binaries.
	 *
	 * --langdef=nm --regex-nm=...
	 * --langdef=elf --pre-processor-elf=/bin/nm ...
	 *
	 * vim/emacs users never wants the cursor to jump to
	 * a binary file but may wants to utilize the symbol
	 * information for completion.
	 *
	 * https://bitbucket.org/haypo/hachoir3 can be
	 * used the alternative for /bin/nm
	 */
	processExcludeOption (NULL, "*.o");
	processExcludeOption (NULL, "*.a");
	processExcludeOption (NULL, "*.so");

	processExcludeOption (NULL, "*.obj");
	processExcludeOption (NULL, "*.lib");
	processExcludeOption (NULL, "*.dll");
	processExcludeOption (NULL, "*.exe");

	processExcludeOption (NULL, "*.gcno");
	processExcludeOption (NULL, "*.gcda");

	processExcludeOption (NULL, "*.class");

	processExcludeOption (NULL, "*.pyc");
	processExcludeOption (NULL, "*.pyo");
}

extern void freeOptionResources (void)
{
	freeString (&Option.tagFileName);
	freeString (&Option.fileList);
	freeString (&Option.filterTerminator);

	freeList (&Excluded);
	freeList (&ExcludedException);
	freeList (&Option.headerExt);
	freeList (&Option.etagsInclude);

	freeSearchPathList (&OptlibPathList);

	freeList (&OptionFiles);
}

static void processDumpOptionsOption (const char *const option CTAGS_ATTR_UNUSED, const char *const parameter CTAGS_ATTR_UNUSED)
{
	fprintf(stdout, "# %s\n", "ParametricOptions");
	for (unsigned int i = 0; i < ARRAY_SIZE(ParametricOptions); i++)
		fprintf(stdout, "%s\n", ParametricOptions[i].name);

	fprintf(stdout, "# %s\n", "BooleanOptions");
	for (unsigned int i = 0; i < ARRAY_SIZE(BooleanOptions); i++)
		fprintf(stdout, "%s\n", BooleanOptions[i].name);
}

static void processDumpPreludeOption (const char *const option CTAGS_ATTR_UNUSED, const char *const parameter CTAGS_ATTR_UNUSED)
{
	extern const char ctagsCommonPrelude[];
	fprintf(stdout, "%s\n", ctagsCommonPrelude);
	exit (0);
}

extern bool inSandbox (void)
{
	return (Option.interactive == INTERACTIVE_SANDBOX);
}

extern bool canUseLineNumberAsLocator (void)
{
	return (Option.locate != EX_PATTERN);
}

extern bool isDestinationStdout (void)
{
	bool toStdout = false;

	if (Option.filter || Option.interactive ||
		(Option.tagFileName != NULL  &&  (strcmp (Option.tagFileName, "-") == 0
						  || strcmp (Option.tagFileName, "/dev/stdout") == 0
		)))
		toStdout = true;
	else if (Option.tagFileName == NULL && NULL == outputDefaultFileName ())
		toStdout = true;

	return toStdout;
}
