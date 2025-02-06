// for x in $(grep ^declop ~/var/ctags/dsl/optscript.c | sed -e 's/declop(\([^)]*\));/\1/'); do grep -q $x Tmain/optscript.d/*.ps || echo $x; done
/*
 *   Copyright (c) 2020, Masatake YAMATO
 *   Copyright (c) 2020, Red Hat, Inc.
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 */


#include "general.h"

#include "debug.h"
#include "es.h"
#include "htable.h"
#include "optscript.h"
#include "ptrarray.h"
#include "routines.h"
#include "vstring.h"

#include <ctype.h>
#include <string.h>


struct sOptVM
{
	ptrArray  *ostack;
	ptrArray  *dstack;
	ptrArray  *estack;

	int        dstack_protection;
	MIO       *in;
	MIO       *out;
	MIO       *err;

	EsObject  *error;

	int        print_depth;
	int        read_depth;
	char      *prompt;
	void      *app_data;
};

typedef struct sOperatorFat
{
	EsObject *name;
	int arity;
	const char *help_str;
} OperatorFat;

typedef struct sOperatorExtra
{
	const char *name;
	int arity;
	const char *help_str;
} OperatorExtra;

typedef OptOperatorFn Operator;

typedef enum eAttr {
	ATTR_READABLE   = 1 << 0,
	ATTR_WRITABLE   = 1 << 1,
	ATTR_EXECUTABLE = 1 << 2,
} Attr;

typedef struct sDictFat
{
	unsigned int attr;
} DictFat;

typedef struct sArrayFat
{
	unsigned int attr;
} ArrayFat;

typedef struct sStringFat
{
	unsigned int attr;
} StringFat;

typedef struct sNameFat
{
	unsigned int attr;
} NameFat;

static EsObject* opt_system_dict;

int OPT_TYPE_ARRAY;
int OPT_TYPE_DICT;
int OPT_TYPE_OPERATOR;
int OPT_TYPE_STRING;
int OPT_TYPE_NAME;
int OPT_TYPE_MARK;

static EsObject *OPT_ERR_UNDEFINED;
static EsObject *OPT_ERR_SYNTAX;
EsObject *OPT_ERR_UNDERFLOW;
EsObject *OPT_ERR_TYPECHECK;
EsObject *OPT_ERR_RANGECHECK;
static EsObject *OPT_ERR_DICTSTACKUNDERFLOW;
static EsObject *OPT_ERR_UNMATCHEDMARK;
static EsObject *OPT_ERR_INTERNALERROR;
static EsObject *OPT_ERR_END_PROC;
static EsObject *OPT_ERR_INVALIDEXIT;
static EsObject *OPT_ERR_STOPPED;
EsObject *OPT_ERR_QUIT;
static EsObject *OPT_ERR_INVALIDACCESS;
static EsObject *OPT_ERR_INTOVERFLOW;

static EsObject* OPT_MARK_ARRAY;
static EsObject* OPT_MARK_DICT;
static EsObject* OPT_MARK_MARK;

static EsObject* OPT_KEY_newerror;
static EsObject* OPT_KEY_errorname;
static EsObject* OPT_KEY_command;
static EsObject* OPT_KEY_ostack;
static EsObject* OPT_KEY_estack;
static EsObject* OPT_KEY_dstack;

/* Naming conversions
 *
 * Opt|OPT
 * =====================================================================
 * exported as part of the library API
 *
 * optscript and ctags may refer these names.
 *
 *
 * <datatype>_...
 * =====================================================================
 * functions released to PS datatypes
 * PS datatypes are array, dict, operator, ...
 *
 * <datatype>_es_...
 * ---------------------------------------------------------------------
 * functions for representing the PS datatype object as EsObject object
 *
 * <datatype>_op_...
 * ---------------------------------------------------------------------
 * functions for accessing the datatype object from vm internal purpose
 *
 *
 * op_...<operator>
 * =====================================================================
 * functions implementing operators
 *
 *
 * vm_...
 * =====================================================================
 * the rest VM related functions
 *
 */

static EsObject* array_new (unsigned int attr);
static EsObject* array_shared_new (EsObject* original, unsigned int attr);

static EsObject*    array_es_init_fat (void *fat, void *ptr, void *extra);
static void         array_es_free  (void *ptr, void *fat);
static int          array_es_equal (const void *a,
									const void *afat,
									const void *b,
									const void *bfat);
static void         array_es_print (const void *ptr, const void *fat, MIO *out);

static void         array_op_add    (EsObject* array, EsObject* elt);
static unsigned int array_op_length (const EsObject* array);
static EsObject*    array_op_get    (const EsObject* array, unsigned int n);
static void         array_op_put    (EsObject* array, unsigned int n, EsObject *obj);


static EsObject* dict_new (unsigned int size, unsigned int attr);

static EsObject* dict_es_init_fat (void *fat, void *ptr, void *extra);
static void      dict_es_free  (void *ptr, void *fat);
static int       dict_es_equal (const void *a,
								const void *afat,
								const void *b,
								const void *bfat);
static void      dict_es_print (const void *ptr, const void *fat, MIO *out);


static void      dict_op_def           (EsObject* dict, EsObject *key, EsObject *val);
static bool      dict_op_undef         (EsObject* dict, EsObject *key);
static bool      dict_op_known_and_get (EsObject* dict, EsObject *key, EsObject **val);
static void      dict_op_clear         (EsObject* dict);


static EsObject* operator_new (Operator op, const char *name, int arity, const char *help_str);

static EsObject* operator_es_init_fat (void *fat, void *ptr, void *extra);
static void      operator_es_print (const void *ptr, const void *fat, MIO *out);
static void      operator_es_free  (void *ptr, void *fat);


static EsObject* string_new   (vString *vstr);

static EsObject* string_es_init_fat (void *fat, void *ptr, void *extra);
static void      string_es_free  (void *ptr, void *fat);
static int       string_es_equal (const void *a,
								  const void *afat,
								  const void *b,
								  const void *bfat);
static void      string_es_print (const void *ptr, const void *fat, MIO *out);


static EsObject* name_new     (EsObject* symbol, unsigned int attr);
static EsObject* name_newS    (const char*s, unsigned int attr);
static EsObject* name_newS_cb (const char*s, void *attr);

static EsObject* name_es_init_fat (void *fat, void *ptr, void *extra);
static void      name_es_print (const void *ptr, const void *fat, MIO *out);
static void      name_es_free  (void *ptr, void *fat);
static int       name_es_equal (const void *a,
								const void *afat,
								const void *b,
								const void *bfat);


static EsObject* mark_new      (const char* mark);

static void      mark_es_print (const void *ptr, MIO *out);
static void      mark_es_free  (void *ptr);
static int       mark_es_equal (const void *a, const void *b);

static EsObject* vm_read          (OptVM *vm);
static EsObject* vm_call_operator (OptVM *vm, EsObject *op);
static EsObject* vm_call_proc     (OptVM *vm, EsObject *proc);
static void      vm_print         (OptVM *vm, EsObject *o);
static void      vm_print_full    (OptVM *vm, EsObject *o, bool string_as_is, int dict_recursion);
static void      vm_help          (OptVM *vm, MIO *out, struct OptHelpExtender *extop, void *data);
static void      vm_record_stop   (OptVM *vm, EsObject *cmd);
static void      vm_record_error  (OptVM *vm, EsObject *e, EsObject *cmd);
static void      vm_report_error  (OptVM *vm, EsObject *e);
static void      vm_bind_proc     (OptVM *vm, ptrArray *proc);

static void         vm_ostack_push        (OptVM *vm, EsObject *o);
static EsObject*    vm_ostack_pop         (OptVM *vm);
static unsigned int vm_ostack_count       (OptVM *vm);
static EsObject*    vm_ostack_top         (OptVM *vm);
static EsObject*    vm_ostack_peek        (OptVM *vm, int index_from_top);
static int          vm_ostack_counttomark (OptVM *vm);

static void         vm_dict_def           (OptVM *vm, EsObject *key, EsObject *val);

/* Returns the dictionary where the value for the key is found.
 * val can be NULL. */
static EsObject*    vm_dstack_known_and_get (OptVM *vm, EsObject *key, EsObject **val);
static void         vm_dstack_push          (OptVM *vm, EsObject *o);
/* FIXME: return type */
static int          vm_dstack_count         (OptVM *vm);
static EsObject*    vm_dstack_pop           (OptVM *vm);
static void         vm_dstack_clear         (OptVM *vm);

static EsObject*    vm_estack_push          (OptVM *vm, EsObject *p);
static EsObject*    vm_estack_pop           (OptVM *vm);

#define declop(OP)										\
	static EsObject* op_##OP(OptVM *vm, EsObject *name)


#define defOP(DICT, FN, NAME, ARITY, HELP)								\
	dict_op_def (DICT,													\
				 es_object_autounref(es_symbol_intern (NAME)),			\
				 es_object_autounref(operator_new (FN, NAME, ARITY, HELP)))

