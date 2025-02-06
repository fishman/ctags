#	Copyright (c) 2009, Darren Hiebert
#
#	This source code is released for free distribution under the terms
#	of the GNU General Public License version 2 or (at your option) any
#	later version.

#	Process this file with autoconf to produce a configure script.

AC_PREREQ([2.64])

dnl You must update PROGRAM_VERSION in main/ctags.h, too.
dnl --version option of readtags also prints this.
AC_INIT([universal-ctags],[6.1.0])

if ! test -e "${srcdir}/config.h.in"; then
   echo "---"
   echo "--- ${srcdir}/config.h.in WAS NOT FOUND." 1>&2
   echo "--- YOU MIGHT HAVE RUN autoconf ONLY" 1>&2
   echo "--- BUT YOU MAY HAVE TO RUN autogen.sh." 1>&2
   echo "---"
   exit 1
fi

# Report system info
# ------------------
program_name=[`grep 'PROGRAM_NAME  *"' $srcdir/main/ctags.h | sed -e 's/.*"\([^"]*\)".*/\1/'`]
program_version=[`grep 'PROGRAM_VERSION  *"' $srcdir/main/ctags.h | sed -e 's/.*"\([^"]*\)".*/\1/'`]
echo "$program_name, version $program_version"
uname -mrsv 2>/dev/null

AM_INIT_AUTOMAKE([foreign subdir-objects tar-ustar])
AM_SILENT_RULES([yes])
AC_CONFIG_HEADERS([config.h])

m4_ifndef([PKG_CHECK_EXISTS], [m4_fatal([must install pkg-config 0.18 or later before running ./autogen.sh])])

AC_CANONICAL_BUILD
AC_CANONICAL_HOST

case $host in
    *darwin*)
        if test "${enable_static}" = "yes"; then
            printf "\n%s\n" "macOS not support --enable-static." >&5
            printf "\n%s\n" "macOS not support --enable-static." >&6
            exit 1
        fi
        ;;
    *freebsd*|*openbsd*)
        if test "$cross_compiling" = "no" ; then
            # https://github.com/universal-ctags/ctags/issues/3338
            export LDFLAGS="$LDFLAGS -L/usr/local/lib"
        fi
        ;;
esac

AH_TEMPLATE([PACKAGE], [Package name.])
AH_TEMPLATE([VERSION], [Package version.])
AH_TEMPLATE([clock_t],
	[Define to the appropriate type if <time.h> does not define this.])
AH_TEMPLATE([fpos_t],
	[Define to long if <stdio.h> does not define this.])
AH_TEMPLATE([L_tmpnam],
	[Define to the appropriate size for tmpnam() if <stdio.h> does not define
	this.])
AH_TEMPLATE([HAVE_STAT_ST_INO],
	[Define this macro if the field "st_ino" exists in struct stat in
	<sys/stat.h>.])
AH_TEMPLATE([HAVE_STATEMENT_EXPRESSION_EXT],
	[Define this macro if compiler supports statement expression non-standard
	C feature.])
AH_TEMPLATE([remove],
	[Define remove to unlink if you have unlink(), but not remove().])
AH_TEMPLATE([INT_MAX],
	[Define as the maximum integer on your system if not defined <limits.h>.])
AH_TEMPLATE([CUSTOM_CONFIGURATION_FILE],
	[You can define this label to be a string containing the name of a
	site-specific configuration file containing site-wide default options. The
	files /etc/ctags.conf and /usr/local/etc/ctags.conf are already checked,
	so only define one here if you need a file somewhere else.])
AH_TEMPLATE([ETAGS],
	[Define the string to check (in executable name) for etags mode])
AH_TEMPLATE([MACROS_USE_PATTERNS],
	[Define this label if you want macro tags (defined lables) to use patterns
	in the EX command by default (original ctags behavior is to use line
	numbers).])
AH_VERBATIM([DEFAULT_FILE_FORMAT], [
/* Define this as desired.
 * 1:  Original ctags format
 * 2:  Extended ctags format with extension flags in EX-style comment.
 */
#define DEFAULT_FILE_FORMAT	2
])
AH_TEMPLATE([CASE_INSENSITIVE_FILENAMES],
	[Define this label if your system uses case-insensitive file names])
AH_VERBATIM([EXTERNAL_SORT], [
/* Define this label to use the system sort utility (which is probably more
*  efficient) over the internal sorting algorithm.
*/
#ifndef INTERNAL_SORT
# undef EXTERNAL_SORT
#endif
])
AH_TEMPLATE([HAVE_ICONV],
	[Define this value if support multibyte character encoding.])
AH_TEMPLATE([ICONV_USE_LIB_PREFIX],
	[Define this value if the platform uses "lib" as prefix for iconv functions.])
AH_TEMPLATE([TMPDIR],
	[If you wish to change the directory in which temporary files are stored,
	define this label to the directory desired.])
