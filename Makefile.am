ACLOCAL_AMFLAGS = -I m4
#AM_CPPFLAGS = -I$(top_builddir)/gnulib -I$(top_srcdir)/gnulib
GNULIB_DIR = gnulib
GNULIB_CPPFLAGS = -I$(top_builddir)/$(GNULIB_DIR) -I$(top_srcdir)/$(GNULIB_DIR)
GNULIB_LIBS = $(GNULIB_DIR)/libgnu.a $(LIBPTHREAD)
$(GNULIB_LIBS):
	$(MAKE) -C $(GNULIB_DIR) all
SUBDIRS = $(GNULIB_DIR) . man

include $(top_srcdir)/source.mak

PACKCC_FILES = misc/packcc/.gitignore   \
	       misc/packcc/README.md    \
	       misc/packcc/LICENSE      \
	       misc/packcc/src/packcc.c \
	       \
	       $(NULL)

EXTRA_DIST   = README.md NEWS.rst autogen.sh \
	       misc/optlib2c \
	       .ctags.d/exclusion.ctags .dir-locals.el .editorconfig .gdbinit .gitignore \
	       .indent.pro .uncrustify.cfg \
	       misc/ctags-optlib-mode.el \
	       misc/mk-interactive-request.sh misc/roundtrip misc/tinst \
	       $(PACKCC_FILES) \
	       misc/validators/validator-jq \
	       misc/validators/validator-KNOWN-INVALIDATION \
	       misc/validators/validator-NONE \
	       misc/validators/validator-puppet \
	       $(WIN32_HEADS) $(WIN32_SRCS) mk_mingw.mak mk_mvc.mak \
	       win32/mkstemp/COPYING.MinGW-w64-runtime.txt \
	       win32/ctags.rc win32/ctags.exe.manifest win32/resource.h \
	       win32/ctags_vs2013.sln \
	       win32/ctags_vs2013.vcxproj win32/ctags_vs2013.vcxproj.filters \
	       win32/ctags_vs2013.vcxproj.in win32/ctags_vs2013.vcxproj.filters.in \
	       win32/GNUmakefile \
	       win32/gen-repoinfo.bat \
	       misc/txt2cstr \
	       $(PEG_INPUT) $(OPTLIB2C_INPUT) $(OPTLIB2C_PCRE2_INPUT) $(TXT2CSTR_INPUT) \
	       docs Tmain Units m4/gnulib-cache.m4

if HAVE_PEGOF
# Keeping intermediate files for debugging pegof and u-ctags buildsys.
EXTRA_DIST   += $(PEGO_INTERMEDIATE)
endif

CLEANFILES =
MOSTLYCLEANFILES =
MAINTAINERCLEANFILES = autom4te.cache

# for distcheck
clean-local:
	@if test "$(top_srcdir)" != "$(top_builddir)"; then \
		rm -rf $(OPTLIB2C_SRCS) $(OPTLIB2C_PCRE2_SRCS); \
		rm -rf $(TXT2CSTR_SRCS); \
	else \
		$(MAKE) -C docs clean; \
	fi

maintainer-clean-local:
	rm -rf .deps misc/packcc/.dirstamp

bin_PROGRAMS = ctags
noinst_LIBRARIES =
noinst_LIBRARIES += libctags.a
noinst_LIBRARIES += libutil.a

noinst_PROGRAMS = utiltest

AM_LDFLAGS = $(EXTRA_LDFLAGS)

# packcc always uses native compiler even when cross-compiling.
# packcc cannot use the standard Automake rule.
PACKCC_CPPFLAGS_FOR_BUILD  = -fsigned-char
if HAVE_STRNLEN_FOR_BUILD
PACKCC_CPPFLAGS_FOR_BUILD += -DPCC_USE_SYSTEM_STRNLEN
endif

PACKCC = $(top_builddir)/packcc$(BUILD_EXEEXT)
CLEANFILES += $(PACKCC)

cc4b_verbose = $(cc4b_verbose_@AM_V@)
cc4b_verbose_ = $(cc4b_verbose_@AM_DEFAULT_V@)
cc4b_verbose_0 = @echo CC4BUILD "  $@";
$(PACKCC): $(srcdir)/$(PACKCC_SRC)
	$(cc4b_verbose)$(CC_FOR_BUILD) $(CFLAGS_FOR_BUILD) $(CPPFLAGS_FOR_BUILD) $(PACKCC_CPPFLAGS_FOR_BUILD) $(LDFLAGS_FOR_BUILD) -o $@ $(srcdir)/$(PACKCC_SRC)

