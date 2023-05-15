/*
*   Copyright (c) 2020, Masatake YAMATO
*   Copyright (c) 2020, Red Hat, Inc.
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains ctags specific optscript objects
*/

#include "general.h"  /* must always come first */

#include "debug.h"
#include "entry.h"
#include "field_p.h"
#include "htable.h"			/* For HT_PTR_TO_INT */
#include "optscript.h"
#include "parse.h"
#include "routines.h"
#include "script_p.h"
#include "xtag_p.h"

#include <ctype.h>
#include <string.h>

EsObject *OPTSCRIPT_ERR_NOTAGENTRY;
EsObject *OPTSCRIPT_ERR_UNKNOWNLANGUAGE;

int OPT_TYPE_MATCHLOC;
static int locEqual (const void *a, const void  *b);
static void locPrint (const void *a, MIO *out);

int OPT_TYPE_TAG;
static void tagFree (void *a);
static int tagEqual (const void *a, const void  *b);
static void tagPrint (const void *a, MIO *out);

static void vStringCatToupperS (vString *str, const char *s)
{
	for (const char *tmp = s; *tmp != '\0'; tmp++)
	{
		int c = toupper ((unsigned char) *tmp);
		vStringPut (str, c);
	}
}

extern OptVM *optscriptInit (void)
{
	opt_init ();
	MIO *in  = mio_new_fp (stdin, NULL);

	/* stdout is for emitting tags.
	 * The interpreter should not touch it; use only stderr. */
	MIO *out = mio_new_fp (stderr, NULL);
	MIO *err = mio_new_fp (stderr, NULL);

	OptVM *optvm = opt_vm_new (in, out, err);

	mio_unref (err);
	mio_unref (out);
	mio_unref (in);

	OPTSCRIPT_ERR_NOTAGENTRY = es_error_intern ("notagentry");

	OPT_TYPE_MATCHLOC = es_type_define_pointer ("matchloc",
												eFreeNoNullCheck,
												locEqual,
												locPrint);
	OPT_TYPE_TAG = es_type_define_pointer ("tagEntryInfo",
										   tagFree,
										   tagEqual,
										   tagPrint);
	return optvm;
}

static EsObject* lrop_get_field_value (OptVM *vm, EsObject *name)
{
	EsObject *nobj = opt_vm_ostack_top (vm);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get (nobj);
	tagEntryInfo *e = getEntryInCorkQueue (n);
	if (e == NULL)
		return OPTSCRIPT_ERR_NOTAGENTRY;

	void * data = es_symbol_get_data (name);
	fieldType ftype = HT_PTR_TO_INT (data);
	EsObject *val = getFieldValue (ftype, e);
	if (es_error_p (val))
		return val;

	opt_vm_ostack_pop (vm);

	if (isFieldValueAvailableAlways (ftype))
	{
		opt_vm_ostack_push (vm, val);
		es_object_unref (val);
	}
	else if (es_null (val))
	{
		opt_vm_ostack_push (vm, es_false);
	}
	else
	{
		opt_vm_ostack_push (vm, val);
		opt_vm_ostack_push (vm, es_true);
		es_object_unref (val);
	}
	return es_false;
}