#define defop(DICT, NAME, ARITY, HELP)			\
	defOP (DICT, op_##NAME, #NAME, ARITY, HELP)

static EsObject* op__print_objdict_rec (OptVM *vm, EsObject *name);
static EsObject* op__print_objdict     (OptVM *vm, EsObject *name);
static EsObject* op__print_object      (OptVM *vm, EsObject *name);
static EsObject* op__print             (OptVM *vm, EsObject *name);
static EsObject* op__make_array        (OptVM *vm, EsObject *name);
static EsObject* op__make_dict         (OptVM *vm, EsObject *name);

/* non-standard operator */
declop(_help);

/* tested in pstack.ps */
declop(pstack);

/* error related non-standard operators */
declop(_newerror);
declop(_errorname);

/* Operators for operand stack manipulation
 * tested in stack.ps */
declop(pop);
declop(exch);
declop(dup);
declop(index);
declop(roll);
declop(clear);
declop(count);
declop(mark);
declop(cleartomark);
declop(counttomark);

/* Arithmetic Operators
   tested in arithmetic.ps */
declop(add);
declop(idiv);
declop(mod);
declop(mul);
declop(sub);
declop(abs);
declop(neg);

/* Operators for array manipulation
   tested in array.ps */
declop(array);
declop(astore);
declop(aload);

/* Operators for dictionary manipulation
 * tested in dict.ps */
declop(dict);
declop(begin);
declop(end);
declop(def);
declop(load);
declop(undef);
declop(known);
declop(where);
declop(currentdict);
declop(countdictstack);
declop(store);
declop(dictstack);
declop(cleardictstack);

/* Operators for string manipulation
   tested in string.ps */
/* -anchorsearch, -search, -token */
declop(string);
declop(_strstr);
declop(_strrstr);
declop(_strchr);
declop(_strrchr);
declop(_strpbrk);
declop(_strrpbrk);

/* Relation, logical, and bit operators
   tested in relalogbit.ps */
declop(eq);
declop(ne);
declop(true);
declop(false);
declop(and);
declop(or);
declop(xor);
declop(not);
declop(bitshift);
declop(ge);
declop(gt);
declop(le);
declop(lt);

/* Operators for control flow
 * tested in control.ps */
declop(exec);
declop(if);
declop(ifelse);
declop(repeat);
declop(loop);
declop(exit);
declop(stop);
declop(stopped);
declop(for);
declop(quit);
declop(countexecstack);
declop(execstack);
/* ?start */

/* Operators for type, attribute and their conversion
 * tested in typeattrconv.ps */
declop(type);
declop(cvn);
declop(cvs);
declop(cvx);
/* cvlit, xcheck, executeonly, noacess, readonly,
   rcheck, wcheck, cvi, cvr, cvrs, cvs,... */

/* Operators for Virtual Memory Operators  */
/* ?save, ?restore */

/* Misc operators
 * tested in misc.ps */
declop(null);
declop(bind);

/* Methods for compound objects
   tested in compound.ps */
declop(length);
declop(copy);
declop(get);
declop(put);
declop(forall);
declop(putinterval);
declop(_copyinterval);
/* -getinterval .... */


/*
 * Public functions
 */

int
opt_init (void)
{
	OPT_TYPE_ARRAY    = es_type_define_fatptr ("arraytype",
											   sizeof (ArrayFat),
											   array_es_init_fat,
											   array_es_free,
											   array_es_equal,
											   array_es_print);
	OPT_TYPE_DICT     = es_type_define_fatptr ("dicttype",
											   sizeof (DictFat),
											   dict_es_init_fat,
											   dict_es_free,
											   dict_es_equal,
											   dict_es_print);
	OPT_TYPE_OPERATOR = es_type_define_fatptr ("operatortype",
											   sizeof (OperatorFat),
											   operator_es_init_fat,
											   operator_es_free,
											   NULL,
											   operator_es_print);
	OPT_TYPE_STRING   = es_type_define_fatptr ("stringtype",
											   sizeof (StringFat),
											   string_es_init_fat,
											   string_es_free,
											   string_es_equal,
											   string_es_print);
	OPT_TYPE_NAME     = es_type_define_fatptr ("nametype",
											   sizeof (NameFat),
											   name_es_init_fat,
											   name_es_free,
											   name_es_equal,
											   name_es_print);
	OPT_TYPE_MARK   = es_type_define_pointer ("marktype",
											  mark_es_free,
											  mark_es_equal,
											  mark_es_print);

	OPT_ERR_UNDEFINED          = es_error_intern ("undefined");
	OPT_ERR_SYNTAX             = es_error_intern ("syntaxerror");
	OPT_ERR_UNDERFLOW          = es_error_intern ("stackunderflow");
	OPT_ERR_TYPECHECK          = es_error_intern ("typecheck");
	OPT_ERR_RANGECHECK         = es_error_intern ("rangecheck");
	OPT_ERR_DICTSTACKUNDERFLOW = es_error_intern ("dictstackunderflow");
	OPT_ERR_UNMATCHEDMARK      = es_error_intern ("unmatchedmark");
	OPT_ERR_INTERNALERROR      = es_error_intern ("internalerror");
	OPT_ERR_END_PROC           = es_error_intern ("}");
	OPT_ERR_INVALIDEXIT        = es_error_intern ("invalidexit");
	OPT_ERR_STOPPED            = es_error_intern ("stopped");
	OPT_ERR_QUIT               = es_error_intern ("quit");
	OPT_ERR_INVALIDACCESS      = es_error_intern ("invalidaccess");
	OPT_ERR_INTOVERFLOW        = es_error_intern ("intoverflow");

	es_symbol_intern ("true");
	es_symbol_intern ("false");
	es_symbol_intern ("null");

	OPT_MARK_ARRAY = mark_new ("[");
	OPT_MARK_DICT  = mark_new ("<<");
	OPT_MARK_MARK  = mark_new ("mark");

	opt_system_dict = dict_new (101, ATTR_READABLE);

	es_autounref_pool_push ();

	defOP (opt_system_dict, op__print_objdict_rec,"====", 1,  "any === -");
	defOP (opt_system_dict, op__print_objdict,    "===",  1,  "any === -");
	defOP (opt_system_dict, op__print_object,     "==",   1,  "any == -");
	defOP (opt_system_dict, op__print,            "=",    1,  "any == -");

	defOP (opt_system_dict, op_mark,           "<<",  0,  "- << mark");
	defOP (opt_system_dict, op_mark,           "[",   0,  "- [ mark");
	defOP (opt_system_dict, op__make_array,    "]",   0,  "[ any1 ... anyn ] array");
	defOP (opt_system_dict, op__make_dict ,    ">>",  0, "<< key1 value1 ... keyn valuen >> dict");

	defop (opt_system_dict, _help,  0, "- _HELP -");
	defop (opt_system_dict, pstack, 0, "|- any1 ... anyn PSTACK |- any1 ... anyn");

	defop (opt_system_dict, _newerror,  0, "- _NEWERROR bool");
	defop (opt_system_dict, _errorname, 0, "- _ERRORNAME error:name|null");

	defop (opt_system_dict, pop,    1, "any POP -");
	defop (opt_system_dict, exch,   2, "any1 any2 EXCH any2 any1");
	defop (opt_system_dict, dup,    1, "any DUP any any");
	defop (opt_system_dict, index,  1, "anyn ... any0 n INDEX anyn ... any0 anyn");
	defop (opt_system_dict, roll,   2, "any_n-1 ... any0 n j ROLL any_(j-1)_mod_n ... any_n-1 ... any_j_mod_n");
	defop (opt_system_dict, clear,  0, "|- any1 ... anyn CLEAR |-");
	defop (opt_system_dict, count,  0, "|- any1 ... anyn COUNT any1 ... anyn n");
	defop (opt_system_dict, mark,   0, "- MARK mark");
	defop (opt_system_dict, cleartomark, 1, "mark any1 ... anyn CLEARTOMARK -");
	defop (opt_system_dict, counttomark, 1, "mark any1 ... anyn COUNTTOMARK mark any1 ... anyn n");

	defop (opt_system_dict, add,  2, "int1 int2 ADD int");
	defop (opt_system_dict, idiv, 2, "int1 int2 IDIV int");
	defop (opt_system_dict, mod,  2, "int1 int1 MOD int");
	defop (opt_system_dict, mul,  2, "int1 int2 MUL int");
	defop (opt_system_dict, sub,  2, "int1 int2 SUB int");
	defop (opt_system_dict, abs,  1, "int1 ABS int2");
	defop (opt_system_dict, neg,  1, "int1 NEG int2");

	defop (opt_system_dict, array,  1, "int ARRAY array");
	defop (opt_system_dict, astore, 1, "any0 ... any_n_1 array ASTORE array");
	defop (opt_system_dict, aload,  1, "array ALOAD any0 ... any_n-1 array");

	defop (opt_system_dict, eq,       2, "any1 any2 EQ bool");
	defop (opt_system_dict, ne,       2, "any1 any2 NE bool");
	defop (opt_system_dict, true,     0, "- TRUE true");
	defop (opt_system_dict, false,    0, "- FALSE false");
	defop (opt_system_dict, ge,       2, "int1 int2 GE bool%"
		   "string1 string2 GE bool");
	defop (opt_system_dict, gt,       2, "int1 int2 GT bool%"
		   "string1 string2 GT bool");
	defop (opt_system_dict, le,       2, "int1 int2 LE bool%"
		   "string1 string2 LE bool");
	defop (opt_system_dict, lt,       2, "int1 int2 LT bool%"
		   "string1 string2 LT bool");
	defop (opt_system_dict, and,      2, "bool1 bool2 AND bool3%"
		   "int1 int2 AND int3");
	defop (opt_system_dict, or,       2, "bool1 bool2 OR bool3%"
		   "int1 int2 OR int3");
	defop (opt_system_dict, xor,      2, "bool1 bool2 XOR bool3%"
		   "int1 int2 XOR int3");
	defop (opt_system_dict, not,      1, "bool1|int1 NOT bool2|int2");
	defop (opt_system_dict, bitshift, 2, "int1 shift BITSHIFT int2");

	defop (opt_system_dict, dict,           1, "int DICT dict");
	defop (opt_system_dict, begin,          1, "dict BEGIN -");
	defop (opt_system_dict, end,            0, "- END -");
	defop (opt_system_dict, def,            2, "key value DEF -");
	defop (opt_system_dict, load,           1, "key LOAD value");
	defop (opt_system_dict, undef,          2, "dict key UNDEF -");
	defop (opt_system_dict, known,          2, "dict key KNOWN bool");
	defop (opt_system_dict, where,          1, "key WHERE dict true%key WHERE false");
	defop (opt_system_dict, store,          2, "key value STORE -");
	defop (opt_system_dict, currentdict,    0, "- CURRENTDICT dict");
	defop (opt_system_dict, countdictstack, 0, "- COUNTDICTSTACK int");
	defop (opt_system_dict, dictstack,      1, "array DICTSTACK array");
	defop (opt_system_dict, cleardictstack, 0, "- CLEARDICTSTACK -");

	defop (opt_system_dict, string,   1, "int STRING -");
	defop (opt_system_dict, _strstr,  2, "string seek _STRSTR string offset true%"
		   "string seek _STRSTR string false");
	defop (opt_system_dict, _strrstr, 2, "string seek _STRRSTR string offset true%"
		   "string seek _STRRSTR string false");
	defop (opt_system_dict, _strchr,  2, "string chr _STRCHR string offset true%"
		   "string chr _STRCHR string false");
	defop (opt_system_dict, _strrchr, 2, "string chr _STRRCHR string offset true%"
		   "string chr _STRRCHR string false");
	defop (opt_system_dict, _strpbrk, 2, "string accept _STRPBRK string offset true%"
		   "string accept _STRPBRK string false");
	defop (opt_system_dict, _strrpbrk, 2, "string accept _STRRPBRK string offset true%"
		   "string accept _STRRPBRK string false");

	defop (opt_system_dict, exec,           1, "any EXEC -");
	defop (opt_system_dict, if,             2, "bool proc IF -");
	defop (opt_system_dict, ifelse,         3, "bool proc_t proc_f IFELSE -");
	defop (opt_system_dict, repeat,         2, "int proc REPEAT -");
	defop (opt_system_dict, loop,           1, "proc LOOP -");
	defop (opt_system_dict, exit,           0, "- EXIT -");
	defop (opt_system_dict, stop,           0, "- STOP -");
	defop (opt_system_dict, stopped,        1, "any STOPPED bool");
	defop (opt_system_dict, for,            4, "initial increment limit proc FOR -");
	defop (opt_system_dict, quit,           0, "- quit -");
	defop (opt_system_dict, countexecstack, 0, "- countexecstack int");
	defop (opt_system_dict, execstack,      1, "array EXECSTACK array");

	defop (opt_system_dict, type,   1, "any TYPE name");
	defop (opt_system_dict, cvn,    1, "string CVN name");
	defop (opt_system_dict, cvs,    2, "any string CVS string");
	defop (opt_system_dict, cvx,    1, "any CVX any");

	defop (opt_system_dict, null,   0, "- NULL null");
	defop (opt_system_dict, bind,   1, "proc BIND proc");

	defop (opt_system_dict, copy,   1, "any1 ... anyn n COPY any1 ... anyn any1 ... anyn%"
		   "array1 array2 COPY array2%"
		   "dict1 dict2 COPY dict2%"
		   "string1 string2 COPY string2");
	defop (opt_system_dict, length, 1, "array LENGTH int%"
		   "dict LENGTH int%"
		   "string LENGTH int");
	defop (opt_system_dict, get,    2, "array index GET any%"
		   "dict key GET any%"
		   "string int GET int");
	defop (opt_system_dict, put,    3, "array index any PUT -%"
		   "dict key any PUT -%"
		   "string index int PUT -");
	defop (opt_system_dict, forall, 2, "array proc FORALL -%"
		   "dict proc FORALL -%"
		   "string proc FORALL -");
	defop (opt_system_dict, putinterval, 3, "array1 index array2 PUTINTERVAL -%"
		   "string1 index string2 PUTINTERVAL -");
	defop (opt_system_dict, _copyinterval, 4, "array1 index count array2 _COPYINTERVAL array2%"
		   "string1 index count string2 _COPYINTERVAL string2");

#define defKey(S) OPT_KEY_##S = es_symbol_intern(#S)
	defKey(newerror);
	defKey(errorname);
	defKey(command);
	defKey(ostack);
	defKey(estack);
	defKey(dstack);

	es_autounref_pool_pop ();

	return 0;
}

OptVM *
opt_vm_new (MIO *in, MIO *out, MIO *err)
{
	OptVM *vm = xCalloc (1, OptVM);

	vm->in    = mio_ref (in);
	vm->out   = mio_ref (out);
	vm->err   = mio_ref (err);

	EsObject *tmp;

	tmp = array_new (0);
	vm->ostack = (ptrArray *)es_pointer_take (tmp);
	es_object_unref (tmp);

	tmp = array_new (0);
	vm->dstack = (ptrArray *)es_pointer_take (tmp);
	es_object_unref (tmp);

	tmp = array_new (0);
	vm->estack = (ptrArray *)es_pointer_take (tmp);
	es_object_unref (tmp);

	vm->dstack_protection = 0;
	vm_dstack_push (vm, opt_system_dict);
	vm->dstack_protection++;

	vm->error = dict_new (6, ATTR_READABLE|ATTR_WRITABLE);

	vm->print_depth = 0;
	vm->read_depth = 0;
	vm->prompt = NULL;

	return vm;
}

void
opt_vm_clear (OptVM *vm, bool clear_app_data)
{
	ptrArrayClear  (vm->estack);
	ptrArrayClear  (vm->ostack);
	vm_dstack_clear (vm);
	if (clear_app_data)
		vm->app_data = NULL;
	dict_op_clear (vm->error);
}

void
opt_vm_delete (OptVM *vm)
{
	ptrArrayDelete  (vm->estack);
	ptrArrayDelete (vm->dstack);
	ptrArrayDelete  (vm->ostack);
	es_object_unref (vm->error);

	mio_unref (vm->err);
	mio_unref (vm->out);
	mio_unref (vm->in);
	eFree (vm);
}

EsObject *
opt_dict_new (unsigned int size)
{
	return dict_new (size, ATTR_READABLE|ATTR_WRITABLE);
}

bool
opt_dict_known_and_get_cstr (EsObject *dict, const char* name, EsObject **val)
{
	if (es_object_get_type (dict) != OPT_TYPE_DICT)
		return false;

	EsObject *sym = es_symbol_intern (name);
	return dict_op_known_and_get (dict, sym, val);
}

bool
opt_dict_known_and_get (EsObject *dict, EsObject *key, EsObject **val)
{
	return dict_op_known_and_get (dict, key, val);
}

bool
opt_dict_foreach (EsObject *dict, bool (* fn) (EsObject *, EsObject *, void*), void *data)
{
	if (es_object_get_type (dict) != OPT_TYPE_DICT)
		return false;

	hashTable *htable = es_pointer_get (dict);
	return hashTableForeachItem (htable, (hashTableForeachFunc) fn, data);
}

void
opt_dict_def (EsObject *dict, EsObject *sym, EsObject *val)
{
	Assert (!es_null(sym));
	dict_op_def (dict, sym, val);
}

bool
opt_dict_undef (EsObject *dict, EsObject *sym)
{
	Assert (!es_null(sym));
	return dict_op_undef (dict, sym);
}

void
opt_dict_clear (EsObject *dict)
{
	Assert (es_object_get_type (dict) == OPT_TYPE_DICT);
	dict_op_clear (dict);
}

EsObject *
opt_array_new (void)
{
	return array_new (ATTR_READABLE | ATTR_WRITABLE);
}

EsObject *
opt_array_get (const EsObject *array, unsigned int index)
{
	return array_op_get (array, index);
}

void
opt_array_put (EsObject *array, unsigned int index, EsObject *obj)
{
	array_op_put (array, index, obj);
}

void
opt_array_add (EsObject *array, EsObject* elt)
{
	array_op_add (array, elt);
}

unsigned int
opt_array_length(const EsObject *array)
{
	return array_op_length (array);
}

void
opt_vm_dstack_push  (OptVM *vm, EsObject *dict)
{
	vm_dstack_push (vm, dict);
	vm->dstack_protection++;
}

void
opt_vm_dstack_pop  (OptVM *vm)
{
	vm->dstack_protection--;
	vm_dstack_pop (vm);
}

EsObject*
opt_vm_ostack_top (OptVM *vm)
{
	return vm_ostack_top (vm);
}

EsObject*
opt_vm_ostack_peek (OptVM *vm, int index_from_top)
{
	return vm_ostack_peek (vm, index_from_top);
}

EsObject*
opt_vm_ostack_pop (OptVM *vm)
{
	return vm_ostack_pop (vm);
}

void
opt_vm_ostack_push        (OptVM *vm, EsObject *obj)
{
	vm_ostack_push (vm, obj);
}

unsigned int
opt_vm_ostack_count (OptVM *vm)
{
	return vm_ostack_count (vm);
}

static EsObject*
vm_eval (OptVM *vm, EsObject * o)
{
	EsObject *r = es_false;

	if (es_error_p (o))
	{
		r = o;
		goto out;
	}
	else if (es_object_get_type (o) == OPT_TYPE_NAME)
	{
		unsigned int attr = ((NameFat *)es_fatptr_get (o))->attr;
		if (attr & ATTR_EXECUTABLE)
		{
			EsObject *sym = es_pointer_get (o);
			EsObject *val  = es_nil;
			EsObject *dict = vm_dstack_known_and_get (vm, sym, &val);

			if (es_object_get_type (dict) == OPT_TYPE_DICT)
			{
				int t = es_object_get_type (val);
				if (t == OPT_TYPE_OPERATOR)
					r = vm_call_operator (vm, val);
				else if (t == OPT_TYPE_ARRAY
						 && (((ArrayFat *)es_fatptr_get (val))->attr & ATTR_EXECUTABLE))
					r = vm_call_proc (vm, val);
				else
				{
					vm_ostack_push (vm, val);
					r = es_false;
				}

				if (es_error_p (r))
					goto out;
			}
			else
			{
				r = es_error_set_object (OPT_ERR_UNDEFINED, o);
				vm_record_error (vm, r, o); /* TODO */
				goto out;
			}
		}
		else
			vm_ostack_push (vm, o);
	}
	else if (es_object_get_type (o) == OPT_TYPE_OPERATOR)
	{
		r = vm_call_operator (vm, o);
		goto out;
	}
	else
		vm_ostack_push (vm, o);
 out:
	return r;
}

EsObject*
opt_vm_read (OptVM *vm, MIO *in)
{
	EsObject *e;
	MIO *tmp;
	if (in)
	{
		tmp = vm->in;
		vm->in = in;
	}
	e = vm_read (vm);
	if (in)
		vm->in = tmp;
	return e;
}

EsObject *
opt_vm_eval (OptVM *vm, EsObject *obj)
{
	return vm_eval (vm, obj);
}

void
opt_vm_report_error (OptVM *vm, EsObject *eobj, MIO *err)
{
	MIO *tmp;

	if (err)
	{
		tmp = vm->err;
		vm->err = err;
	}
	vm_report_error	(vm, eobj);
	if (err)
		vm->err = tmp;
}

char*
opt_vm_set_prompt (OptVM *vm, char *prompt)
{
	char *tmp = vm->prompt;
	vm->prompt = prompt;
	return tmp;
}

void
opt_vm_print_prompt   (OptVM *vm)
{
	if (vm->prompt && vm->read_depth == 0)
	{
		mio_puts (vm->err, vm->prompt);
		unsigned int c = ptrArrayCount (vm->ostack);

		if (c > 0)
			mio_printf (vm->err, "<%u> ", c);
		else
			mio_printf (vm->err, "> ");
	}
}

void*
opt_vm_get_app_data (OptVM *vm)
{
	return vm->app_data;
}

void*
opt_vm_set_app_data (OptVM *vm, void *app_data)
{
	void *tmp = vm->app_data;
	vm->app_data = app_data;
	return tmp;
}

int
opt_vm_help    (OptVM *vm, MIO *out, struct OptHelpExtender *extop, void *data)
{
	vm_help (vm, out? out: vm->out, extop, data);
	return 0;
}

EsObject *
opt_operator_new (OptOperatorFn op, const char *name, int arity, const char *help_str)
{
	return operator_new (op, name, arity, help_str);
}

EsObject *opt_string_new_from_cstr     (const char *cstr)
{
	vString *vstr = vStringNewInit (cstr? cstr: "");
	return string_new (vstr);
}

const char* opt_string_get_cstr (const EsObject *str)
{
	vString *vstr = es_pointer_get (str);
	return vStringValue (vstr);
}

EsObject *opt_name_new_from_cstr (const char *cstr)
{
	return name_newS (cstr, ATTR_READABLE);
}

const char* opt_name_get_cstr (const EsObject *name)
{
	if (es_object_get_type (name) == OPT_TYPE_NAME)
		name = es_pointer_get (name);
	if (!es_symbol_p (name))
		return NULL;
	return es_symbol_get (name);
}


/*
 * VM
 */
static void
vm_read_skip_comment(OptVM *vm)
{
	while (true)
	{
		int c = mio_getc (vm->in);
		if (c == EOF || c == '\n' || c == '\r')
		{
			if (c != EOF)
				opt_vm_print_prompt (vm);
			return;
		}
	}
}

#define is_meta_char(c) ((c) == '%'				\
						 || (c) == '/'			\
						 || (c) == '('			\
						 || (c) == '{'			\
						 || (c) == '}'			\
						 || (c) == '['			\
						 || (c) == ']'			\
						 || (c) == '<'			\
						 || (c) == '>')