if USE_READCMD
bin_PROGRAMS+= readtags
readtags_CPPFLAGS = -I. -I$(srcdir) -I$(srcdir)/main -I$(srcdir)/libreadtags
if ENABLE_DEBUGGING
readtags_CPPFLAGS+= $(DEBUG_CPPFLAGS)
endif
readtags_CFLAGS = $(EXTRA_CFLAGS) $(WARNING_CFLAGS) $(COVERAGE_CFLAGS)
dist_readtags_SOURCES = $(READTAGS_SRCS) $(READTAGS_HEADS)
readtags_CPPFLAGS += $(GNULIB_CPPFLAGS)
readtags_CPPFLAGS += -I$(srcdir)/dsl
readtags_CPPFLAGS += -DHAVE_CTAGS_INLINE_H
readtags_LDADD  =
readtags_LDADD += $(GNULIB_LIBS)
readtags_LDADD += libutil.a
dist_readtags_SOURCES += $(READTAGS_DSL_SRCS) $(READTAGS_DSL_HEADS)
endif

if HAVE_PCRE2
PARSER_SRCS += $(PCRE2_SRCS) $(OPTLIB2C_PCRE2_SRCS)
PARSER_HEADS += $(PCRE2_HEADS)
endif

if HAVE_LIBXML
PARSER_SRCS += $(XML_SRCS)
PARSER_HEADS += $(XML_HEADS)
endif

if HAVE_JANSSON
LIB_SRCS += $(JANSSON_SRCS)
LIB_HEADS += $(JANSSON_HEADS)
endif

if HAVE_LIBYAML
PARSER_SRCS += $(YAML_SRCS)
PARSER_HEADS += $(YAML_HEADS)
endif

PARSER_HEADS += $(PEG_EXTRA_HEADS)

libutil_a_CPPFLAGS = -I$(srcdir) -I$(srcdir)/main
libutil_a_CFLAGS   =
libutil_a_CFLAGS  += $(EXTRA_CFLAGS)
libutil_a_CFLAGS  += $(WARNING_CFLAGS)
libutil_a_CFLAGS  += $(COVERAGE_CFLAGS)
if ENABLE_DEBUGGING
libutil_a_CPPFLAGS+= $(DEBUG_CPPFLAGS)
endif
dist_libutil_a_SOURCES = $(UTIL_HEADS) $(UTIL_SRCS)

utiltest_CPPFLAGS   = -I$(srcdir) -I$(srcdir)/main
utiltest_CFLAGS     =
utiltest_CFLAGS    += $(EXTRA_CFLAGS)
utiltest_CFLAGS    += $(WARNING_CFLAGS)
utiltest_CFLAGS    += $(COVERAGE_CFLAGS)
if ENABLE_DEBUGGING
utiltest_CPPFLAGS  += $(DEBUG_CPPFLAGS)
endif
utiltest_LDADD  =
utiltest_LDADD += libutil.a
dist_utiltest_SOURCES = $(UTILTEST_HEADS) $(UTILTEST_SRCS)

libctags_a_CPPFLAGS = -I. -I$(srcdir) -I$(srcdir)/main -I$(srcdir)/dsl -I$(srcdir)/peg -DHAVE_PACKCC
if ENABLE_DEBUGGING
libctags_a_CPPFLAGS+= $(DEBUG_CPPFLAGS)
endif
if HAVE_STRNLEN
libctags_a_CPPFLAGS += -DUSE_SYSTEM_STRNLEN
endif
if HAVE_PEGOF
libctags_a_CPPFLAGS += -DHAVE_PEGOF
endif
libctags_a_CPPFLAGS+= $(GNULIB_CPPFLAGS)
libctags_a_CPPFLAGS+= -DHAVE_REPOINFO_H