AH_TEMPLATE([NON_CONST_PUTENV_PROTOTYPE],
	[Define this is you have a prototype for putenv() in <stdlib.h>, but
	doesn't declare its argument as "const char *".])
AH_TEMPLATE([MSDOS_STYLE_PATH],
	[Define to 1 if your system uses MS-DOS style path.])
AH_TEMPLATE([MANUAL_GLOBBING],
	[Define to 1 if your system doesn't expand wildcards.])
AH_TEMPLATE([HAVE__FINDFIRST],
	[Define to 1 if your system have _findfirst().])
AH_TEMPLATE([ENABLE_GCOV],
	[Define to 1 if gcov is instrumented.])
AH_TEMPLATE([USE_GNULIB_FNMATCH],
	[Define to 1 when using fnmatch implementation in bundled gnulib.])

# Define convenience macros
# -------------------------
# CHECK_HEADER_DEFINE(LABEL, HEADER [,ACTION-IF-FOUND [, ACTION-IF-NOT-FOUND ] ])
AC_DEFUN([CHECK_HEADER_DEFINE], [
	AC_MSG_CHECKING([if $1 is defined in $2])
	AC_EGREP_CPP([$2:$1],
[
#include <$2>
#ifdef $1
const char *result_yes = "$2:$1";
#endif
], [
	AC_MSG_RESULT(yes)
	[$3]
], [
	AC_MSG_RESULT(no)
	[$4]
]) ])

# CHECK_PROTO(FUNCTION, HEADER-FILE)
AC_DEFUN([CHECK_PROTO], [
	AC_EGREP_HEADER([[^A-Za-z0-9_]$1([ 	]+[A-Za-z0-9_]*)?[	 ]*\(],
	[$2],
	[],
	[
		AC_MSG_NOTICE([adding prototype for $1])
		AH_TEMPLATE(AS_TR_CPP([NEED_PROTO_$1]),
			    [If you receive error or warning messages indicating that you are missing a prototype for, or a type mismatch using, the following function, define this label and remake.])
		AC_DEFINE(AS_TR_CPP([NEED_PROTO_$1]))
	])
])

# PRETTY_ARG_VAR(VARIABLE, DESCRIPTION, DEFAULT)
# ----------------------------------------------
# Call AC_ARG_VAR with DEFAULT value.
AC_DEFUN([PRETTY_ARG_VAR], [
	AC_ARG_VAR([$1], [$2 [$3]])

	if test "${ac_env_$1_set}" = "set"; then
		$1="${ac_env_$1_value}"
	else
		$1="$3"
	fi
])

# Checks for configuration options
# --------------------------------

AC_ARG_WITH([sparse-cgcc],
	[AS_HELP_STRING([--with-sparse-cgcc],
		[use Sparse 'compiler wrapper' cgcc as C compiler [no]])])

AC_ARG_ENABLE([coverage-gcov],
	[AS_HELP_STRING([--enable-coverage-gcov],
		[enable 'gcov' coverage testing tool [no]])])

AC_ARG_ENABLE(readcmd,
	[AS_HELP_STRING([--disable-readcmd],
		[do not include readtags command during install])],
	[], [enable_readcmd=yes])

AC_ARG_ENABLE(etags,
	[AS_HELP_STRING([--enable-etags],
		[enable the installation of links for etags])])

AC_ARG_ENABLE(extended-format,
	[AS_HELP_STRING([--disable-extended-format],
		[disable extension flags; use original ctags file format only])],
	AC_DEFINE(DEFAULT_FILE_FORMAT, 1), AC_DEFINE(DEFAULT_FILE_FORMAT, 2))

AC_ARG_ENABLE(external-sort,
	[AS_HELP_STRING([--disable-external-sort],
		[use internal sort algorithm instead of sort program])])

AC_ARG_ENABLE(iconv,
	[AS_HELP_STRING([--disable-iconv],
		[disable multibyte character encoding support])])

AC_ARG_ENABLE(custom-config,
	[AS_HELP_STRING([--enable-custom-config=FILE],
		[enable custom config file for site-wide defaults])])

AC_ARG_ENABLE(macro-patterns,
	[AS_HELP_STRING([--enable-macro-patterns],
		[use patterns as default method to locate macros instead of line numbers])])

AC_ARG_ENABLE(tmpdir,
	[AS_HELP_STRING([--enable-tmpdir=DIR],
		[default directory for temporary files [ARG=/tmp]])],
	tmpdir_specified=yes)

AC_ARG_ENABLE([debugging],
	[AS_HELP_STRING([--enable-debugging],
		[enable debugging features])])

AC_ARG_ENABLE([static],
	[AS_HELP_STRING([--enable-static],
		[enable static linking (mainly for MinGW)])])

AC_ARG_ENABLE([lto],
	[AS_HELP_STRING([--enable-lto],
		[enable link time optimization])])

AC_ARG_PROGRAM

# Process configuration options
# -----------------------------

AM_CONDITIONAL(INSTALL_ETAGS, [test "x$enable_etags" = "xyes"])
AM_CONDITIONAL(USE_READCMD, [test "x$enable_readcmd" = "xyes"])

dnl AC_MSG_NOTICE(Change with $program_transform_name)
CTAGS_NAME_EXECUTABLE=`echo ctags | sed "$program_transform_name"`
AC_SUBST(CTAGS_NAME_EXECUTABLE)
ETAGS_NAME_EXECUTABLE=`echo etags | sed "$program_transform_name"`
AC_SUBST(ETAGS_NAME_EXECUTABLE)

AC_DEFINE_UNQUOTED(ETAGS, "$ETAGS_NAME_EXECUTABLE")

if test "$enable_custom_config" = no -o "$enable_custom_config" = yes ; then
	AC_MSG_NOTICE(no name supplied for custom configuration file)
elif test -n "$enable_custom_config" ; then
	AC_DEFINE_UNQUOTED(CUSTOM_CONFIGURATION_FILE, "$enable_custom_config")
	AC_MSG_NOTICE($enable_custom_config will be used as custom configuration file)
fi

if test "$enable_macro_patterns" = yes ; then
	AC_DEFINE(MACROS_USE_PATTERNS)
	AC_MSG_NOTICE(tag file will use patterns for macros by default)
fi

AM_CONDITIONAL(ENABLE_DEBUGGING, [test "x$enable_debugging" = "xyes"])


# Checks for programs
# -------------------

AC_PROG_CC
AC_PROG_CC_C99
AC_C_INLINE
# For gnulib.
#  Typically, this is immediately after AC_PROG_CC, ...
gl_EARLY

AC_ARG_VAR(RANLIB,[ranlib command or path])
AC_PROG_RANLIB
PRETTY_ARG_VAR([AR], [ar command or path], [ar])
AC_C_BIGENDIAN

if test "$cross_compiling" = "yes"; then
    # We need to compile and run a program on the build machine.
    AC_MSG_CHECKING(for cc for build)
    if test "x${CC_FOR_BUILD}" = "x" ; then
        CC_FOR_BUILD=cc
    fi
    AC_MSG_RESULT($CC_FOR_BUILD)

    AC_MSG_CHECKING(if $CC_FOR_BUILD works)

    unset TEMP_DIR_FOR_CHECKING
    TEMP_DIR_FOR_CHECKING=$(mktemp -d)

    unset CC_FOR_BUILD_WORKS
    CC_FOR_BUILD_WORKS=no

    (
        cd "$TEMP_DIR_FOR_CHECKING" || return 1

        printf 'int main() { return 0;}' > test.c

        $CC_FOR_BUILD $CPPFLAGS_FOR_BUILD $CFLAGS_FOR_BUILD $LDFLAGS_FOR_BUILD -o test test.c
    ) && CC_FOR_BUILD_WORKS=yes

    AC_MSG_RESULT($CC_FOR_BUILD_WORKS)

    unset BUILD_EXEEXT
    unset BUILD_OBJEXT

    BUILD_OBJEXT='o'

    if test -f "$TEMP_DIR_FOR_CHECKING/test.exe" ; then
        BUILD_EXEEXT='.exe'
    fi
else
    CC_FOR_BUILD="$CC"
    CFLAGS_FOR_BUILD="$CFLAGS"
    CPPFLAGS_FOR_BUILD="$CPPFLAGS"
    LDFLAGS_FOR_BUILD="$LDFLAGS"

    BUILD_OBJEXT="$OBJEXT"
    BUILD_EXEEXT="$EXEEXT"
fi

AC_ARG_VAR(CC_FOR_BUILD,[build system C compiler])
AC_ARG_VAR(CFLAGS_FOR_BUILD,[CFLAGS for build system C compiler])
AC_ARG_VAR(CPPFLAGS_FOR_BUILD,[CPPFLAGS for build system C compiler])
AC_ARG_VAR(LDFLAGS_FOR_BUILD,[LDFLAGS for build system C compiler])

AC_SUBST([CC_FOR_BUILD])
AC_SUBST([CFLAGS_FOR_BUILD])
AC_SUBST([CPPFLAGS_FOR_BUILD])
AC_SUBST([LDFLAGS_FOR_BUILD])

AC_SUBST([BUILD_OBJEXT])
AC_SUBST([BUILD_EXEEXT])

AC_ARG_VAR(WINDRES,[windres command or path, used with mingw-w64])
AC_SUBST([WINDRES])

if test "x${WINDRES}" = "x" ; then
    WINDRES=windres
fi

if test "${with_sparse_cgcc}" = "yes"; then
	REAL_CC="${CC}"
	CC="cgcc"
	AC_SUBST([REAL_CC])

	PRETTY_ARG_VAR([CGCC_CFLAGS], [cgcc specific flags],
		       [-Wsparse-all])
else
	CGCC_CFLAGS=""
fi

if test "${enable_coverage_gcov}" = "yes"; then
	COVERAGE_CFLAGS="--coverage"
	COVERAGE_LDFLAGS="--coverage"
	AC_DEFINE(ENABLE_GCOV)
fi
AC_SUBST([COVERAGE_CFLAGS])
AC_SUBST([COVERAGE_LDFLAGS])

AC_PROG_LN_S
AC_CHECK_PROG(STRIP, strip, strip, :)
AC_SYS_LARGEFILE

AC_CHECK_PROG([perl_found], [perl], [yes], [no])
AM_CONDITIONAL([RUN_OPTLIB2C], [test "${perl_found}" = "yes"])
AM_CONDITIONAL([RUN_TXT2CSTR], [test "${perl_found}" = "yes"])

# rst2man and rst2html are part of python-docutils, and if not found
# 	can be installed using pip ("pip install docutils"). On some
#	systems, rst2man and rst2html are actually installed as rst2man.py
#	and rst2html.py - create a symlink of that's the case.
#	Also, allow to define path as "--with-rst2man" and "--with-rst2html".
AC_ARG_WITH([rst2man],
  AS_HELP_STRING([--with-rst2man=PATH], [Location of rst2man (auto)]),
  [RST2MAN="$withval"],
  [AC_PATH_PROGS(RST2MAN,
    [rst2man rst2man.py rst2man-3 rst2man-3.6 rst2man-3.7 rst2man-3.8 rst2man-3.9, rst2man-3.10],
    [no])])
AM_CONDITIONAL([HAVE_RST2MAN], [test "x$RST2MAN" != "xno"])
AC_ARG_WITH([rst2html],
  AS_HELP_STRING([--with-rst2html=PATH], [Location of rst2html (auto)]),
  [RST2HTML="$withval"],
  [AC_PATH_PROGS(RST2HTML,
    [rst2html rst2html.py rst2html-3 rst2html-3.6 rst2html-3.7 rst2html-3.8 rst2html-3.9, rst2html-3.10],
    [no])])
AM_CONDITIONAL([HAVE_RST2HTML], [test "x$RST2HTML" != "xno"])

# rst2pdf is a separate tool and can also be installed via pip (e.g.,
# 	"pip install rst2pdf")
AC_PATH_PROGS(RST2PDF, [rst2pdf], [no])
AM_CONDITIONAL([HAVE_RST2PDF], [test "x$RST2PDF" != "xno"])

# rst2man had a bug about code-block:: handling.
# https://sourceforge.net/p/docutils/patches/141
RST2MAN_OPTIONS=
if ! test "x$RST2MAN" = "xno"; then
   RST2MAN_SUPPORTING_SYNTAX_HIGHLIGHT_OPTION=no
   AC_MSG_CHECKING(whether rst2mn has --syntax-highlight option)
   if $RST2MAN --help | grep -q -e --syntax-highlight; then
      RST2MAN_SUPPORTING_SYNTAX_HIGHLIGHT_OPTION=yes
      dnl See man/Makefile about the option
      RST2MAN_OPTIONS=--syntax-highlight=none
   fi
   AC_MSG_RESULT($RST2MAN_SUPPORTING_SYNTAX_HIGHLIGHT_OPTION)
fi
AC_SUBST(RST2MAN_OPTIONS)
AM_CONDITIONAL([HAS_GNU_SED], [sed --version 2>/dev/null | grep -q GNU])

AC_ARG_WITH([pegof],
  AS_HELP_STRING([--with-pegof=PATH], [Location of pegof (auto)]),
  [PEGOF="$withval"],
  [AC_PATH_PROGS(PEGOF,
    [pegof],
    [no])])
AM_CONDITIONAL([HAVE_PEGOF], [test "x$PEGOF" != "xno"])

# Checks for operating environment
# --------------------------------
in_git_repo=no
AC_MSG_CHECKING(building in a git repository)
if test -f "${srcdir}/.git/HEAD" || test -f "${srcdir}/.git"; then
	in_git_repo=yes
fi
AC_MSG_RESULT(${in_git_repo})

AC_CHECK_PROGS([GIT], [git])
if ! test "${GIT:+set}" = "set"; then
	in_git_repo=no
fi
AM_CONDITIONAL([BUILD_IN_GIT_REPO], [test "x${in_git_repo}" = "xyes"])

have_timeout=no
AC_CHECK_PROGS([TESTING_TIMEOUT], [timeout])
if test "${TESTING_TIMEOUT:+set}" = "set"; then
	have_timeout=yes
fi
AM_CONDITIONAL([HAVE_TIMEOUT], [test "x${have_timeout}" = "xyes"])

AC_CHECK_PROGS([PYTHON],
			   [python3 python3.8 python3.7 python3.6 python3.5 python], [])


# Check for temporary directory
AC_MSG_CHECKING(directory to use for temporary files)
if test -n "$enable_tmpdir"; then
	tmpdir="$enable_tmpdir"
elif test -n "$TMPDIR"; then
	tmpdir="$TMPDIR"
elif test -n "$TMP"; then
	tmpdir="$TMP"
elif test -n "$TEMP"; then
	tmpdir="$TEMP"
elif test -d "c:/"; then
	tmpdir="c:/"
else
	tmpdir="/tmp"
fi

if test "$cross_compiling" = yes ; then
        AC_MSG_RESULT($tmpdir)
        AC_DEFINE_UNQUOTED(TMPDIR, "$tmpdir")
else
    if test -d $tmpdir ; then
        AC_MSG_RESULT($tmpdir)
        AC_DEFINE_UNQUOTED(TMPDIR, "$tmpdir")
    else
        AC_MSG_ERROR($tmpdir does not exist)
    fi
fi

# Test for case-insensitive filenames
AC_MSG_CHECKING(for case-insensitive filenames)
touch conftest.cif
if test -f CONFTEST.CIF; then
	AC_MSG_RESULT(yes)
	AC_DEFINE(CASE_INSENSITIVE_FILENAMES)
else
	AC_MSG_RESULT(no)
fi
rm -f conftest.cif

AC_MSG_CHECKING(selected sort method)
if test no = "$enable_external_sort"; then
	AC_MSG_RESULT(simple internal algorithm)
else
	AC_MSG_RESULT(external sort utility)
	enable_external_sort=no
    AC_CHECK_PROG(sort_found, sort, yes, no)
	if test "$sort_found" = yes ; then
		AC_MSG_CHECKING(if sort accepts our command line)
		touch ${tmpdir}/sort.test
		sort -u -f -o ${tmpdir}/sort.test ${tmpdir}/sort.test 1>/dev/null 2>&1
		if test $? -ne 0 ; then
			AC_MSG_RESULT(no)
		else
			AC_MSG_RESULT(yes)
			AC_DEFINE(EXTERNAL_SORT)
			enable_external_sort=yes
		fi
		rm -f ${tmpdir}/sort.test
    fi
fi
if test "$enable_external_sort" != yes ; then
	AC_MSG_NOTICE(using internal sort algorithm as fallback)
fi


# Checks for header files
# -----------------------

AC_CHECK_HEADERS([direct.h dirent.h fcntl.h io.h stat.h types.h unistd.h])
AC_CHECK_HEADERS([sys/dir.h sys/stat.h sys/types.h sys/wait.h])

# Checks for header file macros
# -----------------------------

CHECK_HEADER_DEFINE(L_tmpnam, [stdio.h],, AC_DEFINE(L_tmpnam, 20))


# Checks for typedefs and types
# -----------------------------

AC_TYPE_OFF_T

AC_CHECK_HEADERS([stdbool.h])

# Checks for compiler characteristics
# -----------------------------------

AC_C_CONST
AC_OBJEXT
AC_EXEEXT
AC_C_TYPEOF

if test "${enable_static}" = "yes"; then
	LDFLAGS="$LDFLAGS -static"
fi

# Check for host type
case "$host" in
  i?86-*-mingw* | x86_64-*-mingw*)
	host_mingw=yes
	# See https://github.com/universal-ctags/ctags/pull/3069
	gl_cv_have_weak=no
	AC_DEFINE(MSDOS_STYLE_PATH)
	AC_DEFINE(MANUAL_GLOBBING)
	AC_DEFINE(HAVE__FINDFIRST)
	;;
  *-cygwin | *-msys)
	gl_cv_have_weak=no
	;;
esac
AM_CONDITIONAL([HOST_MINGW], [test "x${host_mingw}" = "xyes"])

AC_MSG_CHECKING(if compiler supports statement expressions)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],[
int main(int argc, char **argv) {return ({ int x = 1; x + argc;});}
])],[have_statement_expression_ext=yes],[have_statement_expression_ext=no])
AC_MSG_RESULT($have_statement_expression_ext)
if test yes = "$have_statement_expression_ext"; then
	AC_DEFINE(HAVE_STATEMENT_EXPRESSION_EXT)
