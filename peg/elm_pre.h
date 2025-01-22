/*
 *   Copyright (c) 2022 Nik Silver
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 *
 *   This module contains macros, data decls and prototypes to generate tags for Elm.
 */

/*
 *   INCLUDE FILES
 */

#include "kind.h"
#include "peg_common.h"

/*
 * This allows us to save and restore the module scope.
 * We want to do this because it's unhelpful to say that an imported item
 * is the scope of the importing module.
 */

// Remember the scope of the module tag. But at first there is none (CORK_NIL).
static int elm_module_scope_index;

#define ELM_INIT_MODULE_SCOPE \
	elm_module_scope_index = CORK_NIL
#define ELM_SAVE_MODULE_SCOPE \
	if (elm_module_scope_index != CORK_NIL) { \
		POP_SCOPE (auxil); \
	}
#define ELM_RESTORE_MODULE_SCOPE \
	if (elm_module_scope_index != CORK_NIL) { \
		SET_SCOPE (auxil, elm_module_scope_index); \
	} else { \
		POP_SCOPE (auxil); \
	}

/*
 *   DATA DECLARATIONS
 */
typedef enum {
	K_MODULE,
	K_NAMESPACE,
	K_TYPE,
	K_CONSTRUCTOR,
	K_ALIAS,
	K_PORT,
	K_FUNCTION,
	COUNT_KINDS,
} ElmKind;

/* We only define roles which aren't def(ined)
 */
typedef enum {
	ELM_MODULE_IMPORTED,
} elmModuleRoles;

static roleDefinition ElmModuleRoles [] = {
	{ true, "imported", "module imported" },
};

#define define_elm_role(NAME,Name)							\
	typedef enum {											\
		ELM_##NAME##_EXPOSED,								\
	} elm##Name##Roleds;									\
	static roleDefinition Elm##Name##Roles [] = {			\
		{ true, "exposed", "item exposed from a module" },	\
	}
define_elm_role(TYPE,Type);
define_elm_role(CONSTRUCTOR,Constructor);
define_elm_role(FUNCTION,Function);

typedef enum {
	F_MODULENAME,
	COUNT_FIELDS,
} ElmField;

static fieldDefinition ElmFields [COUNT_FIELDS] = {
	{ .name        = "moduleName",
	  .description = "actual name of renamed module",
	  .enabled     = true },
};

/* Use referenceOnly = true when a tag must always appear
 * as role that's not def(ined).
 */
static kindDefinition ElmKinds [COUNT_KINDS] = {
	{ true, 'm', "module", "modules",
	  .referenceOnly = false, ATTACH_ROLES (ElmModuleRoles) },
	{ true, 'n', "namespace", "modules renamed", },
	{ true, 't', "type", "types",
	  .referenceOnly = false, ATTACH_ROLES (ElmTypeRoles) },
	{ true, 'c', "constructor", "constructors",
	  .referenceOnly = false, ATTACH_ROLES (ElmConstructorRoles) },
	{ true, 'a', "alias", "aliases", },
	{ true, 'p', "port", "ports", },
	{ true, 'f', "function", "functions",
	  .referenceOnly = false, ATTACH_ROLES (ElmFunctionRoles) },
};

struct parserCtx {
	struct parserBaseCtx base;
	vString *customType;
	vString *consSubtype;
};

/*
*   FUNCTION PROTOTYPES
*/
#define USE_KIND_STACK KIND_GHOST_INDEX
static int makeElmTag (struct parserCtx *auxil, const char *name, long offset, int kind, int role);
static int makeElmTagSettingScope (struct parserCtx *auxil, const char *name, long offset, int kind, int role);
static void addElmSignature(int scope_index, const char *sig);
static void addElmTypeRef(int scope_index, const char *str);
static void addElmAccess(int scope_index, const char *access);
static void initElmConstructorFields (struct parserCtx *auxil, const char *name);
static void initElmConstructorSubtypeFields (struct parserCtx *auxil);
static void addElmConstructorSubtype (struct parserCtx *auxil, const char *name);
static void addElmConstructorTypeRef (struct parserCtx *auxil, int tag_index);
static void tidyElmConstructorFields (struct parserCtx *auxil);
static vString *collapseWhitespace (const char *sig);