libctags_a_CFLAGS   =
libctags_a_CFLAGS  += $(EXTRA_CFLAGS)
libctags_a_CFLAGS  += $(WARNING_CFLAGS)
libctags_a_CFLAGS  += $(COVERAGE_CFLAGS)
libctags_a_CFLAGS  += $(CGCC_CFLAGS)
libctags_a_CFLAGS  += $(LIBXML_CFLAGS)
libctags_a_CFLAGS  += $(JANSSON_CFLAGS)
libctags_a_CFLAGS  += $(LIBYAML_CFLAGS)
libctags_a_CFLAGS  += $(SECCOMP_CFLAGS)
libctags_a_CFLAGS  += $(PCRE2_CFLAGS)

nodist_libctags_a_SOURCES = $(REPOINFO_HEADS) $(PEG_SRCS) $(PEG_HEADS)
BUILT_SOURCES = $(REPOINFO_HEADS)
CLEANFILES += $(REPOINFO_HEADS) $(PEG_SRCS) $(PEG_HEADS)
EXTRA_libctags_a_DEPENDENCIES = $(GNULIB_LIBS)
$(REPOINFO_SRCS): $(REPOINFO_HEADS)
repoinfo_verbose = $(repoinfo_verbose_@AM_V@)
repoinfo_verbose_ = $(repoinfo_verbose_@AM_DEFAULT_V@)
repoinfo_verbose_0 = @echo REPOINFO "  $@";
if BUILD_IN_GIT_REPO
GEN_REPOINFO = $(srcdir)/misc/gen-repoinfo
$(REPOINFO_HEADS): FORCE
	$(repoinfo_verbose)$(GEN_REPOINFO) $@
FORCE:
else
$(REPOINFO_HEADS):
	$(repoinfo_verbose)echo > $@
endif

SUFFIXES=

if RUN_OPTLIB2C
optlib2c_verbose = $(optlib2c_verbose_@AM_V@)
optlib2c_verbose_ = $(optlib2c_verbose_@AM_DEFAULT_V@)
optlib2c_verbose_0 = @echo OPTLIB2C "  $@";
OPTLIB2C = $(srcdir)/misc/optlib2c
OPTLIB2C_DIR = optlib
SUFFIXES += .ctags
.ctags.c:
	$(optlib2c_verbose)$(OPTLIB2C) $< > $@
$(OPTLIB2C_SRCS): $(OPTLIB2C) Makefile
if HAVE_PCRE2
$(OPTLIB2C_PCRE2_SRCS): $(OPTLIB2C) Makefile
endif
endif

if RUN_TXT2CSTR
txt2cstr_verbose = $(txt2cstr_verbose_@AM_V@)
txt2cstr_verbose_ = $(txt2cstr_verbose_@AM_DEFAULT_V@)
txt2cstr_verbose_0 = @echo TXT2CSTR "  $@";
TXT2CSTR = $(srcdir)/misc/txt2cstr
TXT2CSTR_DIR = main
SUFFIXES += .ps
.ps.c:
	$(txt2cstr_verbose)$(TXT2CSTR) $< > $@
$(TXT2CSTR_SRCS): $(TXT2CSTR) Makefile
endif

packcc_verbose = $(packcc_verbose_@AM_V@)
packcc_verbose_ = $(packcc_verbose_@AM_DEFAULT_V@)
packcc_verbose_0 = @echo PACKCC "    $@";
SUFFIXES += .peg

if HAVE_PEGOF
pegof_verbose = $(pegof_verbose_@AM_V@)
pegof_verbose_ = $(pegof_verbose_@AM_DEFAULT_V@)
pegof_verbose_0 = @echo PEGOF "    $@";
SUFFIXES += .pego

PEGOF_FLAGS = -O all
.peg.pego:
	$(pegof_verbose)$(PEGOF) $(PEGOF_FLAGS) -o $(top_builddir)/peg/$(@F) -i "$<"
.pego.c:
	$(packcc_verbose)$(PACKCC) -o $(top_builddir)/peg/$(*F) $(top_builddir)/peg/$(<F)
.pego.h:
	$(packcc_verbose)$(PACKCC) -o $(top_builddir)/peg/$(*F) $(top_builddir)/peg/$(<F)
else
.peg.c:
	$(packcc_verbose)$(PACKCC) -o $(top_builddir)/peg/$(*F) "$<"
.peg.h:
	$(packcc_verbose)$(PACKCC) -o $(top_builddir)/peg/$(*F) "$<"