fi


# Check if struct stat contains st_ino.
# MinGW has st_ino, but it doesn't work.
if test yes != "$host_mingw"; then
	AC_MSG_CHECKING(if struct stat contains st_ino)
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
		#include <sys/stat.h>
		#include <stdlib.h>
		], [
		struct stat st;
		stat(".", &st);
		if (st.st_ino > 0)
			exit(0);
	])],[have_st_ino=yes],[have_st_ino=no])
	AC_MSG_RESULT($have_st_ino)
	if test yes = "$have_st_ino"; then
		AC_DEFINE(HAVE_STAT_ST_INO)
	fi
fi

PRETTY_ARG_VAR([EXTRA_CPPFLAGS], [extra (Objective) C/C++ preprocessor flags],
	       [-D_GNU_SOURCE -D__USE_GNU])
PRETTY_ARG_VAR([DEBUG_CPPFLAGS], [(Objective) C/C++ preprocessor debug flags],
	       [-DDEBUG])

PRETTY_ARG_VAR([EXTRA_CFLAGS], [extra C compiler flags],
	       [-std=gnu99])
PRETTY_ARG_VAR([WARNING_CFLAGS], [C compiler warning flags],
	       [-Wall])

PRETTY_ARG_VAR([EXTRA_LDFLAGS], [extra linker flags],
	       [])