static EsObject*
vm_read_char (OptVM *vm)
{
	int c = mio_getc (vm->in);
	int i;

	if (c == EOF)
		return OPT_ERR_SYNTAX;
	else if (c == '\\')
	{
		c = mio_getc (vm->in);
		int i;
		switch (c)
		{
		case 't':
			i = '\t';
			break;
		case 'n':
			i = '\n';
			break;
		case 'f':
			i = '\f';
			break;
		case 'r':
			i = '\r';
			break;
		case 'v':
			i = '\v';
			break;
		case ' ':
		case '_':
			i = ' ';
			break;
		case '\\':
			i = '\\';
			break;
		default:
			return OPT_ERR_SYNTAX;
		}
		c = mio_getc (vm->in);
		if (!(c == EOF || isspace (c) || is_meta_char (c)))
			return OPT_ERR_SYNTAX;
		mio_ungetc (vm->in, c);
		return es_integer_new (i);
	}
	else if (isgraph(c))
	{
		i = c;

		c = mio_getc (vm->in);
		if (!(c == EOF || isspace (c) || is_meta_char (c)))
			return OPT_ERR_SYNTAX;
		mio_ungetc (vm->in, c);

		return es_integer_new (i);
	}
	else
		return OPT_ERR_SYNTAX;
}

static EsObject*
vm_read_string (OptVM *vm)
{
	int depth = 0;
	vString *s = vStringNew ();
	while (true)
	{
		int c = mio_getc (vm->in);
		if (c == ')')
		{
			if (depth == 0)
				return string_new (s);
			vStringPut (s, c);
			depth--;
		}
		else if (c == '(')
		{
			vStringPut (s, c);
			depth++;
		}
		else if (c == '\\')
		{
			c = mio_getc (vm->in);
			switch (c)
			{
			case EOF:
				vStringDelete (s);
				return OPT_ERR_SYNTAX;
			case 'n':
				vStringPut (s, '\n');
				break;
			case 't':
				vStringPut (s, '\t');
				break;
			case 'r':
				vStringPut (s, '\r');
				break;
			case 'f':
				vStringPut (s, '\f');
				break;
			case 'v':
				vStringPut (s, '\v');
				break;
			case '\\':
			case '(':
			case ')':
				vStringPut (s, c);
				break;
			default:
				vStringPut (s, c);
				break;
				;
			}
		}
		else if (c == EOF)
		{
			vStringDelete (s);
			return OPT_ERR_SYNTAX;
		}
		else
			vStringPut (s, c);
	}
}

static EsObject*
vm_read_generic(OptVM *vm, int c,
				EsObject * (* make_object) (const char *, void *),
				void *data)
{
	vString *name = vStringNew ();
	vStringPut (name, c);

	while (1)
	{
		c = mio_getc (vm->in);
		if (c == EOF)
			break;
		else if (isspace (c) || is_meta_char (c))
		{
			mio_ungetc (vm->in, c);
			break;
		}
		else
			vStringPut (name, c);
	}
	EsObject *n = make_object (vStringValue (name), data);
	vStringDelete (name);
	return n;
}

static EsObject*
vm_read_name (OptVM *vm, int c, unsigned int attr)
{
	return vm_read_generic (vm, c, name_newS_cb, &attr);
}

struct name_or_number_data {
	unsigned int attr;
	bool negative;
};

static EsObject*
name_or_number_new (const char* s, void *data)
{
	struct name_or_number_data *d = data;

	bool number = true;
	const char *t = s;
	while (*t)
	{
		if (!isdigit ((unsigned char) *t))
		{
			number = false;
			break;
		}
		t++;
	}
	if (number)
	{
		int n;
		if (strToInt (s, 10, &n))
			return es_integer_new (n * ((d->negative)? -1: 1));
		else
			return OPT_ERR_INTOVERFLOW;
	}
	else
		return name_newS_cb (s, &d->attr);
}

static EsObject*
vm_read_name_or_number (OptVM *vm, int c, unsigned int attr, bool negative)
{
	struct name_or_number_data data = {
		.attr     = attr,
		.negative = negative,
	};

	return vm_read_generic (vm, c, name_or_number_new, &data);
}

static EsObject*
vm_read_quoted (OptVM *vm)
{
	bool immediate = false;

	int c = mio_getc (vm->in);
	switch (c)
	{
	case '/':
		immediate = true;
		c = mio_getc (vm->in);
		break;
	default:
		break;
	}

	EsObject *s = vm_read_name (vm, c, ATTR_READABLE);
	if (immediate)
	{
		EsObject *q;

		EsObject *val  = es_nil;
		EsObject *dict = vm_dstack_known_and_get (vm, s, &val);
		if (es_object_get_type (dict) == OPT_TYPE_DICT)
			q = es_object_ref (val);
		else
		{
			q = es_error_set_object (OPT_ERR_UNDEFINED, s);
			vm_record_error (vm, q, s); /* TODO */
		}
		es_object_unref (s);
		return q;
	}
	else
		return s;
}

static EsObject*
vm_read_proc (OptVM *vm)
{
	EsObject *proc = array_new (ATTR_EXECUTABLE|ATTR_READABLE);

	vm->read_depth++;
	while (true)
	{
		EsObject *o = vm_read (vm);
		if (es_object_equal (o, OPT_ERR_END_PROC))
		{
			break;
		}
		else if (es_error_p (o))
		{
			es_object_unref (proc);
			proc = o;
			break;
		}
		else
		{
			array_op_add (proc, o);
			es_object_unref (o);
		}
	}
	vm->read_depth--;
	return proc;
}

static EsObject*
vm_read (OptVM *vm)
{
	while (true)
	{
		int c = mio_getc (vm->in);
		if (c == EOF)
			return es_object_ref (ES_READER_EOF);
		else if (c == '\n' || c == '\r')
		{
			opt_vm_print_prompt (vm);
			continue;
		}
		else if (isspace (c))
			continue;
		else if (c == '%')
		{
			vm_read_skip_comment (vm);
			continue;
		}
		else if (isdigit (c))
		{
			return vm_read_name_or_number (vm, c, ATTR_EXECUTABLE|ATTR_READABLE,
										   false);
		}
		else if (c == '-' || c == '+')
		{
			bool negative = (c == '-');
			c = mio_getc (vm->in);
			if (isdigit (c))
				return vm_read_name_or_number (vm, c, ATTR_EXECUTABLE|ATTR_READABLE,
											   negative);
			else
			{
				mio_ungetc (vm->in, c);
				return vm_read_name_or_number (vm, '-', ATTR_EXECUTABLE|ATTR_READABLE,
											   false);
			}
		}
		else if (c == '/')
			return vm_read_quoted (vm);
		else if (c == '(')
			return vm_read_string (vm);
		else if (c == '{')
			return vm_read_proc (vm);
		else if (c == '}')
		{
			if (vm->read_depth)
				return OPT_ERR_END_PROC;
			else
				return OPT_ERR_SYNTAX;
		}
		else if (c == '[' || c == ']')
		{
			const char name[2] = { [0] = c, [1] = '\0' };
			EsObject *s = es_symbol_intern (name);
			EsObject *n = name_new (s, ATTR_EXECUTABLE|ATTR_READABLE);
			return n;
		}
		else if (c == '<' || c == '>')
		{
			int c0 = mio_getc (vm->in);
			if (c != c0)
				return OPT_ERR_SYNTAX;

			const char name [3] = { [0] = c, [1] = c, [2] = '\0' };
			EsObject *s = es_symbol_intern (name);
			EsObject *n = name_new (s, ATTR_EXECUTABLE|ATTR_READABLE);
			return n;
		}
		else if (c == '?')
			return vm_read_char (vm);
		else
			return vm_read_name (vm, c, ATTR_EXECUTABLE|ATTR_READABLE);
	}
}

static void
vm_ostack_push (OptVM *vm, EsObject *o)
{
	ptrArrayAdd (vm->ostack, es_object_ref (o));
}

static EsObject*
vm_ostack_pop (OptVM *vm)
{
	unsigned int c = vm_ostack_count (vm);

	if (c > 0)
	{
		ptrArrayDeleteLast (vm->ostack);
		return es_false;
	}

	return OPT_ERR_UNDERFLOW;
}

static unsigned int
vm_ostack_count (OptVM *vm)
{
	return ptrArrayCount (vm->ostack);
}

static int
vm_ostack_counttomark (OptVM *vm)
{
	unsigned int c = ptrArrayCount (vm->ostack);
	unsigned int i;

	if  (c == 0)
		return -1;

	for (i = c; i > 0; i--)
	{
		EsObject *elt = ptrArrayItem (vm->ostack, i - 1);
		if (es_object_get_type (elt) == OPT_TYPE_MARK)
			break;
	}

	if (i == 0)
		return -1;

	int r = (c - i);
	if (r < 0)					/* FIXME */
		r = -1;
	return r;
}

static EsObject*
vm_ostack_top (OptVM *vm)
{
	if (ptrArrayCount (vm->ostack) > 0)
		return ptrArrayLast (vm->ostack);
	return OPT_ERR_UNDERFLOW;
}

static EsObject*
vm_ostack_peek (OptVM *vm, int index_from_top)
{
	unsigned int c = ptrArrayCount (vm->ostack);
	if (c > (unsigned int)index_from_top)
	{
		unsigned int i = (c - ((unsigned int)index_from_top)) - 1;
		Assert (i < c);
		return ptrArrayItem (vm->ostack, i);
	}
	return OPT_ERR_UNDERFLOW;
}

static EsObject*
vm_dstack_known_and_get (OptVM *vm, EsObject *key, EsObject **val)
{
	if (es_object_get_type (key) == OPT_TYPE_NAME)
		key = es_pointer_get (key);

	int c = ptrArrayCount (vm->dstack);

	for (int i = c - 1; i >= 0; i--)
	{
		EsObject *d = ptrArrayItem (vm->dstack, i);
		if (dict_op_known_and_get (d, key, val))
			return d;
	}
	return es_false;
}

static void
vm_dict_def (OptVM *vm, EsObject *key, EsObject *val)
{
	Assert (!es_null(key));
	dict_op_def (ptrArrayLast(vm->dstack), key, val);
}

static void
vm_dstack_push  (OptVM *vm, EsObject *o)
{
	ptrArrayAdd (vm->dstack, es_object_ref (o));
}

static int
vm_dstack_count (OptVM *vm)
{
	return ptrArrayCount (vm->dstack);
}

static EsObject*
vm_dstack_pop (OptVM *vm)
{
	if (vm_dstack_count (vm) <= vm->dstack_protection)
		return OPT_ERR_DICTSTACKUNDERFLOW;
	ptrArrayDeleteLast (vm->dstack);
	return es_false;
}

static void
vm_dstack_clear         (OptVM *vm)
{
	while (ptrArrayCount (vm->dstack) > 1)
		ptrArrayDeleteLast (vm->dstack);

	vm->dstack_protection = 1;
}

static EsObject*
vm_call_operator (OptVM *vm, EsObject *op)
{
	EsObject *r;

	Operator operator = es_pointer_get (op);
	OperatorFat *ofat = es_fatptr_get (op);

	vm_estack_push (vm, op);

	if (ofat->arity > 0)
	{
		unsigned int c = ptrArrayCount (vm->ostack);
		if (c < (unsigned int)ofat->arity)
		{
			vm_estack_pop (vm);
			vm_record_error (vm, OPT_ERR_UNDERFLOW, op);
			return OPT_ERR_UNDERFLOW;
		}
	}

	r = (* operator) (vm, ofat->name);
	if (es_error_p (r))
	{
		vm_estack_pop (vm);
		if (es_object_equal (OPT_ERR_STOPPED, r))
			vm_record_stop (vm, op);
		else
			vm_record_error (vm, r, op);
		return r;
	}

	vm_estack_pop (vm);
	return es_false;
}

static EsObject*
vm_call_proc     (OptVM *vm, EsObject *proc)
{
	ptrArray *a = es_pointer_get (proc);
	unsigned int c = ptrArrayCount (a);

	vm_estack_push (vm, proc);
	for (unsigned int i = 0; i < c; i++)
	{
		EsObject *o = ptrArrayItem (a, i);
		EsObject* e = vm_eval (vm, o);
		if (es_error_p (e))
		{
			vm_estack_pop (vm);	/* ??? */
			return e;
		}
	}
	vm_estack_pop (vm);

	return es_false;
}

static EsObject*
vm_estack_push (OptVM *vm, EsObject *p)
{
	ptrArrayAdd (vm->estack, es_object_ref (p));
	return es_false;
}

static EsObject*
vm_estack_pop (OptVM *vm)
{
	if (ptrArrayCount (vm->estack) < 1)
		return OPT_ERR_INTERNALERROR;
	ptrArrayDeleteLast (vm->estack);
	return es_false;
}

static void
insert_spaces (MIO *mio, int n)
{
	while (n-- > 0)
		mio_putc(mio, ' ');
}

struct htable_print_data {
	OptVM *vm;
	int dict_recursion;
};

static bool
htable_print_entry (const void *key, void *val, void *user_data)
{
	struct htable_print_data *data = user_data;

	vm_print_full (data->vm, (EsObject *)key, false, data->dict_recursion);
	mio_putc (data->vm->out, ' ');
	vm_print_full (data->vm, (EsObject *)val, false, data->dict_recursion);

	return true;
}

static bool
htable_print_entries (const void *key, void *val, void *user_data)
{
	struct htable_print_data *data = user_data;

	insert_spaces (data->vm->out, data->vm->print_depth * 2);
	htable_print_entry (key, val, user_data);
	mio_putc (data->vm->out, '\n');

	return true;
}

static void
vm_print (OptVM *vm, EsObject *elt)
{
	vm_print_full (vm, elt, false, 0);
}

static void
vm_print_full(OptVM *vm, EsObject *elt, bool string_as_is, int dict_recursion)
{
	if (es_object_equal (elt, es_true))
		mio_puts (vm->out, "true");
	else if (es_object_equal (elt, es_false))
		mio_puts (vm->out, "false");
	else if (es_object_equal (elt, es_nil))
		mio_puts (vm->out, "null");
	else if (es_error_p (elt))
	{
		mio_putc (vm->out, '/');
		mio_puts (vm->out, es_error_name (elt));
	}
	else if (es_object_get_type (elt) == OPT_TYPE_DICT)
	{
		hashTable *d = es_pointer_get (elt);

		struct htable_print_data data = {
			.vm = vm,
			.dict_recursion = dict_recursion - 1,
		};

		if (dict_recursion)
		{
			switch (hashTableCountItem (d))
			{
			case 0:
				mio_puts(vm->out, "<<>> ");
				break;
			case 1:
				mio_puts(vm->out, "<<");
				hashTableForeachItem (d, htable_print_entry, &data);
				mio_puts(vm->out, ">> ");
				break;
			default:
				mio_puts(vm->out, "<<\n");
				vm->print_depth++;
				hashTableForeachItem (d, htable_print_entries, &data);
				vm->print_depth--;
				insert_spaces (vm->out, vm->print_depth*2);
				mio_puts(vm->out, ">> ");
				break;
			}
		}
		else
		{
			mio_printf (vm->out, "-dict:%u-",
						hashTableCountItem (d));
		}
	}
	else if (es_object_get_type (elt) == OPT_TYPE_ARRAY)
	{
		ArrayFat *afat = (ArrayFat *)es_fatptr_get (elt);
		ptrArray *a    = (ptrArray *)es_pointer_get (elt);
		unsigned int c = ptrArrayCount (a);
		int is_proc = (afat->attr & ATTR_EXECUTABLE)? 1: 0;

		mio_putc (vm->out, is_proc? '{': '[');
		vm->print_depth += is_proc;
		for (unsigned int i = 0; i < c; i++)
		{
			vm_print_full (vm, (EsObject *)ptrArrayItem (a, i), false, dict_recursion);
			if (i != c - 1)
				mio_putc (vm->out, ' ');
		}
		vm->print_depth -= is_proc;
		mio_putc (vm->out, is_proc? '}': ']');
	}
	else if (es_object_get_type (elt) == OPT_TYPE_STRING && string_as_is)
	{
		const char *cstr = opt_string_get_cstr (elt);
		mio_puts (vm->out, cstr);
	}
	else if ((es_object_get_type (elt) == OPT_TYPE_NAME || es_symbol_p (elt))
			 && string_as_is)
	{
		const char *cstr = opt_name_get_cstr (elt);
		mio_puts (vm->out, cstr);
	}
	else if (es_symbol_p (elt) && (! string_as_is))
	{
		mio_putc (vm->out, '/');
		es_print (elt, vm->out);
	}
	else
		es_print (elt, vm->out);
}

