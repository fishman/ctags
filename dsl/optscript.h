/*
*   Copyright (c) 2020, Masatake YAMATO
*   Copyright (c) 2020, Red Hat, Inc.
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*/

#ifndef OPTSCRIPT_H
#define OPTSCRIPT_H

#include "general.h"

#include "es.h"
#include "mio.h"
#include "ptrarray.h"

typedef struct sOptVM OptVM;
typedef EsObject* (* OptOperatorFn) (OptVM *, EsObject *);

struct OptHelpExtender {
	void        (* add)          (ptrArray *, void *);
	const char* (* get_help_str) (EsObject *, void *);
};

int       opt_init (void);

OptVM    *opt_vm_new          (MIO *in, MIO *out, MIO *err);
void      opt_vm_delete       (OptVM *vm);

EsObject *opt_vm_read         (OptVM *vm, MIO *in);
EsObject *opt_vm_eval         (OptVM *vm, EsObject *obj);
void      opt_vm_report_error (OptVM *vm, EsObject *eobj, MIO *err);

void     *opt_vm_set_app_data (OptVM *vm, void *app_data);
void     *opt_vm_get_app_data (OptVM *vm);

char     *opt_vm_set_prompt   (OptVM *vm, char *prompt);
void      opt_vm_print_prompt (OptVM *vm);

int       opt_vm_help         (OptVM *vm, MIO *out,
							   struct OptHelpExtender *extop, void *data);

void      opt_vm_clear       (OptVM *vm, bool clear_app_data);
void      opt_vm_dstack_push (OptVM *vm, EsObject *dict);
void      opt_vm_dstack_pop  (OptVM *vm);

EsObject*  opt_vm_ostack_top  (OptVM *vm);
EsObject*  opt_vm_ostack_peek  (OptVM *vm, int index_from_top);
EsObject*  opt_vm_ostack_pop  (OptVM *vm);
void       opt_vm_ostack_push (OptVM *vm, EsObject *obj);
unsigned int opt_vm_ostack_count (OptVM *vm);

EsObject *opt_dict_new       (unsigned int size);
bool      opt_dict_known_and_get_cstr (EsObject *dict, const char* name, EsObject **val);
bool      opt_dict_known_and_get (EsObject *dict, EsObject *key, EsObject **val);
bool      opt_dict_foreach   (EsObject *dict, bool (* fn) (EsObject *, EsObject *, void*), void *data);
void      opt_dict_def       (EsObject *dict, EsObject *sym, EsObject *val);
bool      opt_dict_undef     (EsObject *dict, EsObject *sym);
void      opt_dict_clear     (EsObject *dict);

EsObject *opt_array_new      (void);
EsObject *opt_array_get      (const EsObject *array, unsigned int index);
void      opt_array_put      (EsObject *array, unsigned int index, EsObject *obj);
void      opt_array_add      (EsObject *array, EsObject *elt);

unsigned int opt_array_length(const EsObject *array);

EsObject *opt_operator_new   (OptOperatorFn op, const char *name, int arity, const char *help_str);

EsObject *opt_string_new_from_cstr (const char *cstr);
const char* opt_string_get_cstr (const EsObject *str);

EsObject *opt_name_new_from_cstr (const char *cstr);
const char* opt_name_get_cstr (const EsObject *name);

extern EsObject *OPT_ERR_TYPECHECK;
extern EsObject *OPT_ERR_QUIT;
extern EsObject *OPT_ERR_RANGECHECK;
extern EsObject *OPT_ERR_UNDERFLOW;

extern int OPT_TYPE_ARRAY;
extern int OPT_TYPE_DICT;
extern int OPT_TYPE_OPERATOR;
extern int OPT_TYPE_STRING;
extern int OPT_TYPE_NAME;
extern int OPT_TYPE_MARK;

#endif