dnl ref. https://stackoverflow.com/questions/40374061/autoconf-recipe-to-use-gcc-ar-and-gcc-ranlib
use_lto=no

AS_IF([test "x$cross_compiling" != "xyes" -a "x$enable_lto" = "xyes"],[
		AX_CHECK_COMPILE_FLAG(-flto,[use_lto=yes])])
AS_VAR_IF([use_lto], [yes], [
	AS_CASE($CC,
		[gcc|*/gcc],     [AC_CHECK_PROGS([LTO_AR], [gcc-ar ar],   [:])],
		[clang|*/clang], [AC_CHECK_PROGS([LTO_AR], [clang-ar ar], [:])],
		[use_lto=no])
	AC_MSG_CHECKING([the wrapper for ar working with $CC -flto])
	AS_VAR_IF([LTO_AR], [:], AC_MSG_ERROR([could not find AR tool working with -flto.]))
	AS_VAR_IF([use_lto], [yes],
		[AS_CASE($LTO_AR,
			[*-ar],[AC_MSG_RESULT([$LTO_AR])],
			[use_lto=no
			 AC_MSG_RESULT([no suitable AR tool is found; truning off LTO])])],
		[AC_MSG_RESULT([no suitable AR tool is found; truning off LTO])])
])
AS_VAR_IF([use_lto], [yes], [
	AS_CASE($CC,
		[gcc|*/gcc],     [AC_CHECK_PROGS([LTO_RANLIB], [gcc-ranlib ranlib],  [:])],
		[clang|*/clang], [AC_CHECK_PROGS([LTO_RANLIB], [clang-ranlib ranlib], [:])],
		[use_lto=no])
	AC_MSG_CHECKING([the wrapper for ranlib working with $CC -flto])
	AS_VAR_IF([LTO_RANLIB], [:], AC_MSG_ERROR([could not find RANLIB tool working with -flto.]))
	AS_VAR_IF([use_lto], [yes],
		[AS_CASE($LTO_RANLIB,
			[*-ranlib],[AC_MSG_RESULT([$LTO_RANLIB])],
			[use_lto=no
			 AC_MSG_RESULT([no suitable ranlib tool is found; truning off LTO])])],
		[AC_MSG_RESULT([no suitable ranlib is found; truning off LTO])])
])
AS_VAR_IF([use_lto], [yes], [
	AR="$LTO_AR"
	RANLIB="$LTO_RANLIB"
	EXTRA_CFLAGS="$EXTRA_CFLAGS -flto"
	EXTRA_LDFLAGS="$EXTRA_LDFLAGS -flto"
],[
	AS_VAR_IF([enable_lto], [yes], [
		AC_MSG_ERROR([though --enable-lto is specified, the fto feature is not available nor usable.])
	])
])