static bool
collect_operators (const void *key, void *value, void *user_data)
{
	ptrArray *a = user_data;
	EsObject *op   = value;

	if (es_object_get_type (op) == OPT_TYPE_OPERATOR)
	{
		OperatorFat *ofat = es_fatptr_get (op);
		if (ofat->help_str)
			ptrArrayAdd (a, op);
	}
	return true;
}

static const char*
callable_get_name (const EsObject *callable)
{
	if (es_object_get_type (callable) == OPT_TYPE_OPERATOR)
	{
		const OperatorFat *ofat_callable = es_fatptr_get (callable);
		return es_symbol_get (ofat_callable->name);
	}
	else
		return opt_name_get_cstr(callable);
}

static int
compare_callable_by_name (const void *a, const void *b)
{
	const char *str_a = callable_get_name (a);
	const char *str_b = callable_get_name (b);

	return strcmp (str_a, str_b);
}

static void
vm_help (OptVM *vm, MIO *out, struct OptHelpExtender *extop, void *data)
{
	unsigned int c = ptrArrayCount (vm->dstack);

	ptrArray *a = ptrArrayNew (NULL);
	for (unsigned int i = 0; i < c; i++)
	{
		hashTable *t = es_pointer_get (ptrArrayItem (vm->dstack, i));
		hashTableForeachItem (t, collect_operators, a);
	}
	if (extop)
		extop->add (a, data);

	ptrArraySort (a, compare_callable_by_name);

	unsigned int ca = ptrArrayCount (a);
	size_t maxlen = 0;
	for (unsigned int i = 0; i < ca; i++)
	{
		EsObject* obj = ptrArrayItem (a, i);
		const char *name = callable_get_name (obj);

		size_t l = strlen (name);
		if (l > maxlen)
			maxlen = l;
	}

	for (unsigned int i = 0; i < ca; i++)
	{
		EsObject* obj = ptrArrayItem (a, i);
		const char *name = NULL;
		const char *help_str_head = NULL;
		const char *help_str_original = NULL;

		if (es_object_get_type (obj) == OPT_TYPE_OPERATOR)
		{
			OperatorFat *ofat = es_fatptr_get (obj);
			name = es_symbol_get (ofat->name);
			help_str_head = ofat->help_str;
		}
		else if (extop)
		{
			name = opt_name_get_cstr (obj);
			help_str_head = extop->get_help_str (obj, data);
		}
		help_str_original = help_str_head;

		if (name == NULL || help_str_head == NULL)
			continue;

		while (help_str_head)
		{
			const char *next = strpbrk (help_str_head, "%\n");
			const char *label = (help_str_head == help_str_original)? name: NULL;
			if (next)
			{
				char *tmp = eStrndup (help_str_head, next - help_str_head);
				bool desc = (tmp[0] == ':');
				mio_printf (out, "%*s%s%s\n",
							(int)maxlen, label? label: "",
							((desc || (label == NULL))? "		": "	->	"),
							(desc? tmp + 1: tmp));
				eFree ((char *)tmp);
				help_str_head = next + 1;
				while (*help_str_head && isspace ((unsigned char)*help_str_head))
					help_str_head++;
			}
			else
			{
				if (*help_str_head != '\0')
				{
					bool desc = (help_str_head[0] == ':');
					mio_printf (out, "%*s%s%s\n",
								(int)maxlen, label? label: "",
								((desc || (label == NULL))? "		": "	->	"),
								(desc? help_str_head + 1: help_str_head));
				}
				help_str_head = NULL;
			}
		}
	}

	ptrArrayDelete (a);
}

static EsObject *
array_new_from_stack (ptrArray *src)
{
	EsObject *dst = array_new (0);
	ptrArray *a = (ptrArray *)es_pointer_get (dst);
	for (unsigned int i = 0; i < ptrArrayCount(src); i++)
		ptrArrayAdd (a, es_object_ref (ptrArrayItem (src, i)));
	return dst;
}

static void
vm_record_stop   (OptVM *vm, EsObject *cmd)
{
	dict_op_def (vm->error, OPT_KEY_command, cmd);
	dict_op_def (vm->error, OPT_KEY_errorname, es_nil);
	dict_op_def (vm->error, OPT_KEY_newerror, es_false);
	/* OPT_KEY_{o,e,o}stack are kept as is. */
}

static void
vm_record_error  (OptVM *vm, EsObject *e, EsObject *cmd)
{
	EsObject *newerror = es_nil;
	if (dict_op_known_and_get (vm->error, OPT_KEY_newerror, &newerror)
		&& es_object_equal (newerror, es_true))
		return;

	dict_op_def (vm->error, OPT_KEY_newerror, es_true);
	dict_op_def (vm->error, OPT_KEY_errorname, e);
	dict_op_def (vm->error, OPT_KEY_command, cmd);

	EsObject *a;

	a = array_new_from_stack (vm->ostack);
	dict_op_def (vm->error, OPT_KEY_ostack, a);
	es_object_unref (a);

	a = array_new_from_stack (vm->estack);
	dict_op_def (vm->error, OPT_KEY_estack, a);
	es_object_unref (a);

	a = array_new_from_stack (vm->dstack);
	dict_op_def (vm->error, OPT_KEY_dstack, a);
	es_object_unref (a);
}

static void
vm_report_error (OptVM *vm, EsObject *e)
{
	MIO *out = vm->out;
	vm->out = vm->err;
	mio_puts (vm->err, "Error: ");

	EsObject *newerror = es_nil;
	if (!dict_op_known_and_get (vm->error, OPT_KEY_newerror, &newerror))
	{
		vm_print (vm, e);
		mio_putc (vm->err, '\n');
		goto out;
	}

	if (es_object_equal (newerror, es_false))
	{
		vm_print (vm, e);
		mio_putc (vm->err, '\n');
		goto out;
	}

	if (!dict_op_known_and_get (vm->error, OPT_KEY_errorname, &e))
	{
		vm_print (vm, OPT_ERR_INTERNALERROR);
		mio_putc (vm->err, '\n');
		goto out;
	}

	vm_print (vm, e);

	EsObject *command = es_nil;
	dict_op_known_and_get (vm->error, OPT_KEY_command, &command);
	EsObject *attached_object = es_error_get_object (e);

	if (!es_null (attached_object))
	{
		mio_puts (vm->err, " in ");
		vm_print (vm, attached_object);
	}
	else if (!es_null (command))
	{
		mio_puts (vm->err, " in ");
		vm_print (vm, command);
		command = es_nil;
	}
	mio_putc (vm->err, '\n');

	EsObject *ostack = es_nil;
	if (dict_op_known_and_get (vm->error, OPT_KEY_ostack, &ostack))
	{
		mio_puts (vm->err, "Operand stack:\n");
		mio_puts (vm->err, "top|");
		ptrArray *a = es_pointer_get (ostack);
		for (unsigned int i = ptrArrayCount (a); i > 0; i--)
		{
			EsObject *o = ptrArrayItem (a, i - 1);
			mio_puts (vm->err, "   ");
			vm_print (vm, o);
		}
	}
	mio_puts (vm->err, "   |bottom\n");

	EsObject *estack = es_nil;
	if (dict_op_known_and_get (vm->error, OPT_KEY_estack, &estack))
	{
		mio_puts (vm->err, "Execution stack:\n");
		mio_puts (vm->err, "top|");

		if (!es_null (command))
		{
			mio_puts (vm->err, "   ");
			vm_print (vm, command);
		}

		ptrArray *a = es_pointer_get (estack);
		for (unsigned int i = ptrArrayCount (a); i > 0; i--)
		{
			EsObject *o = ptrArrayItem (a, i - 1);
			mio_puts (vm->err, "   ");
			vm_print (vm, o);
		}
	}
	mio_puts (vm->err, "   |bottom\n");

	EsObject *dstack = es_nil;
	if (dict_op_known_and_get (vm->error, OPT_KEY_dstack, &dstack))
	{
		mio_puts (vm->err, "Dictionary stack:\n");
		mio_puts (vm->err, "top|");
		ptrArray *a = es_pointer_get (dstack);
		for (unsigned int i = ptrArrayCount (a); i > 0; i--)
		{
			EsObject *o = ptrArrayItem (a, i - 1);
			mio_puts (vm->err, "   ");
			vm_print (vm, o);
		}
	}
	mio_puts (vm->err, "   |bottom\n");

 out:
	dict_op_def (vm->error, OPT_KEY_newerror, es_false);
	vm->out = out;
}

static void
vm_bind_proc (OptVM *vm, ptrArray *proc)
{
	unsigned int c = ptrArrayCount (proc);
	for (unsigned int i = 0; i < c; i++)
	{
		EsObject *x = ptrArrayItem (proc, i);

		if (es_object_get_type (x) == OPT_TYPE_ARRAY)
			vm_bind_proc (vm, es_pointer_get (x));
		else if (es_object_get_type (x) == OPT_TYPE_NAME)
		{
			if (!(((NameFat *)es_fatptr_get (x))->attr
				  & ATTR_EXECUTABLE))
				continue;

			EsObject* val = NULL;
			EsObject *r = vm_dstack_known_and_get (vm, x, &val);
			if (es_object_get_type (r) == OPT_TYPE_DICT)
			{
				if (es_object_get_type (val) == OPT_TYPE_OPERATOR)
					ptrArrayUpdate (proc, i, es_object_ref (val), es_nil);
			}
		}
	}
}


/*
 * Array
 */
static EsObject*
array_new (unsigned int attr)
{
	ptrArray *a = ptrArrayNew ((ptrArrayDeleteFunc)es_object_unref);
	return es_fatptr_new  (OPT_TYPE_ARRAY, a, &attr);
}

static EsObject*
array_shared_new (EsObject* original, unsigned int attr)
{
	ptrArray *a = es_pointer_get (original);
	ptrArrayRef (a);
	return es_fatptr_new (OPT_TYPE_ARRAY, a, &attr);
}

static EsObject*
array_es_init_fat (void *fat, void *ptr, void *extra)
{
	ArrayFat *a = fat;
	a->attr = *((unsigned int *)extra);
	return es_false;
}

static void
array_es_free (void *ptr, void *fat)
{
	if (ptr)
		ptrArrayDelete ((ptrArray *)ptr);
}

static int
array_es_equal (const void *a, const void *afat, const void *b, const void *bfat)
{
	if (ptrArrayIsEmpty ((ptrArray *)a) && ptrArrayIsEmpty ((ptrArray*)b))
		return 1;
	else if (a == b)
		return 1;
	else
		return 0;
}

static void
array_es_print (const void *ptr, const void *fat, MIO *out)
{
	unsigned int c = ptrArrayCount ((ptrArray *)ptr);
	ArrayFat *a = (ArrayFat *)fat;
	mio_printf (out, "%c%c%c count: %u",
				(a->attr & ATTR_READABLE)  ? 'r': '-',
				(a->attr & ATTR_WRITABLE)  ? 'w': '-',
				(a->attr & ATTR_EXECUTABLE)? 'x': '-',
				c);
}

static void
array_op_add (EsObject* array, EsObject* elt)
{
	ptrArray *a = es_pointer_get (array);
	ptrArrayAdd (a, es_object_ref (elt));
}

static unsigned int
array_op_length (const EsObject* array)
{
	ptrArray *a = es_pointer_get (array);
	return ptrArrayCount (a);
}

static EsObject*
array_op_get (const EsObject* array, unsigned int n)
{
	ptrArray *a = es_pointer_get (array);
	unsigned int len = ptrArrayCount (a);
	if (n >= len)
		return OPT_ERR_RANGECHECK;
	return ptrArrayItem (a, n);
}

static void
array_op_put (EsObject* array, unsigned int n, EsObject *obj)
{
	ptrArray *a = es_pointer_get (array);
	ptrArrayUpdate (a, n,
					es_object_ref (obj), es_nil);
}


/*
 * Dictionary
 */
static unsigned int
opt_es_hash (const void * const key)
{
	const EsObject *k = key;

	if (es_integer_p (key))
		return es_integer_get (key);
	else if (es_boolean_p (key))
		return es_object_equal (key, es_true)? 1: 0;

	return hashPtrhash (k);
}

static bool
opt_es_eq (const void* a, const void* b)
{
	return es_object_equal (a, b);
}

static EsObject*
dict_new (unsigned int size, unsigned int attr)
{
	hashTable *t = hashTableNew (size,
								 opt_es_hash,
								 opt_es_eq,
								 (hashTableDeleteFunc)es_object_unref,
								 (hashTableDeleteFunc)es_object_unref);
	hashTableSetValueForUnknownKey (t, t, NULL);
	return es_fatptr_new  (OPT_TYPE_DICT, t, &attr);
}

static EsObject*
dict_es_init_fat (void *fat, void *ptr, void *extra)
{
	DictFat *a = fat;
	a->attr = *((unsigned int *)extra);
	return es_false;
}

static void
dict_es_free (void *ptr, void *fat)
{
	if (ptr)
		hashTableDelete ((hashTable *)ptr);
}

static int
dict_es_equal (const void *a, const void *afat, const void *b, const void *bfat)
{
	if (a == b)
		return 1;
	return 0;
}

static void
dict_es_print (const void *ptr, const void *fat, MIO *out)
{
	unsigned int c = hashTableCountItem ((hashTable *)ptr);
	DictFat *a = (DictFat *)fat;
	mio_printf (out, "%c%c%c count: %u",
				(a->attr & ATTR_READABLE)  ? 'r': '-',
				(a->attr & ATTR_WRITABLE)  ? 'w': '-',
				(a->attr & ATTR_EXECUTABLE)? 'x': '-',
				c);
}

static void
dict_op_def (EsObject* dict, EsObject *key, EsObject *val)
{
	hashTable *t = es_pointer_get (dict);
	Assert (t);
	Assert (!es_null (key));

	if (es_object_get_type (key) == OPT_TYPE_NAME)
		key = es_pointer_get (key);

	key = es_object_ref (key);
	val = es_object_ref (val);

	if (hashTableUpdateOrPutItem (t, key, val))
	{
		/* A key in the hashtable is reused. */
		es_object_unref (key);
	}
}

static bool
dict_op_undef (EsObject *dict, EsObject *key)
{
	hashTable *t = es_pointer_get (dict);
	Assert (t);

	if (es_object_get_type (key) == OPT_TYPE_NAME)
		key = es_pointer_get (key);

	/* TODO: handle the case key == NULL */
	return hashTableDeleteItem (t, key);
}

static bool
dict_op_known_and_get(EsObject* dict, EsObject *key, EsObject **val)
{
	hashTable *t = es_pointer_get (dict);
	Assert (t);

	if (es_object_get_type (key) == OPT_TYPE_STRING)
	{
		const char * cstr = opt_string_get_cstr (key);
		key = es_symbol_intern (cstr);
	}

	if (es_object_get_type (key) == OPT_TYPE_NAME)
		key = es_pointer_get (key);

	void *tmp = hashTableGetItem (t, key);
	if (tmp == t)
		return false;

	if (val)
		*val = tmp;
	return true;
}

static void
dict_op_clear (EsObject* dict)
{
	hashTable *h = es_pointer_get (dict);
	Assert (h);

	hashTableClear (h);
}


/*
 * Operator
 */
static EsObject*
operator_new (Operator op, const char *name, int arity, const char *help_str)
{
	OperatorExtra extra = { .name = name, .arity = arity, .help_str = help_str };
	return es_fatptr_new (OPT_TYPE_OPERATOR, op, &extra);
}

static EsObject*
operator_es_init_fat (void *fat, void *ptr, void *extra)
{
	OperatorFat *ofat = fat;

	if (!extra)
	{
		ofat->name = NULL;
		return es_true;
	}

	OperatorExtra *oextra = extra;
	const char *name = oextra->name;
	EsObject *o = es_symbol_intern (name);

	if (es_error_p (o))
		return o;
	ofat->name = o;
	ofat->arity = oextra->arity;
	ofat->help_str = oextra->help_str? eStrdup (oextra->help_str): NULL;
	return es_true;
}

static void
operator_es_free  (void *ptr, void *fat)
{
	OperatorFat *ofat = fat;
	if (ofat->help_str)
		eFree ((char *)ofat->help_str);
}

