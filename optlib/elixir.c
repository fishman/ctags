/*
 * Generated by ./misc/optlib2c from optlib/elixir.ctags, Don't edit this manually.
 */
#include "general.h"
#include "parse.h"
#include "routines.h"
#include "field.h"
#include "xtag.h"


static void initializeElixirParser (const langType language CTAGS_ATTR_UNUSED)
{
}

extern parserDefinition* ElixirParser (void)
{
	static const char *const extensions [] = {
		"ex",
		"exs",
		NULL
	};

	static const char *const aliases [] = {
		NULL
	};

	static const char *const patterns [] = {
		NULL
	};

	static kindDefinition ElixirKindTable [] = {
		{
		  true, 'f', "function", "functions (def ...)",
		},
		{
		  true, 'c', "callback", "callbacks (defcallback ...)",
		},
		{
		  true, 'd', "delegate", "delegates (defdelegate ...)",
		},
		{
		  true, 'e', "exception", "exceptions (defexception ...)",
		},
		{
		  true, 'g', "guard", "guards (defguard ...)",
		},
		{
		  true, 'i', "implementation", "implementations (defimpl ...)",
		},
		{
		  true, 'a', "macro", "macros (defmacro ...)",
		},
		{
		  true, 'o', "operator", "operators (e.g. \"defmacro a <<< b\")",
		},
		{
		  true, 'm', "module", "modules (defmodule ...)",
		},
		{
		  true, 'p', "protocol", "protocols (defprotocol...)",
		},
		{
		  true, 'r', "record", "records (defrecord...)",
		},
		{
		  true, 't', "test", "tests (test ...)",
		},
	};
	static tagRegexTable ElixirTagRegexTable [] = {
		{"^[ \t]*def((p?)|macro(p?))[ \t]+([a-zA-Z0-9_?!]+)[ \t]+([\\|\\^/&<>~.=!*+-]{1,3}|and|or|in|not|when|not in)[ \t]+[a-zA-Z0-9_?!]", "\\5",
		"o", "{exclusive}", NULL, false},
		{"^[ \t]*def(p?)[ \t]+([a-z_][a-zA-Z0-9_?!]*)(.[^\\|\\^/&<>~.=!*+-]+)", "\\2",
		"f", NULL, NULL, false},
		{"^[ \t]*(@|def)callback[ \t]+([a-z_][a-zA-Z0-9_?!]*)", "\\2",
		"c", NULL, NULL, false},
		{"^[ \t]*defdelegate[ \t]+([a-z_][a-zA-Z0-9_?!]*)", "\\1",
		"d", NULL, NULL, false},
		{"^[ \t]*defexception[ \t]+([A-Z][a-zA-Z0-9_]*\\.)*([A-Z][a-zA-Z0-9_?!]*)", "\\2",
		"e", NULL, NULL, false},
		{"^[ \t]*defguard(p?)[ \t]+(is_[a-zA-Z0-9_?!]+)", "\\2",
		"g", NULL, NULL, false},
		{"^[ \t]*defimpl[ \t]+([A-Z][a-zA-Z0-9_]*\\.)*([A-Z][a-zA-Z0-9_?!]*)", "\\2",
		"i", NULL, NULL, false},
		{"^[ \t]*defmacro(p?)[ \t]+([a-z_][a-zA-Z0-9_?!]*)(.[^\\|\\^/&<>~.=!*+-]+)", "\\2",
		"a", NULL, NULL, false},
		{"^[ \t]*defmodule[ \t]+([A-Z][a-zA-Z0-9_]*\\.)*([A-Z][a-zA-Z0-9_?!]*)", "\\2",
		"m", NULL, NULL, false},
		{"^[ \t]*defprotocol[ \t]+([A-Z][a-zA-Z0-9_]*\\.)*([A-Z][a-zA-Z0-9_?!]*)", "\\2",
		"p", NULL, NULL, false},
		{"^[ \t]*Record\\.defrecord(p?)[ \t(]+:([a-zA-Z0-9_]+)(\\)?)", "\\2",
		"r", NULL, NULL, false},
		{"^[ \t]*test[ \t(]+\"([a-z_][a-zA-Z0-9_?! ]*)\"*(\\)?)[ \t]*do", "\\1",
		"t", NULL, NULL, false},
	};


	parserDefinition* const def = parserNew ("Elixir");

	def->enabled       = true;
	def->extensions    = extensions;
	def->patterns      = patterns;
	def->aliases       = aliases;
	def->method        = METHOD_NOT_CRAFTED|METHOD_REGEX;
	def->kindTable     = ElixirKindTable;
	def->kindCount     = ARRAY_SIZE(ElixirKindTable);
	def->tagRegexTable = ElixirTagRegexTable;
	def->tagRegexCount = ARRAY_SIZE(ElixirTagRegexTable);
	def->initialize    = initializeElixirParser;

	return def;
}