# Checks for function availability
# -----------------------------------

# For gnulib.
#   Place it further down in the file, typically where you normally check for
#   header files or functions.
gl_INIT

if test "$REPLACE_FNMATCH" != 0; then
	AC_DEFINE([USE_GNULIB_FNMATCH])
fi

AC_CHECK_FUNCS(asprintf)
AC_CHECK_FUNCS(strcasecmp stricmp, break)
AC_CHECK_FUNCS(strncasecmp strnicmp, break)

AC_CHECK_FUNCS(mkstemp, have_mkstemp=yes)
if test "$have_mkstemp" != yes ; then
	AC_CHECK_FUNCS(tempnam, have_tempnam=yes)
fi
if test "$have_mkstemp" != yes -a "$have_tempnam" != yes; then
	AC_CHECK_FUNCS(chmod)
	if test "$tmpdir_specified" = yes ; then
		AC_MSG_NOTICE(use of tmpnam overrides temporary directory selection)
	fi
fi

AC_CHECK_FUNCS(opendir findfirst _findfirst, break)
AC_CHECK_FUNCS(strerror strsignal)

AC_CHECK_FUNCS(truncate, have_truncate=yes)
# === Cannot nest AC_CHECK_FUNCS() calls
if test "$have_truncate" != yes  ; then
	AC_CHECK_FUNCS(ftruncate, have_ftruncate=yes)
	if test "$have_ftruncate" != yes ; then
		AC_CHECK_FUNCS(chsize)
	fi