static EsObject* lrop_set_field_value (OptVM *vm, EsObject *name)
{
	EsObject *indexobj = opt_vm_ostack_peek (vm, 1);
	if (!es_integer_p (indexobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get (indexobj);
	tagEntryInfo *e = getEntryInCorkQueue (n);
	if (e == NULL)
		return OPTSCRIPT_ERR_NOTAGENTRY;

	void * data = es_symbol_get_data (name);
	fieldType ftype = HT_PTR_TO_INT (data);
	unsigned int fdata_type = getFieldDataType (ftype);

	EsObject *valobj = opt_vm_ostack_top (vm);
	int valtype = es_object_get_type (valobj);

	if (hasFieldValueCheckerForSetter (ftype))
	{
		EsObject *e = checkFieldValueForSetter (ftype, valobj);
		if (!es_object_equal (e, es_false))
			return e;
	}
	else
	{
		if (! (((fdata_type & FIELDTYPE_STRING) && (valtype == OPT_TYPE_STRING))
			   || ((fdata_type & FIELDTYPE_BOOL) && (valtype == ES_TYPE_BOOLEAN))
			   || ((fdata_type & FIELDTYPE_INTEGER) && (valtype == ES_TYPE_INTEGER))))
			return OPT_ERR_TYPECHECK;
	}

	EsObject *r = setFieldValue (ftype, e, valobj);
	if (es_error_p (r))
		return r;

	opt_vm_ostack_pop (vm);
	opt_vm_ostack_pop (vm);

	return es_false;
}

static void optscriptInstallFieldGetterFast (EsObject *dict, fieldType ftype,
											 vString *op_name, vString *op_desc)
{
	vStringPut (op_name, ':');

	langType lang = getFieldLanguage (ftype);
	if (lang != LANG_IGNORE)
	{
		const char *lname = getLanguageName (lang);
		vStringCatS (op_name, lname);
		vStringPut (op_name, '.');
	}

	const char *fname = getFieldName (ftype);
	vStringCatS (op_name, fname);

	EsObject *op_sym = es_symbol_intern (vStringValue (op_name));
	es_symbol_set_data (op_sym, HT_INT_TO_PTR (ftype));

	const char *vtype = getFieldGetterValueType (ftype);
	unsigned int fdata_type = getFieldDataType (ftype);

	vStringCatS (op_desc, "int :");
	vStringCatToupperS (op_desc, fname);
	vStringPut (op_desc, ' ');

	if (vtype)
		vStringCatS (op_desc, vtype);
	else
	{
		Assert (fdata_type);
		if (fdata_type & FIELDTYPE_STRING)
			vStringCatS (op_desc, "string|");
		if (fdata_type & FIELDTYPE_INTEGER)
			vStringCatS (op_desc, "int|");
		if (fdata_type & FIELDTYPE_BOOL)
			vStringCatS (op_desc, "bool|");
		vStringChop (op_desc);
	}

	if (!isFieldValueAvailableAlways (ftype))
	{
		vStringPut (op_desc, ' ');
		vStringCatS (op_desc, "true%");
		vStringCatS (op_desc, "int :");
		vStringCatToupperS (op_desc, fname);
		vStringCatS (op_desc, " false");
	}

	EsObject *op = opt_operator_new (lrop_get_field_value,
									 vStringValue (op_name),
									 1, vStringValue (op_desc));
	opt_dict_def (dict, op_sym, op);
	es_object_unref (op);
}

static void optscriptInstallFieldSetterFast (EsObject *dict, fieldType ftype,
											 vString *op_name, vString *op_desc)
{
	langType lang = getFieldLanguage (ftype);
	if (lang != LANG_IGNORE)
	{
		const char *lname = getLanguageName (lang);
		vStringCatS (op_name, lname);
		vStringPut (op_name, '.');
	}

	const char *fname = getFieldName (ftype);
	vStringCatS (op_name, fname);
	vStringPut (op_name, ':');

	EsObject *op_sym = es_symbol_intern (vStringValue (op_name));
	es_symbol_set_data (op_sym, HT_INT_TO_PTR (ftype));

	const char *vtype = getFieldSetterValueType (ftype);
	unsigned int fdata_type = getFieldDataType (ftype);
	vStringCatS (op_desc, "tag:int ");

	if (vtype)
		vStringCatS (op_desc, vtype);
	else
	{
		Assert (fdata_type);
		if (fdata_type & FIELDTYPE_STRING)
			vStringCatS (op_desc, "string|");
		if (fdata_type & FIELDTYPE_INTEGER)
			vStringCatS (op_desc, "int|");
		if (fdata_type & FIELDTYPE_BOOL)
			vStringCatS (op_desc, "bool|");
		vStringChop (op_desc);
	}

	vStringPut (op_desc, ' ');
	vStringCatToupperS (op_desc, fname);
	vStringCatS (op_desc, ": -");

	EsObject *op = opt_operator_new (lrop_set_field_value,
									 vStringValue (op_name),
									 2, vStringValue (op_desc));
	opt_dict_def (dict, op_sym, op);
	es_object_unref (op);
}

extern void optscriptInstallFieldAccessor (EsObject *dict, fieldType ftype)
{
	vString *op_name = vStringNew ();
	vString *op_desc = vStringNew ();

	if (hasFieldGetter (ftype))
	{
		optscriptInstallFieldGetterFast (dict, ftype, op_name, op_desc);
		vStringClear (op_name);
		vStringClear (op_desc);
	}

	if (hasFieldSetter (ftype))
		optscriptInstallFieldSetterFast (dict, ftype, op_name, op_desc);

	vStringDelete (op_name);
	vStringDelete (op_desc);
}

static void optscriptInstallFieldAccessors (EsObject *dict)
{
	vString *op_name = vStringNew ();
	vString *op_desc = vStringNew ();

	/* In the current implementation, the upper boundary of the loop
	   can be "<= FIELD_BUILTIN_LAST" because the parser specific fields
	   are registered via defineField. However, when we change the order
	   and the way to initialize parsers, we may have to use "< countFields()"
	   instead. */
	for (fieldType ftype = 0; ftype <= FIELD_BUILTIN_LAST; ftype++)
	{
		if (hasFieldGetter (ftype))
		{
			optscriptInstallFieldGetterFast (dict, ftype, op_name, op_desc);
			vStringClear (op_name);
			vStringClear (op_desc);
		}
		if (hasFieldSetter (ftype))
		{
			optscriptInstallFieldSetterFast (dict, ftype, op_name, op_desc);
			vStringClear (op_name);
			vStringClear (op_desc);
		}
	}

	vStringDelete (op_name);
	vStringDelete (op_desc);
}

/* Define \1, \2,... \9 */
static void optscriptInstallMatchResultProcs (EsObject *dict,
											  OptOperatorFn fun)
{
	char name [] = { [0] = '\\', [2] = '\0' };
	char help [] = "- \\_ string|false";
	char *p = strchr (help, '_');
	for (int i = 1; i <= 9; i++)
	{
		name [1] = '0' + i;
		*p = name [1];
		EsObject *op_sym = es_symbol_intern (name);
		es_symbol_set_data (op_sym, HT_INT_TO_PTR (i));
		EsObject *op = opt_operator_new (fun, name, 0, help);
		opt_dict_def (dict, op_sym, op);
		es_object_unref (op);
	}
}

extern void optscriptInstallProcs (EsObject *dict,
								   OptOperatorFn matchResultAccessor)
{
	optscriptInstallFieldAccessors (dict);
	optscriptInstallMatchResultProcs (dict, matchResultAccessor);
}

static EsObject *optscript_CorkIndex_sym = es_nil;
extern void optscriptSetup (OptVM *vm, EsObject *dict, int corkIndex)
{
	if (corkIndex != CORK_NIL)
	{
		static EsObject *corkIndex_sym = es_nil;
		if (es_null (corkIndex_sym))
			corkIndex_sym = es_symbol_intern (".");
		EsObject *corkIndex_val = es_integer_new (corkIndex);
		opt_dict_def (dict, corkIndex_sym, corkIndex_val);
		es_object_unref (corkIndex_val);
		optscript_CorkIndex_sym = corkIndex_sym;
	}
}

extern void optscriptTeardown (OptVM *vm, EsObject *dict)
{
	if (!es_null (optscript_CorkIndex_sym))
	{
		opt_dict_undef (dict, optscript_CorkIndex_sym);
		optscript_CorkIndex_sym = es_nil;
	}
}

extern EsObject *optscriptRead (OptVM *vm, const char *src, size_t len)
{
	if (len == 0)
		len = strlen (src);

	MIO *mio = mio_new_memory ((unsigned char *)src, len, NULL, NULL);
	EsObject *obj = opt_vm_read (vm, mio);
	if (es_error_p (obj))
		opt_vm_report_error (vm, obj, NULL);
	mio_unref (mio);
	return obj;
}

extern EsObject* optscriptEval (OptVM *vm, EsObject *code)
{
	static EsObject *exec = es_nil;

	if (es_null (exec))
	{
		MIO *mio = mio_new_memory ((unsigned char*)"//exec", 6, NULL, NULL);
		exec = opt_vm_read (vm, mio);
		if (es_error_p (exec))
		{
			opt_vm_report_error (vm, exec, NULL);
			error (FATAL, "failed in converting //exec to an optscript object");
		}
		mio_unref (mio);
	}

	EsObject *o = opt_vm_eval (vm, code);
	if (es_error_p (o))
	{
		opt_vm_report_error (vm, o, NULL);
		error (FATAL, "failed to push the proc representing the script");
	}
	es_object_unref (o);

	EsObject *r = opt_vm_eval (vm, exec);
	if (es_error_p (r))
		opt_vm_report_error (vm, r, NULL);
	return r;
}

extern EsObject* optscriptDefine (EsObject *dict,
								  const char *name, EsObject *obj)
{
	EsObject *sym = es_symbol_intern (name);
	opt_dict_def (dict, sym, obj);
	return sym;
}


extern EsObject *optscriptLoad (OptVM *vm, MIO *mio)
{
	while (true)
	{
		EsObject *o = opt_vm_read (vm, mio);
		if (es_object_equal (o, ES_READER_EOF))
		{
			es_object_unref (o);
			return es_false;
		}
		else if (es_error_p (o))
		{
			opt_vm_report_error (vm, o, NULL);
			return o;
		}

		EsObject *e = opt_vm_eval (vm, o);
		if (es_error_p (e))
		{
			opt_vm_report_error (vm, e, NULL);
			es_object_unref (o);
			return e;
		}

		es_object_unref (o);
	}
}

extern EsObject *optscriptReadAndEval   (OptVM *vm, const char *src, size_t len)
{
	EsObject *obj = optscriptRead (vm, src, len);
	if (es_error_p (obj))
		return obj;

	EsObject *r = optscriptEval (vm, obj);
	es_object_unref (obj);
	return r;
}

extern EsObject *optscriptReadAndDefine (OptVM *vm, EsObject *dict, const char *name,
										 const char *src, size_t len)
{
	EsObject *obj = optscriptRead (vm, src, len);
	if (es_error_p (obj))
		return obj;
	return optscriptDefine (dict, name, obj);
}

static bool procdocs_add_key_val (EsObject *proc, EsObject *help_str, void *data)
{
	ptrArray *a = data;

	if (es_object_get_type (help_str) == OPT_TYPE_STRING)
		ptrArrayAdd (a, proc);

	return true;
}

static const char* procdocs_get_help_str (EsObject *proc, void *data)
{
	EsObject *dict = data;
	const char *name = opt_name_get_cstr (proc);
	EsObject *help_str = NULL;

	if (opt_dict_known_and_get_cstr (dict, name, &help_str))
		return opt_string_get_cstr(help_str);
	return NULL;
}

static void procdocs_add (ptrArray *a, void *data)
{
	EsObject *dict = data;
	opt_dict_foreach (dict, procdocs_add_key_val, a);
}

static struct OptHelpExtender procdocs_help_extender = {
	.add = procdocs_add,
	.get_help_str = procdocs_get_help_str,
};

extern void optscriptHelp (OptVM *vm, FILE *fp, EsObject *procdocs)
{
	MIO *out = mio_new_fp (fp, NULL);
	opt_vm_help (vm, out, procdocs? &procdocs_help_extender: NULL, procdocs);
	mio_unref (out);
}

static int locEqual (const void *a, const void  *b)
{
	if (a == b)
		return 1;

	const matchLoc *al = a;
	const matchLoc *bl = b;

	if (al->line == bl->line
		&& memcmp (&al->pos, &bl->pos, sizeof (al->pos)) == 0)
		return 1;
	return 0;
}

static void locPrint (const void *a, MIO *out)
{
	const matchLoc *al = a;
	mio_printf (out, "#<matchloc %p line: %lu>", a, al->line);
}

static void tagFree (void *a)
{
	tagEntryInfo *e = a;
	eFree ((void *)e->name);	/* TODO */
	eFree (e);
}

static int tagEqual (const void *a, const void  *b)
{
	if (a == b)
		return 1;
	return 0;
}

static void tagPrint (const void *a, MIO *out)
{
	const tagEntryInfo *tag = a;
	mio_printf (out, "#<tagEntryInfo %p name: %s line: %lu>",
				tag, tag->name, tag->lineNumber);
}

extern void optscriptRegisterOperators(EsObject * dict,
									   struct optscriptOperatorRegistration regs[], size_t count)
{
	EsObject *op;
	EsObject *sym;

	for (size_t i = 0; i < count; i++)
	{
		sym = es_symbol_intern (regs[i].name);
		op = opt_operator_new (regs[i].fn, es_symbol_get (sym), regs[i].arity,
							   regs[i].help_str);
		opt_dict_def (dict, sym, op);
		es_object_unref (op);
	}
}

extern xtagType optscriptGetXtagType (const EsObject *extra)
{
	EsObject *extra_sym = es_pointer_get (extra);
	const char *extra_str = es_symbol_get (extra_sym);

	const char *sep = strchr (extra_str, '.');
	if (sep)
	{
		langType lang = getNamedLanguage (extra_str, sep - extra_str);
		if (lang == LANG_IGNORE)
			return XTAG_UNKNOWN;

		return getXtagTypeForNameAndLanguage (sep + 1, lang);
	}
	else
		return getXtagTypeForNameAndLanguage (extra_str, LANG_IGNORE);
}