static void
operator_es_print (const void *ptr, const void *fat, MIO *out)
{
	OperatorFat *ofat = (OperatorFat *)fat;
	mio_printf (out, "--%s--", es_symbol_get (ofat->name));
}

/*
 * String
 */
static EsObject*
string_new   (vString *vstr)
{
	unsigned int attr = ATTR_READABLE|ATTR_WRITABLE;

	if (vstr == NULL)
		vstr = vStringNew ();

	return es_fatptr_new  (OPT_TYPE_STRING, vstr, &attr);
}

static EsObject*
string_es_init_fat (void *fat, void *ptr, void *extra)
{
	StringFat *s = fat;
	s->attr = *((unsigned int *)extra);
	return es_false;
}

static void
string_es_free  (void *ptr, void *fat)
{
	if (ptr)
		vStringDelete (ptr);
}

static int
string_es_equal (const void *a,
				 const void *afat,
				 const void *b,
				 const void *bfat)
{
	if (!strcmp (vStringValue ((vString *)a),
				 vStringValue ((vString *)b)))
		return 1;
	return 0;
}


static void
string_es_print (const void *ptr, const void *fat, MIO *out)
{
	char *v = vStringValue ((vString *)ptr);

	mio_putc (out, '(');
	while (*v != '\0')
	{
		switch (*v)
		{
		case '(':
		case ')':
		case '\\':
			mio_putc (out, '\\');
			mio_putc (out, *v);
			break;
		case '\n':
			mio_putc (out, '\\');
			mio_putc (out, 'n');
			break;
		case '\r':
			mio_putc (out, '\\');
			mio_putc (out, 'r');
			break;
		case '\t':
			mio_putc (out, '\\');
			mio_putc (out, 't');
			break;
		case '\f':
			mio_putc (out, '\\');
			mio_putc (out, 'f');
			break;
		case '\v':
			mio_putc (out, '\\');
			mio_putc (out, 'v');
			break;
		default:
			mio_putc (out, *v);
		}
		v++;
	}
	mio_putc (out, ')');
}


/*
 * Name
 */
static EsObject*
name_new     (EsObject* symbol, unsigned int attr)
{
	return es_fatptr_new (OPT_TYPE_NAME,
						  es_object_ref (symbol), &attr);
}

static EsObject*
name_newS    (const char*s, unsigned int attr)
{
	EsObject *sym = es_symbol_intern (s);
	return name_new (sym, attr);
}

static EsObject* name_newS_cb (const char*s, void *attr)
{
	return name_newS (s, *((unsigned int *)attr));
}

static EsObject*
name_es_init_fat (void *fat, void *ptr, void *extra)
{
	ArrayFat *a = fat;
	a->attr = *((unsigned int *)extra);
	return es_false;
}

static void
name_es_print (const void *ptr, const void *fat, MIO *out)
{
	const EsObject *symbol = ptr;
	const NameFat *qfat = fat;
	if (!(qfat->attr & ATTR_EXECUTABLE))
		mio_putc (out, '/');
	const char *name = es_symbol_get (symbol);
	mio_puts (out, name);
}

static void
name_es_free  (void *ptr, void *fat)
{
	if (ptr)
		es_object_unref (ptr);
}

static int
name_es_equal (const void *a, const void *afat,
			   const void *b, const void *bfat)
{
	const EsObject * asym = a;
	const EsObject * bsym = b;
	return es_object_equal (asym, bsym);
}

/*
 * Mark
 */
static EsObject*
mark_new (const char* mark)
{
	return es_pointer_new (OPT_TYPE_MARK,
						   eStrdup (mark));
}

static void
mark_es_print (const void *ptr, MIO *out)
{
	if (ptr == NULL || (strcmp (ptr, "mark") == 0))
		mio_printf (out, "-mark-");
	else
		mio_printf (out, "-mark:%s-", (char *)ptr);
}

static void
mark_es_free (void *ptr)
{
	if (ptr)
		eFree (ptr);
}

static int
mark_es_equal (const void *a, const void *b)
{
	return 1;
}


/*
 * Operator implementations
 */
#define GEN_PRINTER(NAME, BODY)								\
	static EsObject*										\
	NAME(OptVM *vm, EsObject *name)							\
	{														\
		EsObject * elt = ptrArrayRemoveLast (vm->ostack);	\
		BODY;												\
		mio_putc (vm->out, '\n');							\
		es_object_unref (elt);								\
		return es_false;									\
	}

GEN_PRINTER(op__print_objdict_rec, vm_print_full (vm, elt, false, 10))
GEN_PRINTER(op__print_objdict,     vm_print_full (vm, elt, false, 1))
GEN_PRINTER(op__print_object,      vm_print_full (vm, elt, false, 0))
GEN_PRINTER(op__print,             vm_print_full (vm, elt, true,  0))

static EsObject*
op__make_array (OptVM *vm, EsObject *name)
{
	int n0 = vm_ostack_counttomark (vm);
	if (n0 < 0)
		return OPT_ERR_UNMATCHEDMARK;
	unsigned int n = (unsigned int)n0;

	unsigned int count = vm_ostack_count (vm);
	Assert(count > n);

	EsObject *a = array_new (ATTR_READABLE | ATTR_WRITABLE);
	for (unsigned int i = count - n; i < count; i++)
	{
		EsObject *elt = ptrArrayItem (vm->ostack, i);
		array_op_add (a, elt);
	}

	ptrArrayDeleteLastInBatch (vm->ostack, n + 1);
	vm_ostack_push (vm, a);
	es_object_unref (a);
	return es_false;
}

static EsObject*
op__make_dict (OptVM *vm, EsObject *name)
{
	int n = vm_ostack_counttomark (vm);
	if (n < 0)
		return OPT_ERR_UNMATCHEDMARK;

	if (n % 2)
		return OPT_ERR_RANGECHECK;

	for (int i = 0; i < (n / 2); i++)
	{
		EsObject *key = ptrArrayItemFromLast (vm->ostack, 2 * i + 1);

		if (es_object_get_type (key) != OPT_TYPE_NAME
			&& es_object_get_type (key) != OPT_TYPE_STRING
			&& !es_integer_p (key) && !es_boolean_p (key))
			return OPT_ERR_TYPECHECK;
	}

	/* A hashtable grows automatically when its filling rate is
	 * grater than 80%. If we put elements between `<<' and `>>' to a dictionary
	 * initialized with the size equal to the number of elements, the dictionary
	 * grows once during putting them. Making a 1/0.8 times larger dictionary can
	 * avoid the predictable growing. */
	int size = (10 * (n > 0? (n / 2): 1)) / 8;

	EsObject *d = dict_new (size, ATTR_READABLE|ATTR_WRITABLE);
	for (int i = 0; i < (n / 2); i++)
	{
		EsObject *val = ptrArrayLast (vm->ostack);
		EsObject *key = ptrArrayItemFromLast (vm->ostack, 1);
		bool converted = false;

		if (es_object_get_type (key) == OPT_TYPE_STRING)
		{
			const char *cstr = opt_string_get_cstr (key);
			key = opt_name_new_from_cstr (cstr);
			converted = true;
		}
		dict_op_def (d, key, val);
		if (converted)
			es_object_unref (key);

		ptrArrayDeleteLastInBatch (vm->ostack, 2);
	}
	ptrArrayDeleteLast (vm->ostack); /* Remove the mark */
	vm_ostack_push (vm, d);
	es_object_unref (d);
	return es_false;
}

static EsObject*
op__help (OptVM *vm, EsObject *name)
{
	vm_help (vm, vm->out, NULL, NULL);
	return es_false;
}

static EsObject*
op_pstack (OptVM *vm, EsObject *name)
{
	unsigned int c = vm_ostack_count (vm);

	for (unsigned int i = c; i > 0; i--)
	{
		EsObject * elt = ptrArrayItem (vm->ostack, i - 1);
		vm_print (vm, elt);
		mio_putc (vm->out, '\n');
	}
	return es_false;
}

static EsObject*
op__newerror (OptVM *vm, EsObject *name)
{
	EsObject *newerror;
	if (dict_op_known_and_get (vm->error, OPT_KEY_newerror, &newerror))
		vm_ostack_push (vm, newerror);
	else
		vm_ostack_push (vm, es_false);
	return es_false;
}

static EsObject*
op__errorname (OptVM *vm, EsObject *name)
{
	EsObject *errorname;
	if (dict_op_known_and_get (vm->error, OPT_KEY_errorname, &errorname))
	{
		EsObject *sym = es_nil;
		if (!es_null (errorname))
		{
			const char *cstr = es_error_name(errorname);
			sym = opt_name_new_from_cstr (cstr);
		}
		vm_ostack_push (vm, sym);
		if (!es_null (errorname))
			es_object_unref (sym);
	}
	else
		vm_ostack_push (vm, es_nil);
	return es_false;
}

static EsObject*
op_quit (OptVM *vm, EsObject *name)
{
	int c = mio_getc (vm->in);
	if (!(c == '\n' || c == '\r' || c == EOF))
		mio_ungetc (vm->in, c);
	return OPT_ERR_QUIT;
}

static EsObject*
op_countexecstack (OptVM *vm, EsObject *name)
{
	unsigned int c = ptrArrayCount (vm->estack);
	int n = c;

	if (n < 0)
		return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */

	EsObject *nobj = es_integer_new (n);
	vm_ostack_push (vm, nobj);
	es_object_unref (nobj);

	return es_false;
}

static EsObject*
op__stack_common (OptVM *vm, EsObject *name, ptrArray *stack, EsObject *dstarrayobj,
				  bool ignoreLast)
{
	unsigned int c = ptrArrayCount (stack);
	ptrArray *a = es_pointer_get (dstarrayobj);

	if (ignoreLast && c == 0)
		return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */

	ptrArrayClear (a);
	for (unsigned int i = 0; i < c - (ignoreLast? 1: 0); i++)
	{
		EsObject *d = ptrArrayItem (stack, i);
		ptrArrayAdd (a, es_object_ref (d));
	}

	return es_false;
}

static EsObject*
op_execstack (OptVM *vm, EsObject *name)
{
	EsObject *obj = ptrArrayLast (vm->ostack);
	if (es_object_get_type (obj) != OPT_TYPE_ARRAY)
		return OPT_ERR_TYPECHECK;

	return op__stack_common (vm, name, vm->estack, obj, true);
}


/*
 * Operators for operand stack manipulation
 */
static EsObject*
op_pop (OptVM *vm, EsObject *name)
{
	ptrArrayDeleteLast (vm->ostack);
	return es_false;
}

static EsObject*
op_exch (OptVM *vm, EsObject *name)
{
	EsObject * top = ptrArrayRemoveLast (vm->ostack);
	EsObject * next = ptrArrayRemoveLast (vm->ostack);
	ptrArrayAdd (vm->ostack, top);
	ptrArrayAdd (vm->ostack, next);
	return es_false;
}

static EsObject*
op_dup (OptVM *vm, EsObject *name)
{
	EsObject * top = vm_ostack_top (vm);
	if (es_error_p (top))
		return top;
	vm_ostack_push (vm, top);
	return es_false;
}

static bool
dict_copy_cb (const void *key, void *value, void *user_data)
{
	hashTable *dst = user_data;
	hashTablePutItem (dst, es_object_ref ((void *)key), es_object_ref (value));
	return true;
}

static EsObject*
op__copy_compound (OptVM *vm, EsObject *name, unsigned int c, EsObject *obj2)
{
	int t = es_object_get_type (obj2);
	if (!(t == OPT_TYPE_ARRAY || t == OPT_TYPE_DICT || t == OPT_TYPE_STRING))
		return OPT_ERR_TYPECHECK;

	if (c < 2)
		return OPT_ERR_UNDERFLOW;

	EsObject *obj1 = ptrArrayItemFromLast (vm->ostack, 1);
	if (es_object_get_type (obj1) != t)
		return OPT_ERR_TYPECHECK;

	if (t == OPT_TYPE_ARRAY)
	{
		ptrArray *a1 = es_pointer_get (obj1);
		ptrArray *a2 = es_pointer_get (obj2);
		ptrArrayClear (a2);
		unsigned int len = ptrArrayCount (a1);
		for (unsigned int i = 0; i < len; i++)
		{
			EsObject *o = ptrArrayItem (a1, i);
			ptrArrayAdd (a2, es_object_ref (o));
		}
	}
	else if (t == OPT_TYPE_DICT)
	{
		hashTable *ht1 = es_pointer_get (obj1);
		hashTable *ht2 = es_pointer_get (obj2);
		hashTableClear (ht2);
		hashTableForeachItem (ht1, dict_copy_cb, ht2);
	}
	else
	{
		vString *str1 = es_pointer_get (obj1);
		vString *str2 = es_pointer_get (obj2);
		vStringCopy (str2, str1);
	}

	ptrArrayRemoveLast (vm->ostack);
	ptrArrayDeleteLast (vm->ostack);
	ptrArrayAdd (vm->ostack, obj2);
	return es_false;
}

static EsObject*
op_copy (OptVM *vm, EsObject *name)
{
	unsigned int c = vm_ostack_count (vm);

	if (c > 0)
	{
		EsObject * nobj = ptrArrayLast(vm->ostack);


		if (!es_integer_p (nobj))
			return op__copy_compound (vm, name, c, nobj);

		int n0 = es_integer_get (nobj);
		if (n0 < 0)
			return OPT_ERR_RANGECHECK;
		unsigned int n = (unsigned int)n0;

		c--;

		if (c < n)
			return OPT_ERR_UNDERFLOW;

		ptrArrayDeleteLast(vm->ostack);

		for (unsigned int i = c - n; i < c; i++)
		{
			EsObject * elt = ptrArrayItem (vm->ostack, i);
			vm_ostack_push (vm, elt);
		}
		return es_false;
	}
	return OPT_ERR_UNDERFLOW;
}

static EsObject*
op_index (OptVM *vm, EsObject *name)
{
	unsigned int c = vm_ostack_count (vm);

	EsObject * nobj = ptrArrayLast(vm->ostack);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get (nobj);
	if (n < 0)
		return OPT_ERR_RANGECHECK;
	if (c < (unsigned int)(n + 2))
		return OPT_ERR_UNDERFLOW;

	ptrArrayDeleteLast (vm->ostack);

	EsObject * elt = ptrArrayItem (vm->ostack, c - n - 2);
	vm_ostack_push (vm, elt);
	return es_false;

	return OPT_ERR_UNDERFLOW;
}

static EsObject*
op_roll (OptVM *vm, EsObject *name)
{
	unsigned int c = vm_ostack_count (vm);

	EsObject *jobj = ptrArrayLast (vm->ostack);
	if (!es_integer_p (jobj))
		return OPT_ERR_TYPECHECK;
	int j = es_integer_get (jobj);

	EsObject *nobj = ptrArrayItemFromLast (vm->ostack, 1);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;
	int n = es_integer_get (nobj);
	if (n < 0)
		return OPT_ERR_RANGECHECK;

	if ((((int)c) - 2) < n)
		return OPT_ERR_UNDERFLOW;

	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	if (j == 0)
		return es_false;

	unsigned int indx = c - 2 - n;
	EsObject *p;
	if (j > 0)
	{
		while (j-- != 0)
		{
			p = ptrArrayRemoveLast (vm->ostack);
			ptrArrayInsertItem (vm->ostack, indx, p);
		}
	}
	else
	{
		while (j++ != 0)
		{
			p = ptrArrayRemoveItem(vm->ostack, indx);
			ptrArrayAdd (vm->ostack, p);
		}

	}

	return es_false;
}

static EsObject*
op_clear (OptVM *vm, EsObject *name)
{
	ptrArrayClear (vm->ostack);

	return es_false;
}

static EsObject*
op_count (OptVM *vm, EsObject *name)
{
	unsigned int c = ptrArrayCount (vm->ostack);

	EsObject *n = es_integer_new ((int)c);
	ptrArrayAdd (vm->ostack, n);

	return es_false;
}

static EsObject*
op_mark (OptVM *vm, EsObject *name)
{
	EsObject *mark;
	if (es_object_equal (name, es_symbol_intern ("[")))
		mark = OPT_MARK_ARRAY;
	else if (es_object_equal (name, es_symbol_intern ("<<")))
		mark = OPT_MARK_DICT;
	else
		mark = OPT_MARK_MARK;
	vm_ostack_push (vm, mark);

	return es_false;
}