fi

AC_CHECK_FUNCS(setenv, have_setenv=yes)
# === Cannot nest AC_CHECK_FUNCS() calls
if test "$have_setenv" != yes ; then
	AC_CHECK_FUNCS(putenv, have_putenv=yes)
	if test "$have_putenv" = yes ; then
		AC_EGREP_HEADER(putenv, stdlib.h, have_putenv_prototype=yes)
		if test "$have_putenv_prototype" = yes ; then
			AC_MSG_CHECKING(putenv prototype)
			AC_EGREP_HEADER([[^A-Za-zo-9_]putenv[ 	]*\(.*const.*\)[ 	]*;],
				stdlib.h, AC_MSG_RESULT(correct),
				[
					AC_MSG_RESULT(no const)
					AC_DEFINE(NON_CONST_PUTENV_PROTOTYPE)
				])
		fi
	fi
fi

have_scandir=no
AC_CHECK_FUNCS(scandir,have_scandir=yes)

have_dirent_h=no
AC_CHECK_HEADERS(dirent.h,have_dirent_h=yes)

dnl Dummy check for setting $PKG_CONFIG.
PKG_CHECK_EXISTS([dummy])
if test "${enable_static}" = "yes"; then
	PKG_CONFIG="$PKG_CONFIG --static"
fi

AC_ARG_ENABLE([xml],
	[AS_HELP_STRING([--disable-xml],
		[disable xml support])])

AH_TEMPLATE([HAVE_LIBXML],
	[Define this value if libxml is available.])