endif

# You cannot use $(PACKCC) as a target name here.
$(PEG_SRCS) $(PEG_HEADS): $(PACKCC) Makefile
dist_libctags_a_SOURCES = $(ALL_LIB_HEADS) $(ALL_LIB_SRCS)

ctags_CPPFLAGS = $(libctags_a_CPPFLAGS)
ctags_CFLAGS = $(libctags_a_CFLAGS)
ctags_LDADD  = libctags.a
ctags_LDADD += $(GNULIB_LIBS)
ctags_LDADD += $(LIBXML_LIBS)
ctags_LDADD += $(JANSSON_LIBS)
ctags_LDADD += $(LIBYAML_LIBS)
ctags_LDADD += $(SECCOMP_LIBS)
ctags_LDADD += $(ICONV_LIBS)
ctags_LDADD += $(PCRE2_LIBS)
dist_ctags_SOURCES = $(CMDLINE_HEADS) $(CMDLINE_SRCS)

if HOST_MINGW
RES_OBJ = win32/ctags.res.o
ctags_LDADD += $(RES_OBJ)
windres_verbose = $(windres_verbose_@AM_V@)
windres_verbose_ = $(windres_verbose_@AM_DEFAULT_V@)
windres_verbose_0 = @echo WINDRES "   $@";
$(RES_OBJ): win32/ctags.rc win32/ctags.exe.manifest win32/resource.h
	@mkdir -p $(builddir)/win32
	$(windres_verbose)$(WINDRES) -o $@ -O coff $<
MOSTLYCLEANFILES += $(RES_OBJ)
endif

noinst_PROGRAMS += mini-geany
mini_geany_CPPFLAGS = $(libctags_a_CPPFLAGS)
mini_geany_CFLAGS = $(libctags_a_CFLAGS)
mini_geany_LDADD  = libctags.a
mini_geany_LDADD += $(GNULIB_LIBS)
mini_geany_LDADD += $(LIBXML_LIBS)
mini_geany_LDADD += $(JANSSON_LIBS)
mini_geany_LDADD += $(LIBYAML_LIBS)
mini_geany_LDADD += $(SECCOMP_LIBS)
mini_geany_LDADD += $(ICONV_LIBS)
mini_geany_LDADD += $(PCRE2_LIBS)
mini_geany_SOURCES = $(MINI_GEANY_HEADS) $(MINI_GEANY_SRCS)

bin_PROGRAMS += optscript
optscript_CPPFLAGS = $(libctags_a_CPPFLAGS)
optscript_CFLAGS = $(libctags_a_CFLAGS)
optscript_LDADD  = libctags.a
# TODO: remove these dependencies from optscript command.
optscript_LDADD += $(GNULIB_LIBS)
optscript_LDADD += $(LIBXML_LIBS)
optscript_LDADD += $(JANSSON_LIBS)
optscript_LDADD += $(LIBYAML_LIBS)
optscript_LDADD += $(SECCOMP_LIBS)
optscript_LDADD += $(ICONV_LIBS)
optscript_LDADD += $(PCRE2_LIBS)
optscript_SOURCES = $(OPTSCRIPT_SRCS)

if INSTALL_ETAGS
install-exec-hook:
	cd $(DESTDIR)$(bindir) && \
		$(LN_S) ctags$(EXEEXT) etags$(EXEEXT)

uninstall-hook:
	rm $(DESTDIR)$(bindir)/etags$(EXEEXT)
endif

dist-hook:
	rm -rf $(distdir)/docs/_build
	find $(distdir)/Tmain $(distdir)/Units \( \
		-name '*-actual.txt' -o -name '*-diff.txt' -o \
		-name '*.TMP' -o -name '*.tmp' -o -name '*~' -o \
		-name 'dist.m4' -o -name 'Makefile.am' -o \
		-name 'Makefile.in' -o -name 'Makefile' -o \
		-name 'BUNDLES' \) -exec rm -f {} +

include $(top_srcdir)/makefiles/testing.mak

include $(top_srcdir)/makefiles/help.mak

bashcompletiondir = @bashcompletiondir@
if BASH_COMPLETION
dist_bashcompletion_DATA  =
dist_bashcompletion_DATA += completion/ctags.bash
endif