static EsObject*
op_cleartomark (OptVM *vm, EsObject *name)
{
	int r = vm_ostack_counttomark (vm);

	if (r < 0)
		return OPT_ERR_UNMATCHEDMARK;

	if (r < 0)
		return OPT_ERR_UNMATCHEDMARK;

	for (int i = 0; i <= r; i++)
		ptrArrayDeleteLast (vm->ostack);
	return es_false;
}

static EsObject*
op_counttomark (OptVM *vm, EsObject *name)
{
	int r = vm_ostack_counttomark (vm);

	if (r < 0)
		return OPT_ERR_UNMATCHEDMARK;

	ptrArrayAdd (vm->ostack, es_integer_new (r));
	return es_false;
}


/*
 * Arithmetic Operators
 */
#define INTEGER_BINOP(OP)									\
	EsObject *n0obj = ptrArrayLast (vm->ostack);			\
	if (!es_integer_p (n0obj))								\
		return OPT_ERR_TYPECHECK;							\
	int n0 = es_integer_get (n0obj);						\
															\
	EsObject *n1obj = ptrArrayItemFromLast (vm->ostack, 1);	\
	if (!es_integer_p (n1obj))								\
		return OPT_ERR_TYPECHECK;							\
	int n1 = es_integer_get (n1obj);						\
															\
	EsObject *r = es_integer_new (n1 OP n0);				\
	if (es_error_p (r))										\
		return r;											\
															\
	ptrArrayDeleteLastInBatch (vm->ostack, 2);				\
	ptrArrayAdd (vm->ostack, r);							\
	return es_false

static EsObject*
op_add (OptVM *vm, EsObject *name)
{
	INTEGER_BINOP(+);
}

static EsObject*
op_idiv (OptVM *vm, EsObject *name)
{
	INTEGER_BINOP(/);
}

static EsObject*
op_mod (OptVM *vm, EsObject *name)
{
	INTEGER_BINOP(%);
}

static EsObject*
op_mul (OptVM *vm, EsObject *name)
{
	INTEGER_BINOP(*);
}

static EsObject*
op_sub (OptVM *vm, EsObject *name)
{
	INTEGER_BINOP(-);
}

static EsObject*
op_abs (OptVM *vm, EsObject *name)
{
	EsObject *nobj = ptrArrayLast (vm->ostack);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get(nobj);
	if (n >= 0)
		return es_false;

	EsObject *r = es_integer_new (-n);
	if (es_error_p (r))
		return r;
	ptrArrayDeleteLast (vm->ostack);
	ptrArrayAdd (vm->ostack, r);
	return es_false;
}

static EsObject*
op_neg (OptVM *vm, EsObject *name)
{
	EsObject *nobj = ptrArrayLast (vm->ostack);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;
	int n = es_integer_get(nobj);
	EsObject *r = es_integer_new (-n);
	if (es_error_p (r))
		return r;
	ptrArrayDeleteLast (vm->ostack);
	ptrArrayAdd (vm->ostack, r);
	return es_false;
}


/*
 * Operators for array manipulation
 */
static EsObject*
op_array (OptVM *vm, EsObject *name)
{
	EsObject *nobj = ptrArrayLast (vm->ostack);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get (nobj);
	if (n < 0)
		return OPT_ERR_RANGECHECK;

	ptrArrayDeleteLast (vm->ostack);

	EsObject *array = array_new (ATTR_WRITABLE|ATTR_READABLE);
	ptrArray *a = es_pointer_get (array);
	for (int i = 0; i < n; i++)
		ptrArrayAdd (a, es_nil);
	vm_ostack_push (vm, array);
	es_object_unref (array);

	return es_false;
}

static EsObject*
op_astore (OptVM *vm, EsObject *name)
{
	EsObject *array = ptrArrayLast (vm->ostack);
	if (es_object_get_type (array) != OPT_TYPE_ARRAY)
		return OPT_ERR_TYPECHECK;

	unsigned int c = ptrArrayCount (vm->ostack);
	ptrArray *a = es_pointer_get (array);
	unsigned int l = ptrArrayCount (a);

	if (l == 0)
		return es_false;

	/* +1 is for the array itself. */
	if (c < (l + 1))
		return OPT_ERR_UNDERFLOW;

	ptrArrayClear (a);
	ptrArrayRemoveLast (vm->ostack);

	int i = l - 1;
	if (i < 0)
		return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */
	for (; i >= 0; i--)
	{
		EsObject * o = ptrArrayItemFromLast (vm->ostack, i);
		ptrArrayAdd (a, es_object_ref (o));
	}

	ptrArrayDeleteLastInBatch (vm->ostack, l);
	vm_ostack_push (vm, array);
	es_object_unref (array);
	return es_false;
}

static EsObject*
op_aload (OptVM *vm, EsObject *name)
{
	EsObject *array = ptrArrayLast (vm->ostack);
	if (es_object_get_type (array) != OPT_TYPE_ARRAY)
		return OPT_ERR_TYPECHECK;
	ptrArray *a = es_pointer_get (array);

	ptrArrayRemoveLast (vm->ostack);
	unsigned int c =  ptrArrayCount (a);
	for (unsigned int i = 0; i < c; i++)
	{
		EsObject *o = ptrArrayItem (a, i);
		vm_ostack_push (vm, o);
	}
	vm_ostack_push (vm, array);
	es_object_unref (array);
	return es_false;
}


/*
 * Operators for dictionary manipulation
 */
static EsObject*
op_dict (OptVM *vm, EsObject *name)
{
	EsObject *nobj = ptrArrayLast (vm->ostack);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get (nobj);
	if (n < 0)
		return OPT_ERR_RANGECHECK;
	else if (n == 0)
		n = 1;

	ptrArrayDeleteLast (vm->ostack);

	EsObject *dict = dict_new (n, ATTR_READABLE|ATTR_WRITABLE);
	vm_ostack_push (vm, dict);
	es_object_unref (dict);

	return es_false;
}

static EsObject*
op_def (OptVM *vm, EsObject *name)
{
	EsObject *val = ptrArrayLast (vm->ostack);
	EsObject *key = ptrArrayItemFromLast (vm->ostack, 1);
	/* TODO */
	if (es_object_get_type (key) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	vm_dict_def (vm, key, val);

	ptrArrayDeleteLastInBatch(vm->ostack, 2);

	return es_false;
}

static EsObject*
op_undef (OptVM *vm, EsObject *name)
{
	EsObject *key = ptrArrayLast (vm->ostack);
	EsObject *dict = ptrArrayItemFromLast (vm->ostack, 1);

	if (es_object_get_type (key) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	if (es_object_get_type (dict) != OPT_TYPE_DICT)
		return OPT_ERR_TYPECHECK;

	unsigned int attr = ((DictFat *)es_fatptr_get (dict))->attr;
	if (!(attr & ATTR_WRITABLE))
		return OPT_ERR_INVALIDACCESS;

	if (!dict_op_undef (dict, key))
		return es_error_set_object (OPT_ERR_UNDEFINED, key);

	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	return es_false;
}

static EsObject*
op_begin (OptVM *vm, EsObject *name)
{
	EsObject *d = ptrArrayLast (vm->ostack);
	if (es_object_get_type (d) != OPT_TYPE_DICT)
		return OPT_ERR_TYPECHECK;

	vm_dstack_push (vm, d);
	ptrArrayDeleteLast (vm->ostack);

	return es_false;
}

static EsObject*
op_end (OptVM *vm, EsObject *name)
{
	return vm_dstack_pop (vm);
}

static EsObject*
op_currentdict (OptVM *vm, EsObject *name)
{
	EsObject *dict = ptrArrayLast (vm->dstack);

	vm_ostack_push (vm, dict);

	return es_false;
}

static EsObject*
op_countdictstack (OptVM *vm, EsObject *name)
{
	unsigned int c = ptrArrayCount (vm->dstack);
	int n = c;

	if (n < 0)
		return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */

	EsObject *nobj = es_integer_new (n);
	vm_ostack_push (vm, nobj);
	es_object_unref (nobj);

	return es_false;
}

static EsObject*
op_dictstack (OptVM *vm, EsObject *name)
{
	EsObject *obj = ptrArrayLast (vm->ostack);
	if (es_object_get_type (obj) != OPT_TYPE_ARRAY)
		return OPT_ERR_TYPECHECK;

	return op__stack_common (vm, name, vm->dstack, obj, false);
}

static EsObject*
op_cleardictstack (OptVM *vm, EsObject *name)
{
	unsigned int d = ptrArrayCount (vm->dstack) - vm->dstack_protection;
	ptrArrayDeleteLastInBatch (vm->dstack, d);
	return es_false;
}

static EsObject*
op_where (OptVM *vm, EsObject *name)
{
	EsObject *key = ptrArrayLast (vm->ostack);
	if (es_object_get_type (key) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	EsObject *dict = vm_dstack_known_and_get (vm, key, NULL);
	ptrArrayDeleteLast (vm->ostack);

	if (es_object_get_type (dict) != OPT_TYPE_DICT)
	{
		vm_ostack_push (vm, es_false);
		return es_false;
	}
	else
	{
		vm_ostack_push (vm, dict);
		vm_ostack_push (vm, es_true);
		return es_false;
	}
}

static EsObject*
op_known (OptVM *vm, EsObject *name)
{
	EsObject *key  = ptrArrayLast (vm->ostack);
	EsObject *dict = ptrArrayItemFromLast (vm->ostack, 1);

	if (es_object_get_type (dict) != OPT_TYPE_DICT)
		return OPT_ERR_TYPECHECK;

	EsObject *b =  dict_op_known_and_get (dict, key, NULL)
		? es_true
		: es_false;
	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	vm_ostack_push (vm, b);

	return false;
}

static EsObject*
op_store (OptVM *vm, EsObject *name)
{
	EsObject *val = ptrArrayLast (vm->ostack);
	EsObject *key = ptrArrayItemFromLast (vm->ostack, 1);

	if (es_null (key))
		return OPT_ERR_TYPECHECK;
	if (es_object_get_type (key) != OPT_TYPE_NAME)
		return OPT_ERR_TYPECHECK;

	EsObject *dict = vm_dstack_known_and_get (vm, key, NULL);
	if (es_object_get_type (dict) != OPT_TYPE_DICT)
		vm_dict_def (vm, key, val);
	else if (!(((DictFat *)es_fatptr_get (dict))->attr & ATTR_WRITABLE))
		return OPT_ERR_INVALIDACCESS;
	else
		dict_op_def (dict, key, val);

	ptrArrayDeleteLastInBatch(vm->ostack, 2);
	return es_false;
}

static EsObject*
op_load (OptVM *vm, EsObject *name)
{
	EsObject *key = ptrArrayLast (vm->ostack);
	EsObject *val = NULL;
	EsObject *dict = vm_dstack_known_and_get (vm, key, &val);

	if (es_object_get_type (dict) != OPT_TYPE_DICT)
		return es_error_set_object (OPT_ERR_UNDEFINED, key);
	else
	{
		ptrArrayDeleteLast (vm->ostack);
		vm_ostack_push (vm, val);
		return es_false;
	}
}


/*
 * Operators for string manipulation
 */
static EsObject*
op_string (OptVM *vm, EsObject *name)
{
	EsObject *nobj = ptrArrayLast (vm->ostack);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;
	int n = es_integer_get (nobj);
	if (n < 0)
		return OPT_ERR_RANGECHECK;

	vString *s = vStringNew ();

	while (n-- > 0)
		vStringPut (s, ' ');

	EsObject *sobj = string_new (s);
	ptrArrayDeleteLast (vm->ostack);
	vm_ostack_push (vm, sobj);
	es_object_unref (sobj);
	return es_false;
}

static EsObject*
op__strstr_common (OptVM *vm, EsObject *name, bool fromTail)
{
	EsObject *seekobj = ptrArrayLast (vm->ostack);
	EsObject *strobj = ptrArrayItemFromLast (vm->ostack, 1);

	if (es_object_get_type (strobj) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;
	if (es_object_get_type (seekobj) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;

	vString *stringv = es_pointer_get (strobj);
	vString *seekv = es_pointer_get (seekobj);

	if (vStringLength (stringv) < vStringLength (seekv))
	{
		ptrArrayDeleteLast (vm->ostack);
		vm_ostack_push (vm, es_false);
		return es_false;
	}

	const char *stringc = vStringValue (stringv);
	const char *seekc = vStringValue (seekv);
	char *tmp = (fromTail? strrstr: strstr) (stringc, seekc);

	if (tmp == NULL)
	{
		ptrArrayDeleteLast (vm->ostack);
		vm_ostack_push (vm, es_false);
		return es_false;
	}

	unsigned int ud = tmp - stringc;
	int d = (int)ud;
	if (d < 0)
		return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */

	ptrArrayDeleteLast (vm->ostack);
	EsObject* dobj = es_integer_new (d);
	vm_ostack_push (vm, dobj);
	es_object_unref (dobj);
	vm_ostack_push (vm, es_true);
	return es_false;
}

static EsObject*
op__strstr (OptVM *vm, EsObject *name)
{
	return op__strstr_common (vm, name, false);
}

static EsObject*
op__strrstr (OptVM *vm, EsObject *name)
{
	return op__strstr_common (vm, name, true);
}

static EsObject*
op__strchr_common (OptVM *vm, EsObject *name, bool fromTail)
{
	EsObject *chrobj = ptrArrayLast (vm->ostack);
	EsObject *strobj = ptrArrayItemFromLast (vm->ostack, 1);

	if (! es_integer_p (chrobj))
		return OPT_ERR_TYPECHECK;

	unsigned int chr = (unsigned int)es_integer_get (chrobj);
	/* 0 is unacceptable. */
	if (! (0 < chr && chr < 256))
		return OPT_ERR_RANGECHECK;

	if (es_object_get_type (strobj) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;

	vString *strv = es_pointer_get (strobj);
	const char *str = vStringValue (strv);

	char * p = (fromTail? strrchr: strchr) (str, (int)chr);
	if (p)
	{
		int d = p - str;
		if (d < 0)
			return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */
		ptrArrayDeleteLast (vm->ostack);
		EsObject *dobj = es_integer_new (d);
		vm_ostack_push (vm, dobj);
		es_object_unref (dobj);
		vm_ostack_push (vm, es_true);
		return es_false;
	}
	else
	{
		ptrArrayDeleteLast (vm->ostack);
		vm_ostack_push (vm, es_false);
		return es_false;
	}
}

static EsObject*
op__strchr (OptVM *vm, EsObject *name)
{
	return op__strchr_common (vm, name, false);
}

static EsObject*
op__strrchr (OptVM *vm, EsObject *name)
{
	return op__strchr_common (vm, name, true);
}

static EsObject*
op__strpbrk_common (OptVM *vm, EsObject *name, bool from_tail)
{
	EsObject *acceptobj = ptrArrayLast (vm->ostack);
	EsObject *strobj = ptrArrayItemFromLast (vm->ostack, 1);

	if (es_object_get_type (strobj) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;
	if (es_object_get_type (acceptobj) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;

	vString *strv = es_pointer_get (strobj);
	vString *acceptv = es_pointer_get (acceptobj);

	const char *str = vStringValue (strv);
	char *p = (from_tail? strrpbrk: strpbrk) (str, vStringValue (acceptv));
	if (p)
	{
		int d = p - str;
		if (d < 0)
			return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */
		ptrArrayDeleteLast (vm->ostack);
		EsObject *dobj = es_integer_new (d);
		vm_ostack_push (vm, dobj);
		es_object_unref (dobj);
		vm_ostack_push (vm, es_true);
		return es_false;
	}
	else
	{
		ptrArrayDeleteLast (vm->ostack);
		vm_ostack_push (vm, es_false);
		return es_false;
	}
}

static EsObject*
op__strpbrk (OptVM *vm, EsObject *name)
{
	return op__strpbrk_common (vm, name, false);
}

static EsObject*
op__strrpbrk (OptVM *vm, EsObject *name)
{
	return op__strpbrk_common (vm, name, true);
}


/*
 * Relation, logical, and bit operators
 */
static EsObject*
op__eq_full (OptVM *vm, EsObject *name, bool inversion)
{
	EsObject *a = ptrArrayItemFromLast (vm->ostack, 0);
	EsObject *b = ptrArrayItemFromLast (vm->ostack, 1);

	bool eq = opt_es_eq (a, b);
	EsObject *r = (inversion? (!eq): eq)? es_true: es_false;
	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	vm_ostack_push (vm, r);
	return es_false;
}


/*
 * Relation, logical, and bit operators
 */
static EsObject*
op_eq (OptVM *vm, EsObject *name)
{
	op__eq_full (vm, name, false);
	return es_false;

}

static EsObject*
op_ne (OptVM *vm, EsObject *name)
{
	op__eq_full (vm, name, true);
	return es_false;

}

static EsObject*
op_true (OptVM *vm, EsObject *name)
{
	vm_ostack_push (vm, es_true);
	return es_false;

}

static EsObject*
op_false (OptVM *vm, EsObject *name)
{
	vm_ostack_push (vm, es_false);
	return es_false;
}

#define CMP_OP(OP)											\
	EsObject *o0 = ptrArrayLast (vm->ostack);				\
	EsObject *o1 = ptrArrayItemFromLast (vm->ostack, 1);	\
	EsObject *r;											\
															\
	if (es_integer_p (o0))									\
	{														\
		if (!es_integer_p (o1))								\
			return OPT_ERR_TYPECHECK;						\
															\
		int i0 = es_integer_get (o0);						\
		int i1 = es_integer_get (o1);						\
		r = es_boolean_new (i1 OP i0);						\
	}														\
	else if (es_object_get_type (o0) == OPT_TYPE_STRING)	\
	{														\
		if (es_object_get_type (o1) != OPT_TYPE_STRING)		\
			return OPT_ERR_TYPECHECK;						\
		vString *vs0 = es_pointer_get (o0);					\
		vString *vs1 = es_pointer_get (o1);					\
		const char *s0 = vStringValue (vs0);				\
		const char *s1 = vStringValue (vs1);				\
		int d = strcmp (s1, s0);							\
		r = es_boolean_new (d OP 0);						\
	}														\
	else													\
		return OPT_ERR_TYPECHECK;							\
	ptrArrayDeleteLastInBatch (vm->ostack, 2);				\
	vm_ostack_push (vm, r);									\
	es_object_unref (r);									\
	return es_false

static EsObject*
op_ge (OptVM *vm, EsObject *name)
{
	CMP_OP (>=);
}

static EsObject*
op_gt (OptVM *vm, EsObject *name)
{
	CMP_OP (>);
}

static EsObject*
op_le (OptVM *vm, EsObject *name)
{
	CMP_OP (<=);
}

static EsObject*
op_lt (OptVM *vm, EsObject *name)
{
	CMP_OP (<);
}

#define LOGBIT_OP(LOGOP, BITOP)									\
	EsObject *o0 = ptrArrayLast (vm->ostack);					\
	EsObject *o1 = ptrArrayItemFromLast (vm->ostack, 1);		\
	EsObject *r;												\
																\
	if (es_boolean_p (o0))										\
	{															\
		if (!es_boolean_p (o1))									\
			return OPT_ERR_TYPECHECK;							\
		bool b0 = es_boolean_get (o0);							\
		bool b1 = es_boolean_get (o1);							\
		bool b  = b0 LOGOP b1;									\
		r = es_boolean_new (b);									\
	}															\
	else if (es_integer_p (o0))									\
	{															\
		if (!es_integer_p (o1))									\
			return OPT_ERR_TYPECHECK;							\
		int i0 = es_integer_get (o0);							\
		int i1 = es_integer_get (o1);							\
		int i  = i0 BITOP i1;									\
		r = es_integer_new (i);									\
	}															\
	else														\
		return OPT_ERR_TYPECHECK;								\
																\
	ptrArrayDeleteLastInBatch (vm->ostack, 2);					\
	vm_ostack_push (vm, r);										\
	es_object_unref (r);										\
	return es_false;

static EsObject*
op_and (OptVM *vm, EsObject *name)
{
	LOGBIT_OP (&&, &);
}

static EsObject*
op_or (OptVM *vm, EsObject *name)
{
	LOGBIT_OP (||, |);
}

static EsObject*
op_xor (OptVM *vm, EsObject *name)
{
	LOGBIT_OP (!=, ^);
}

static EsObject*
op_not (OptVM *vm, EsObject *name)
{
	EsObject *o = ptrArrayLast (vm->ostack);
	EsObject *r;

	if (es_boolean_p (o))
		r = es_boolean_new (!es_boolean_get (o));
	else if (es_integer_p (o))
		r = es_integer_new (~ es_integer_get (o));
	else
		return OPT_ERR_TYPECHECK;

	ptrArrayDeleteLast (vm->ostack);
	vm_ostack_push (vm, r);
	es_object_unref (r);
	return es_false;
}

static EsObject*
op_bitshift (OptVM *vm, EsObject *name)
{
	EsObject *shiftobj = ptrArrayLast (vm->ostack);
	if (!es_integer_p (shiftobj))
		return OPT_ERR_TYPECHECK;

	EsObject *iobj = ptrArrayItemFromLast (vm->ostack, 1);
	if (!es_integer_p (iobj))
		return OPT_ERR_TYPECHECK;

	int shift = es_integer_get (shiftobj);
	int i = es_integer_get (iobj);

	EsObject *r;
	if (i == 0 || shift == 0)
		r = es_object_ref (iobj);
	else if (shift > 0)
		r = es_integer_new (i << shift);
	else
		r = es_integer_new (i >> -shift);
	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	vm_ostack_push (vm, r);
	es_object_unref (r);

	return es_false;
}


/*
 * Operators for control flow
 */
static EsObject*
op_exec (OptVM *vm, EsObject *name)
{
	EsObject *x = ptrArrayRemoveLast (vm->ostack);

	EsObject *e;
	if (es_object_get_type (x) == OPT_TYPE_ARRAY
		&& (((ArrayFat *)es_fatptr_get (x))->attr & ATTR_EXECUTABLE))
		e = vm_call_proc (vm, x);
	else
		e = vm_eval (vm, x);

	es_object_unref (x);
	return e;
}

static EsObject*
op_if (OptVM *vm, EsObject *name)
{
	EsObject *proc = ptrArrayLast (vm->ostack);
	if (!((es_object_get_type (proc) == OPT_TYPE_ARRAY)
		  && (((ArrayFat *)es_fatptr_get (proc))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	EsObject *b = ptrArrayItemFromLast	(vm->ostack, 1);
	if (!es_boolean_p (b))
		return OPT_ERR_TYPECHECK;

	if (es_object_equal (b, es_false))
	{
		ptrArrayDeleteLast (vm->ostack);
		ptrArrayDeleteLast (vm->ostack);
		return es_false;
	}

	es_object_ref (proc);
	ptrArrayDeleteLast (vm->ostack);
	ptrArrayDeleteLast (vm->ostack);
	EsObject *e = vm_call_proc (vm, proc);
	es_object_unref (proc);

	return e;
}

static EsObject*
op_ifelse (OptVM *vm, EsObject *name)
{
	EsObject *procf = ptrArrayLast (vm->ostack);
	if (!((es_object_get_type (procf) == OPT_TYPE_ARRAY)
		  && (((ArrayFat *)es_fatptr_get (procf))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	EsObject *proct = ptrArrayItemFromLast	(vm->ostack, 1);
	if (!((es_object_get_type (proct) == OPT_TYPE_ARRAY)
		  && (((ArrayFat *)es_fatptr_get (proct))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	EsObject *b = ptrArrayItemFromLast	(vm->ostack, 2);
	if (!es_boolean_p (b))
		return OPT_ERR_TYPECHECK;

	EsObject *p = (es_object_equal (b, es_false))? procf: proct;

	es_object_ref (p);
	ptrArrayDeleteLast (vm->ostack);
	ptrArrayDeleteLast (vm->ostack);
	ptrArrayDeleteLast (vm->ostack);
	EsObject *e = vm_call_proc (vm, p);
	es_object_unref (p);

	return e;
}

static EsObject*
op_loop (OptVM *vm, EsObject *name)
{
	EsObject *proc = ptrArrayLast (vm->ostack);
	if (!((es_object_get_type (proc) == OPT_TYPE_ARRAY)
		  && (((ArrayFat *)es_fatptr_get (proc))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	es_object_ref (proc);
	ptrArrayDeleteLast (vm->ostack);

	EsObject *e;
	while (true)
	{
		e = vm_call_proc (vm, proc);
		if (es_object_equal (e, OPT_ERR_INVALIDEXIT))
		{
			dict_op_def (vm->error, OPT_KEY_newerror, es_false);
			e = es_false;
			break;
		}
		else if (es_error_p (e))
			break;
	}
	es_object_unref (proc);
	return e;
}

static EsObject*
op_exit (OptVM *vm, EsObject *name)
{
	return OPT_ERR_INVALIDEXIT;
}

static EsObject*
op_repeat (OptVM *vm, EsObject *name)
{
	EsObject *proc = ptrArrayLast (vm->ostack);
	if (!((es_object_get_type (proc) == OPT_TYPE_ARRAY)
		  && (((ArrayFat *)es_fatptr_get (proc))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	EsObject *nobj = ptrArrayItemFromLast (vm->ostack, 1);
	if (!es_integer_p (nobj))
		return OPT_ERR_TYPECHECK;

	int n = es_integer_get (nobj);
	if (n < 0)
		return OPT_ERR_RANGECHECK;

	es_object_ref (proc);
	ptrArrayDeleteLast (vm->ostack);
	ptrArrayDeleteLast (vm->ostack);

	EsObject *e = es_false;
	for (int i = 0; i < n; i++)
	{
		e = vm_call_proc (vm, proc);
		if (es_object_equal (e, OPT_ERR_INVALIDEXIT))
		{
			dict_op_def (vm->error, OPT_KEY_newerror, es_false);
			e = es_false;
			break;
		}
		else if (es_error_p (e))
			break;
	}
	es_object_unref (proc);
	return e;
}

static EsObject*
op_stop (OptVM *vm, EsObject *name)
{
	return OPT_ERR_STOPPED;
}

static EsObject*
op_stopped (OptVM *vm, EsObject *name)
{
	EsObject *e = op_exec (vm, name);
	vm_ostack_push (vm, es_error_p (e)? es_true: es_false);
	return es_false;
}

static EsObject*
op_for (OptVM *vm, EsObject *name)
{
	EsObject *proc = ptrArrayLast (vm->ostack);
	if (!((es_object_get_type (proc) == OPT_TYPE_ARRAY)
		  && (((ArrayFat *)es_fatptr_get (proc))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	EsObject *limitobj = ptrArrayItemFromLast (vm->ostack, 1);
	if (! es_integer_p (limitobj))
		return OPT_ERR_TYPECHECK;
	int limit = es_integer_get (limitobj);

	EsObject *incrementobj = ptrArrayItemFromLast (vm->ostack, 2);
	if (! es_integer_p (incrementobj))
		return OPT_ERR_TYPECHECK;
	int increment = es_integer_get (incrementobj);

	EsObject *initialobj = ptrArrayItemFromLast (vm->ostack, 3);
	if (! es_integer_p (initialobj))
		return OPT_ERR_TYPECHECK;
	int initial = es_integer_get (initialobj);

	ptrArrayRemoveLast (vm->ostack);
	ptrArrayDeleteLastInBatch (vm->ostack, 3);

	EsObject *r = es_false;
	for (int i = initial;
		 (increment >= 0) ? (i <= limit) : (i >= limit);
		 i += increment)
	{
		EsObject *iobj = es_integer_new (i);
		vm_ostack_push (vm, iobj);
		r = vm_call_proc (vm, proc);
		es_object_unref (iobj);

		if (es_object_equal (r, OPT_ERR_INVALIDEXIT))
		{
			dict_op_def (vm->error, OPT_KEY_newerror, es_false);
			r = es_false;
			break;
		}
		if (es_error_p (r))
			break;
	}
	es_object_unref (proc);
	return r;
}


/*
 * Operators for type, attribute and their conversion
 */
static const char*
get_type_name (EsObject *o)
{
	const char *n;

	if (o == es_nil)
		n = "nulltype";
	else if (es_boolean_p (o))
		n = "booleantype";
	else if (es_integer_p (o))
		n = "integertype";
	else
	{
		int t = es_object_get_type (o);
		n = es_type_get_name (t);
	}

	return n;
}

static EsObject*
op_type (OptVM *vm, EsObject *name)
{
	EsObject *o = ptrArrayRemoveLast (vm->ostack);
	const char *n;

	n = get_type_name (o);

	EsObject *p = name_newS (n, ATTR_EXECUTABLE|ATTR_READABLE);
	vm_ostack_push (vm, p);
	es_object_unref (p);
	es_object_unref (o);
	return es_false;
}

static EsObject*
op_cvn (OptVM *vm, EsObject *name)
{
	EsObject *o = ptrArrayLast (vm->ostack);
	if (es_object_get_type (o) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;

	vString *vstr = es_pointer_get (o);
	const char *cstr = vStringValue (vstr);
	StringFat *sfat = es_fatptr_get (o);
	EsObject *n = name_newS (cstr, sfat->attr);
	ptrArrayDeleteLast (vm->ostack);
	vm_ostack_push (vm, n);
	es_object_unref (n);
	return es_false;
}

static EsObject *
op_cvs (OptVM *vm, EsObject *name)
{
	EsObject *o = ptrArrayLast (vm->ostack);
	if (es_object_get_type (o) != OPT_TYPE_STRING)
		return OPT_ERR_TYPECHECK;

	EsObject *any = ptrArrayItemFromLast (vm->ostack, 1);
	vString *vstr = es_pointer_get (o);
	int t = es_object_get_type (any);

	if (t == OPT_TYPE_STRING)
	{
		vString *vany = es_pointer_get (any);
		vStringCopy (vstr, vany);
	}
	else if (t == OPT_TYPE_NAME || t == ES_TYPE_SYMBOL)
	{
		if (t == OPT_TYPE_NAME)
			any = es_pointer_get (any);

		const char *cany = es_symbol_get (any);
		vStringCopyS (vstr, cany);
	}
	else if (t == ES_TYPE_INTEGER)
	{
		int iany = es_integer_get (any);
#define buf_len 13
		char buf[buf_len];
		if (!(snprintf (buf, buf_len, "%d", iany) > 0))
			buf [0] = '\0';
		vStringCopyS (vstr, buf);
	}
	else if (t == ES_TYPE_BOOLEAN)
		vStringCopyS (vstr, any == es_true? "true": "false");
	else
	{
		const char *type_name = get_type_name (any);
		vStringCopyS (vstr, "--");
		vStringCatS (vstr, type_name);
		vStringCatS (vstr, "--");
	}

	es_object_ref (o);
	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	vm_ostack_push (vm, o);
	es_object_unref (o);

	return es_false;
}

static EsObject*
op_cvx (OptVM *vm, EsObject *name)
{
	EsObject *o = ptrArrayLast (vm->ostack);

	if (es_object_get_type (o) == OPT_TYPE_ARRAY
		&& (! (((ArrayFat *)es_fatptr_get (o))->attr & ATTR_EXECUTABLE)))
	{
		EsObject *xarray = array_shared_new (o,
											 ((ArrayFat *)es_fatptr_get (o))->attr | ATTR_EXECUTABLE);
		ptrArrayDeleteLast (vm->ostack);
		vm_ostack_push (vm, xarray);
		es_object_unref(xarray);

	}
	else if (es_object_get_type (o) == OPT_TYPE_NAME
			 && (! (((NameFat *)es_fatptr_get (o))->attr & ATTR_EXECUTABLE)))
	{
		EsObject *symbol = es_pointer_get (o);
		EsObject *xname = name_new (symbol, ((NameFat *)es_fatptr_get (o))->attr | ATTR_EXECUTABLE);
		ptrArrayDeleteLast (vm->ostack);
		vm_ostack_push (vm, xname);
		es_object_unref(xname);
	}

	return es_false;
}


/*
 * Misc operators
 */
static EsObject*
op_null (OptVM *vm, EsObject *name)
{
	vm_ostack_push (vm, es_nil);
	return es_false;
}

static EsObject*
op_bind (OptVM *vm, EsObject *name)
{
	EsObject *proc = ptrArrayLast (vm->ostack);
	if (!((es_object_get_type (proc) == OPT_TYPE_ARRAY)
		  && (((ArrayFat *)es_fatptr_get (proc))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	vm_bind_proc (vm, es_pointer_get (proc));
	return es_false;
}


/*
 * Methods for compound objects
 */
static EsObject*
op_length (OptVM *vm, EsObject *name)
{
	EsObject *o = ptrArrayLast (vm->ostack);
	unsigned int c;

	int t = es_object_get_type (o);
	if (t == OPT_TYPE_ARRAY)
	{
		ptrArray *a = es_pointer_get (o);
		c = ptrArrayCount (a);
	}
	else if (t == OPT_TYPE_DICT)
	{
		hashTable *h = es_pointer_get (o);
		c = hashTableCountItem (h);
	}
	else if (t == OPT_TYPE_STRING)
	{
		vString *s = es_pointer_get (o);
		c = (unsigned int)vStringLength (s);
	}
	else if (t == OPT_TYPE_NAME)
	{
		EsObject *sym = es_pointer_get (o);
		const char* cstr = es_symbol_get (sym);
		c = (unsigned int) strlen (cstr);
	}
	else
		return OPT_ERR_TYPECHECK;

	int n = c;
	if (n < 0)
		return OPT_ERR_INTERNALERROR; /* TODO: integer overflow */

	ptrArrayDeleteLast (vm->ostack);
	EsObject *nobj = es_integer_new (n);
	vm_ostack_push (vm, nobj);
	es_object_unref (nobj);

	return es_false;
}

static EsObject*
op__get_array (OptVM *vm, EsObject *name,
			   EsObject *k, EsObject *obj)
{
	if (!es_integer_p (k))
		return OPT_ERR_TYPECHECK;
	int n = es_integer_get (k);
	if (n < 0)
		return OPT_ERR_RANGECHECK;
	EsObject *r = array_op_get (obj, (unsigned int)n);
	if (es_error_p (r))
		return r;
	es_object_ref (r);
	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	vm_ostack_push (vm, r);
	es_object_unref (r);
	return es_false;
}

static EsObject*
op__get_dict (OptVM *vm, EsObject *name,
			  EsObject *k, EsObject *obj)
{
	EsObject *v = NULL;
	if (!dict_op_known_and_get (obj, k, &v))
		return es_error_set_object (OPT_ERR_UNDEFINED, k);
	es_object_ref (v);
	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	vm_ostack_push (vm, v);
	es_object_unref (v);
	return es_false;
}

static EsObject*
op__get_str (OptVM *vm, EsObject *name,
			 EsObject *k, EsObject *obj)
{
	if (!es_integer_p (k))
		return OPT_ERR_TYPECHECK;
	int n = es_integer_get (k);
	if (n < 0)
		return OPT_ERR_RANGECHECK;
	vString *s = es_pointer_get (obj);
	unsigned int len = vStringLength (s);
	if ((unsigned int)n >= len)
		return OPT_ERR_RANGECHECK;
	unsigned char chr = vStringChar (s, n);
	ptrArrayDeleteLastInBatch (vm->ostack, 2);
	EsObject *chrobj = es_integer_new (chr);
	vm_ostack_push (vm, chrobj);
	es_object_unref (chrobj);
	return es_false;
}

static EsObject*
op_get (OptVM *vm, EsObject *name)
{
	EsObject *k = ptrArrayLast (vm->ostack);
	EsObject *obj = ptrArrayItemFromLast (vm->ostack, 1);

	int t = es_object_get_type (obj);
	if (t == OPT_TYPE_ARRAY)
		return op__get_array (vm, name, k, obj);
	else if (t == OPT_TYPE_DICT)
		return op__get_dict (vm, name, k, obj);
	else if (t == OPT_TYPE_STRING)
		return op__get_str (vm, name, k, obj);

	return OPT_ERR_TYPECHECK;
}

static EsObject*
op__put_array (OptVM *vm, EsObject *name,
			   EsObject *v, EsObject *k, EsObject *array)
{
	if (!es_integer_p (k))
		return OPT_ERR_TYPECHECK;
	int index = es_integer_get (k);
	if (index < 0)
		return OPT_ERR_RANGECHECK;

	array_op_put (array, (unsigned int)index, v);
	ptrArrayDeleteLastInBatch (vm->ostack, 3);
	return es_false;
}

static EsObject*
op__put_dict (OptVM *vm, EsObject *name,
			  EsObject *v, EsObject *k, EsObject *dict)
{
	EsObject *key = k;

	if (es_null (key))
		return OPT_ERR_TYPECHECK;

	if (es_object_get_type (key) == OPT_TYPE_STRING)
	{
		const char *cstr = opt_string_get_cstr (key);
		key = opt_name_new_from_cstr (cstr);
	}

	if (es_object_get_type (key) != OPT_TYPE_NAME
		&& !es_integer_p (key) && !es_boolean_p (key))
		return OPT_ERR_TYPECHECK;

	dict_op_def (dict, key, v);
	if (key != k)
		es_object_unref (key);
	ptrArrayDeleteLastInBatch (vm->ostack, 3);
	return es_false;
}

static EsObject*
op__put_str (OptVM *vm, EsObject *name,
			 EsObject *v, EsObject *k, EsObject *str)
{
	if (!es_integer_p (v))
		return OPT_ERR_TYPECHECK;
	int c = es_integer_get (v);
	if (!(c >= 0 && c < 256))
		return OPT_ERR_RANGECHECK;
	if (!es_integer_p (k))
		return OPT_ERR_TYPECHECK;
	int index = es_integer_get (k);
	if (index < 0)
		return OPT_ERR_RANGECHECK;

	vString *vstr = es_pointer_get (str);
	size_t len    = vStringLength (vstr);
	if (len > (size_t)index)
	{
		if (c == 0)
			vStringTruncate (vstr, (size_t)index);
		else
			vStringChar(vstr, index) = (char)c;
	}
	else
	{
		size_t d = index - len;
		for (size_t i = 0; i < d; i++)
			vStringPut (vstr, ' ');
		if (c != 0)
			vStringPut (vstr, c);
	}

	ptrArrayDeleteLastInBatch (vm->ostack, 3);
	return es_false;
}

static EsObject*
op_put (OptVM *vm, EsObject *name)
{
	EsObject *v = ptrArrayLast (vm->ostack);
	EsObject *k = ptrArrayItemFromLast (vm->ostack, 1);
	EsObject *obj = ptrArrayItemFromLast (vm->ostack, 2);

	int t = es_object_get_type (obj);
	if (t == OPT_TYPE_ARRAY)
		return op__put_array (vm, name, v, k, obj);
	else if (t == OPT_TYPE_DICT)
		return op__put_dict (vm, name, v, k, obj);
	else if (t == OPT_TYPE_STRING)
		return op__put_str (vm, name, v, k, obj);

	return OPT_ERR_TYPECHECK;
}

static EsObject*
op__forall_array (OptVM *vm, EsObject *name,
				  EsObject *proc, EsObject *obj)
{
	ptrArray *a = es_pointer_get (obj);
	unsigned int c = ptrArrayCount (a);

	EsObject *e = es_false;
	for (unsigned int i = 0; i < c; i++)
	{
		EsObject *o = ptrArrayItem (a, i);
		es_object_ref (o);
		vm_ostack_push (vm, o);
		e = vm_call_proc (vm, proc);
		es_object_unref (o);
		if (es_error_p (e))
			break;
	}

	return e;
}

struct dictForallData {
	OptVM *vm;
	EsObject *proc;
	EsObject *err;
};

static bool
dict_forall_cb (const void *key, void *value, void *user_data)
{
	bool r = true;
	EsObject *k = (EsObject *)key;
	EsObject *v = value;
	struct dictForallData *d = user_data;

	/* TODO */
	if (es_symbol_p (k))
		k = name_new (k, ATTR_READABLE);
	else
		es_object_ref ((EsObject *)k);
	es_object_ref (v);

	vm_ostack_push (d->vm, (EsObject *)k);
	vm_ostack_push (d->vm, v);
	EsObject *e = vm_call_proc (d->vm, d->proc);
	if (es_error_p (e))
	{
		d->err = e;
		r = false;
	}
	es_object_unref ((EsObject *)k);
	es_object_unref (v);

	return r;
}

static EsObject*
op__forall_dict (OptVM *vm, EsObject *name,
				 EsObject *proc, EsObject *obj)
{
	EsObject *r = es_false;
	hashTable *ht = es_pointer_get (obj);
	struct dictForallData data = {
		.vm   = vm,
		.proc = proc
	};

	if (!hashTableForeachItem (ht, dict_forall_cb, &data))
		r = data.err;

	return r;
}

static EsObject*
op__forall_string (OptVM *vm, EsObject *name,
				   EsObject *proc, EsObject *obj)
{
	vString *s = es_pointer_get (obj);
	unsigned int c = vStringLength (s);

	EsObject *e = es_false;
	for (unsigned int i = 0; i < c; i++)
	{
		unsigned char chr = vStringChar (s, i);
		EsObject *o = es_integer_new (chr);
		vm_ostack_push (vm, o);
		es_object_unref (o);
		e = vm_call_proc (vm, proc);
		if (es_error_p (e))
			break;
	}

	return e;
}

static EsObject*
op_forall (OptVM *vm, EsObject *name)
{
	EsObject *proc = ptrArrayLast (vm->ostack);
	if (!(es_object_get_type (proc) == OPT_TYPE_ARRAY
		  && (((ArrayFat *)es_fatptr_get (proc))->attr & ATTR_EXECUTABLE)))
		return OPT_ERR_TYPECHECK;

	EsObject *obj = ptrArrayItemFromLast (vm->ostack, 1);

	int t = es_object_get_type (obj);
	EsObject * (* proc_driver) (OptVM *, EsObject *,
								EsObject *, EsObject *) = NULL;
	if (t == OPT_TYPE_ARRAY)
		proc_driver = op__forall_array;
	else if (t == OPT_TYPE_DICT)
		proc_driver = op__forall_dict;
	else if (t == OPT_TYPE_STRING)
		proc_driver = op__forall_string;
	else
		return OPT_ERR_TYPECHECK;

	ptrArrayRemoveLast (vm->ostack);
	ptrArrayRemoveLast (vm->ostack);
	EsObject *e = (*proc_driver) (vm, name, proc, obj);
	es_object_unref (proc);
	es_object_unref (obj);

	if (es_object_equal (e, OPT_ERR_INVALIDEXIT))
	{
		dict_op_def (vm->error, OPT_KEY_newerror, es_false);
		e = es_false;
	}
	return e;
}

static EsObject*
op__putinterval_array (OptVM *vm, EsObject *name,
					   ptrArray *srca, unsigned int index, ptrArray *dsta)
{
	unsigned int dlen = ptrArrayCount (dsta);
	unsigned int slen = ptrArrayCount (srca);
	if (dlen > index)
	{
		unsigned d = dlen - index;
		if (d <= slen)
		{
			ptrArrayDeleteLastInBatch (dsta, d);
			for (unsigned int i = 0; i < slen; i++)
				ptrArrayAdd (dsta, es_object_ref (ptrArrayItem (srca, i)));
			return es_false;
		}
		else
		{
			for (unsigned int i = 0; i < slen; i++)
				ptrArrayUpdate (dsta, index + i,
								es_object_ref (ptrArrayItem (srca, i)),
								es_nil);
			return es_false;
		}
	}
	else if (dlen == index)
	{
		for (unsigned int i = 0; i < slen; i++)
			ptrArrayAdd (dsta, es_object_ref (ptrArrayItem (srca, i)));
		return es_false;
	}
	else
		return OPT_ERR_RANGECHECK;
}

static EsObject*
op__putinterval_string (OptVM *vm, EsObject *name,
						vString *srcv, unsigned int index, vString *dstv)
{
	unsigned int dlen = vStringLength (dstv);
	if (dlen > index)
	{
		unsigned int d = dlen - index;
		unsigned int slen = vStringLength (srcv);
		if (d <= slen)
		{
			vStringTruncate (dstv, index);
			vStringCat (dstv, srcv);
			return es_false;
		}
		else
		{
			for (unsigned int i = 0; i < slen; i++)
				vStringChar (dstv, index + i) = vStringChar (srcv, i);
			return es_false;
		}
	}
	else if (dlen == index)
	{
		vStringCat (dstv, srcv);
		return es_false;
	}
	else
		return OPT_ERR_RANGECHECK;
}

static EsObject*
op_putinterval (OptVM *vm, EsObject *name)
{
	EsObject *src = ptrArrayLast (vm->ostack);
	EsObject *indexobj = ptrArrayItemFromLast (vm->ostack, 1);
	EsObject *dst = ptrArrayItemFromLast (vm->ostack, 2);

	int t = es_object_get_type (src);
	if (t == OPT_TYPE_ARRAY || t == OPT_TYPE_STRING)
	{
		if (!es_integer_p (indexobj))
			return OPT_ERR_TYPECHECK;
		if (es_object_get_type (dst) != t)
			return OPT_ERR_TYPECHECK;
	}
	else
		return OPT_ERR_TYPECHECK;

	int index0 = es_integer_get (indexobj);
	if (index0 < 0)
		return OPT_ERR_RANGECHECK;
	unsigned int index = (size_t)index0;

	EsObject *r;
	if (t == OPT_TYPE_ARRAY)
		r = op__putinterval_array (vm, name,
								   es_pointer_get (src),
								   index,
								   es_pointer_get (dst));
	else
		r = op__putinterval_string (vm, name,
									es_pointer_get (src),
									index,
									es_pointer_get (dst));

	if (!es_error_p (r))
		ptrArrayDeleteLastInBatch (vm->ostack, 3);

	return r;
}

static EsObject*
op__copyinterval_array (OptVM *vm, EsObject *name,
						ptrArray *dsta,
						unsigned int count,
						unsigned int index,
						ptrArray *srca)
{
	unsigned int srcl = ptrArrayCount (srca);

	if (index > srcl)
		return OPT_ERR_RANGECHECK;

	if ((index + count) > srcl)
		return OPT_ERR_RANGECHECK;

	for (unsigned int i = index; i < index + count; i++)
		ptrArrayAdd (dsta, es_object_ref (ptrArrayItem (srca, i)));
	return es_false;
}

static EsObject*
op__copyinterval_string (OptVM *vm, EsObject *name,
						 vString *dsts,
						 unsigned int count,
						 unsigned int index,
						 vString *srcs)
{
	unsigned int srcl = vStringLength (srcs);

	if (index > srcl)
		return OPT_ERR_RANGECHECK;

	if ((index + count) > srcl)
		return OPT_ERR_RANGECHECK;

	vStringNCatSUnsafe (dsts, vStringValue (srcs) + index, count);
	return es_false;
}

static EsObject*
op__copyinterval (OptVM *vm, EsObject *name)
{
	EsObject *dstobj = ptrArrayLast (vm->ostack);
	EsObject *countobj = ptrArrayItemFromLast (vm->ostack, 1);
	EsObject *indexobj = ptrArrayItemFromLast (vm->ostack, 2);
	EsObject *srcobj = ptrArrayItemFromLast (vm->ostack, 3);

	int t = es_object_get_type (dstobj);
	if (! (t == OPT_TYPE_ARRAY || t == OPT_TYPE_STRING))
		return OPT_ERR_TYPECHECK;
	if (t != es_object_get_type (srcobj))
		return OPT_ERR_TYPECHECK;

	if (!es_integer_p (countobj))
		return OPT_ERR_TYPECHECK;
	if (!es_integer_p (indexobj))
		return OPT_ERR_TYPECHECK;

	int count0 = es_integer_get (countobj);
	if (count0 < 0)
		return OPT_ERR_RANGECHECK;
	unsigned int count = (unsigned int)count0;

	int index0 = es_integer_get (indexobj);
	if (index0 < 0)
		return OPT_ERR_RANGECHECK;
	unsigned int index = (size_t)index0;

	EsObject* r;
	if (t == OPT_TYPE_ARRAY)
		r = op__copyinterval_array (vm, name,
									es_pointer_get (dstobj),
									count,
									index,
									es_pointer_get (srcobj));
	else
		r = op__copyinterval_string (vm, name,
									 es_pointer_get (dstobj),
									 count,
									 index,
									 es_pointer_get (srcobj));

	if (es_error_p (r))
		return r;

	es_object_ref (dstobj);
	ptrArrayDeleteLastInBatch (vm->ostack, 4);
	vm_ostack_push (vm, dstobj);
	es_object_unref (dstobj);
	return r;
}