dnl About the condition of version
dnl see https://mail.gnome.org/archives/xml/2010-February/msg00008.html
AS_IF([test "x$enable_xml" != "xno"], [
	PKG_CHECK_MODULES(LIBXML, [libxml-2.0 >= 2.7.7],
				[AC_MSG_CHECKING([libxml-2.0 CRLF handling bug])
				 ORIGINAL_CFLAGS=$CFLAGS
				 ORIGINAL_LIBS=$LIBS
				 CFLAGS="$CFLAGS $LIBXML_CFLAGS"
				 LIBS="$LIBS $LIBXML_LIBS"
				 AC_RUN_IFELSE([AC_LANG_SOURCE([[
				 #include <libxml/parser.h>
				 #include <libxml/tree.h>
				 #include <string.h>

				 #define CRLF "\r\n"
				 #define __LF  " \n"

				 const char * crlf = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?><root><!---->"CRLF CRLF"<target/></root>";
				 const char * __lf = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?><root><!---->"__LF __LF"<target/></root>";

				 static int run(const char *input)
				 {
					xmlDocPtr doc = xmlParseMemory(input, strlen(input));
					xmlNode *root = xmlDocGetRootElement(doc);
					xmlNode *node = root->children->next->next;
					return  node->line;
				 }

				 int main(void)
				 {
					xmlLineNumbersDefault (1);
					return run(crlf) == run(__lf)? 0: 1;
				 }]])],[AC_MSG_RESULT([good])
					have_libxml=yes
					CFLAGS=$ORIGINAL_CFLAGS
					LIBS=$ORIGINAL_LIBS
					AC_DEFINE(HAVE_LIBXML)],
				    [AC_MSG_RESULT([bad])
				    AC_MSG_WARN([Xpath feature is disabled because the way of handling CRLF in libxml2 is broken.
See https://gitlab.gnome.org/GNOME/libxml2/-/commit/43b511fa714df875dc4f40d108061eede0d4d76b])],
				    dnl In a context of cross-compiling: We can not do try-run.
				    dnl Instead, we check the version number only.
				    [PKG_CHECK_MODULES(LIBXML12d0, [libxml-2.0 = 2.12.0],
								   [AC_MSG_WARN([Xpath feature is disabled because the way of handling CRLF in libxml2 2.12.0 may be broken.
See https://gitlab.gnome.org/GNOME/libxml2/-/commit/43b511fa714df875dc4f40d108061eede0d4d76b])
								    have_libxml=no],[
				     PKG_CHECK_MODULES(LIBXML12d1, [libxml-2.0 = 2.12.1],
								   [AC_MSG_WARN([Xpath feature is disabled because the way of handling CRLF in libxml2 2.12.1 may be broken.
See https://gitlab.gnome.org/GNOME/libxml2/-/commit/43b511fa714df875dc4f40d108061eede0d4d76b])
								    have_libxml=no],
				     dnl The available libxml-2.0 is not 2.12.[01].
								   [have_libxml=yes
								    AC_DEFINE(HAVE_LIBXML)
								   ])])]
				)],
				[AS_IF([test "x$enable_xml" = "xyes"], [
				   AC_MSG_ERROR([libxml2 not found])])])
])

AM_CONDITIONAL(HAVE_LIBXML, test "x$have_libxml" = xyes)

if test "${enable_static}" = "yes"; then
	if test "${have_libxml}" = "yes"; then
		if test "${host_mingw}" = "yes"; then
			dnl -DLIBXML_STATIC needs to be added manually.
			LIBXML_CFLAGS="$LIBXML_CFLAGS -DLIBXML_STATIC"
		fi
	fi
fi

dnl https://stackoverflow.com/questions/40798571/autoconf-detect-deprecated-functions-in-glibc
AH_TEMPLATE([IS_xmlLineNumbersDefault_DEPRECATED],
	[Define this value if xmlLineNumbersDefault is marked as a deprecated function.])
AS_VAR_IF([have_libxml], [yes], [
	CC_CHECK_WERROR
	AS_VAR_IF([cc_cv_werror],[x],[],[
		have_deprecated_attr=
		CC_CHECK_CFLAGS_APPEND([-Wdeprecated-declarations], [have_deprecated_attr=yes])
		AS_VAR_IF([have_deprecated_attr], [yes], [
			SAVE_CFLAGS=$CFLAGS
			CFLAGS="$CFLAGS $LIBXML_CFLAGS $cc_cv_werror -Wdeprecated-declarations"
			AC_MSG_CHECKING([whether xmlLineNumbersDefault is deprecated])
			AC_COMPILE_IFELSE([AC_LANG_PROGRAM([
				#include <libxml/parser.h>
			],[
				int main (void) {xmlLineNumbersDefault (1); return 0;}
			])],[is_xmlLineNumbersDefault_deprecated=no],[is_xmlLineNumbersDefault_deprecated=yes])
			CFLAGS=$SAVE_CFLAGS
			AC_MSG_RESULT($is_xmlLineNumbersDefault_deprecated)
			AS_VAR_IF([is_xmlLineNumbersDefault_deprecated],[yes],[
				AC_DEFINE(IS_xmlLineNumbersDefault_DEPRECATED)
			])
		])
	])
])

AC_ARG_ENABLE([json],
	[AS_HELP_STRING([--disable-json],
		[disable json support])])

AH_TEMPLATE([HAVE_JANSSON],
	[Define this value if jansson is available.])
dnl This enforces explicit feature usage regardless of the libraries
dnl available on the build system. This avoids automagic dependencies which
dnl can cause issues for source-based distros [1].
dnl [1]: https://wiki.gentoo.org/wiki/Project:Quality_Assurance/Automagic_dependencies
AS_IF([test "x$enable_json" != "xno"], [
	PKG_CHECK_MODULES(JANSSON, jansson,
			       [have_jansson=yes
			       AC_DEFINE(HAVE_JANSSON)],
			       [AS_IF([test "x$enable_json" = "xyes"], [
			           AC_MSG_ERROR([jansson not found])])])
])
AM_CONDITIONAL(HAVE_JANSSON, test "x$have_jansson" = xyes)

AH_TEMPLATE([HAVE_SECCOMP],
	[Define this value if libseccomp is available.])
AC_ARG_ENABLE([seccomp],
	[AS_HELP_STRING([--disable-seccomp],
		[disable seccomp support])])
AS_IF([test "x$enable_seccomp" != "xno"], [
	PKG_CHECK_MODULES(SECCOMP, libseccomp,
			       [have_seccomp=yes
			       AC_DEFINE(HAVE_SECCOMP)],
			       [AS_IF([test "x$enable_seccomp" = "xyes"], [
			           AC_MSG_ERROR([libseccomp not found])])])
])

AC_ARG_ENABLE([yaml],
	[AS_HELP_STRING([--disable-yaml],
		[disable yaml support])])

AH_TEMPLATE([HAVE_LIBYAML],
	[Define this value if libyaml is available.])
AS_IF([test "x$enable_yaml" != "xno"], [
	PKG_CHECK_MODULES(LIBYAML, yaml-0.1,
			       [have_libyaml=yes
			       AC_DEFINE(HAVE_LIBYAML)],
			       [AS_IF([test "x$enable_yaml" = "xyes"], [
			           AC_MSG_ERROR([libyaml not found])])])
])
AM_CONDITIONAL(HAVE_LIBYAML, test "x$have_libyaml" = xyes)

AC_ARG_ENABLE([pcre2],
	[AS_HELP_STRING([--disable-pcre2],
		[disable pcre2 support])])

AH_TEMPLATE([HAVE_PCRE2],
	[Define this value if pcre2 is available.])
AS_IF([test "x$enable_pcre2" != "xno"], [
	PKG_CHECK_MODULES(PCRE2, libpcre2-8,
			       [have_libpcre2_8=yes
			       AC_DEFINE(HAVE_PCRE2)],
			       [AS_IF([test "x$enable_pcre2" = "xyes"], [
			           AC_MSG_ERROR([libpcre2-8 not found])])])
])
AM_CONDITIONAL(HAVE_PCRE2, test "x$have_libpcre2_8" = xyes)

if test "${enable_static}" = "yes"; then
	if test "${have_libpcre2_8}" = "yes"; then
		if test "${host_mingw}" = "yes"; then
			dnl -DPCRE2_STATIC needs to be added manually.
			PCRE2_CFLAGS="$PCRE2_CFLAGS -DPCRE2_STATIC"
		fi
	fi
fi


# Checks for missing prototypes
# -----------------------------
AC_MSG_NOTICE(checking for new missing prototypes)

CHECK_PROTO(stat,	sys/stat.h)
CHECK_PROTO(lstat,	sys/stat.h)
if test "$have_truncate" = yes ; then
	CHECK_PROTO(truncate, unistd.h)
fi
if test "$have_ftruncate" = yes ; then
	CHECK_PROTO(ftruncate, unistd.h)
fi

# Process library configuration options
# -------------------------------------

ICONV_LIBS=
if test "$enable_iconv" != no ; then
	save_LIBS="$LIBS"
	#
	# Working around for issue #1620
	#
	LIBS="$LIBS $LIBXML_LIBS"
	LIBS="$LIBS $JANSSON_LIBS"
	LIBS="$LIBS $SECCOMP_LIBS"
	LIBS="$LIBS $LIBYAML_LIBS"
	LIBS="$LIBS $ASPELL_LIBS"
	LIBS="$LIBS -liconv"
	#
	AC_MSG_CHECKING(for iconv_open or libiconv_open with -liconv)
	AC_LINK_IFELSE([AC_LANG_PROGRAM([
			#include <iconv.h>
		], [iconv_open("fr", "to")])], [
		#
		# iconv_open is available in libiconv.
		#
		LIBS="$save_LIBS"
		ICONV_LIBS=-liconv
		AC_MSG_RESULT([yes(iconv)])
		AC_DEFINE(HAVE_ICONV)
		], [AC_LINK_IFELSE([AC_LANG_PROGRAM([
		#include <iconv.h>
		], [libiconv_open("fr", "to")])],[
		#
		# libiconv_open is available in libiconv
		#
		LIBS="$save_LIBS"
		ICONV_LIBS=-liconv
		AC_MSG_RESULT([yes(libiconv_open)])
		AC_DEFINE(HAVE_ICONV)
		AC_DEFINE(ICONV_USE_LIB_PREFIX)
		],[LIBS="$save_LIBS"
		AC_MSG_RESULT(no)
		AC_CHECK_FUNC(iconv_open, [
		#
		# iconv_open is available in libc.
		# No extra -l flag is needed.
		#
		AC_DEFINE(HAVE_ICONV)
		], [
		AC_MSG_ERROR([Could not find libiconv. Please install libiconv and libiconv-devel.])])])
		])
	AC_SUBST([ICONV_LIBS])
fi

# Checks for declaration
# ----------------------
AC_CHECK_DECLS([__environ], [], [], [
[#include <unistd.h>]
])
AC_CHECK_DECLS([_NSGetEnviron],[],[],[
[#include <crt_externs.h>]
])
AC_CHECK_DECLS([strnlen],[have_strnlen=yes],[],[
[#include <string.h>]
])
AM_CONDITIONAL(HAVE_STRNLEN, test "x$have_strnlen" = xyes)

if test "$cross_compiling" = "yes"; then
    AC_MSG_CHECKING(whether strnlen is declared for build)
    TEMP_DIR_FOR_CHECKING=$(mktemp -d || exit 1)
    cat > "$TEMP_DIR_FOR_CHECKING/test.c" <<EOF
#include<string.h>
int main() { strnlen("abcd",1); return 0;}
EOF

    if $CC_FOR_BUILD $CPPFLAGS_FOR_BUILD $CFLAGS_FOR_BUILD -o "$TEMP_DIR_FOR_CHECKING/test.o" "$TEMP_DIR_FOR_CHECKING/test.c" > /dev/null 2>&1 ; then
        have_strnlen_for_build=yes
    else
        have_strnlen_for_build=no
    fi
    AC_MSG_RESULT($have_strnlen_for_build)
    AM_CONDITIONAL(HAVE_STRNLEN_FOR_BUILD, test "x$have_strnlen_for_build" = xyes)
else
    AM_CONDITIONAL(HAVE_STRNLEN_FOR_BUILD, test "x$have_strnlen" = xyes)
fi

if test "$CTAGS_NAME_EXECUTABLE" != ctags ; then
	AC_MSG_NOTICE(Changing name of 'ctags' for $CTAGS_NAME_EXECUTABLE)
fi

if test "$ETAGS_NAME_EXECUTABLE" != etags ; then
	AC_MSG_NOTICE(Changing name of 'etags' for $ETAGS_NAME_EXECUTABLE)
fi

AC_ARG_ENABLE([bash-completion],
  AS_HELP_STRING([--disable-bash-completion], [do not install bash completion files]),
  [], [enable_bash_completion=yes]
)
AM_CONDITIONAL([BASH_COMPLETION], [test "x$enable_bash_completion" = xyes])

AC_ARG_WITH([bashcompletiondir],
  AS_HELP_STRING([--with-bashcompletiondir=DIR], [Bash completions directory]),
  [], [
  AS_IF([test "x$enable_bash_completion" = xyes], [
    PKG_CHECK_VAR([with_bashcompletiondir], [bash-completion], [completionsdir],
      [],
      [with_bashcompletiondir=${datadir}/bash-completion/completions])
  ])
])
AS_IF([test "x$enable_bash_completion" = xyes], [
  AC_SUBST([bashcompletiondir], [$with_bashcompletiondir])
])

# Output files
# ------------

AC_CHECK_LIB([m], [isnan])

AC_CHECK_FUNCS(mblen)

AC_CONFIG_FILES([Makefile gnulib/Makefile man/GNUmakefile])

AC_OUTPUT

AC_MSG_RESULT([
##############################################################################
Now you can run make or gmake. When you run as make, make sure it is GNU make.
##############################################################################
])

# vim:ts=4:sw=4:
