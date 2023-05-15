/*
 *
 *  Copyright (c) 2015, Red Hat, Inc.
 *  Copyright (c) 2015, Masatake YAMATO
 *
 *  Author: Masatake YAMATO <yamato@redhat.com>
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 *
 */

#include "general.h"  /* must always come first */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ctags.h"
#include "debug.h"
#include "entry.h"
#include "entry_p.h"
#include "field.h"
#include "field_p.h"
#include "kind.h"
#include "options_p.h"
#include "parse_p.h"
#include "read.h"
#include "routines.h"
#include "trashbox.h"
#include "writer_p.h"
#include "xtag_p.h"

#include "optscript.h"
#include "script_p.h"

typedef struct sFieldObject {
	fieldDefinition *def;
	vString     *buffer;
	const char* nameWithPrefix;
	langType language;
	fieldType sibling;
} fieldObject;

static const char *renderFieldName (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldNameNoEscape (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b);
static const char *renderFieldInput (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldInputNoEscape (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldCompactInputLine (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldSignature (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldSignatureNoEscape (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldScope (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldScopeNoEscape (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldTyperef (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldInherits (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldKindName (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldLineNumber (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldLanguage (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldAccess (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldKindLetter (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldImplementation (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldFile (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldPattern (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldRoles (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldRefMarker (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldExtras (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldXpath (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldScopeKindName(const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldEnd (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldEpoch (const tagEntryInfo *const tag, const char *value, vString* b);
static const char *renderFieldNth (const tagEntryInfo *const tag, const char *value, vString* b);

static bool doesContainAnyCharInName (const tagEntryInfo *const tag, const char *value, const char *chars);
static bool doesContainAnyCharInInput (const tagEntryInfo *const tag, const char*value, const char *chars);
static bool doesContainAnyCharInFieldScope (const tagEntryInfo *const tag, const char *value, const char *chars);
static bool doesContainAnyCharInSignature (const tagEntryInfo *const tag, const char *value, const char *chars);

static bool     isTyperefFieldAvailable   (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isFileFieldAvailable      (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isInheritsFieldAvailable  (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isAccessFieldAvailable    (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isImplementationFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isSignatureFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isExtrasFieldAvailable    (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isXpathFieldAvailable     (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isEndFieldAvailable       (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isEpochAvailable          (const tagEntryInfo *const tag, const fieldDefinition *fdef);
static bool     isNthAvailable            (const tagEntryInfo *const tag, const fieldDefinition *fdef);

static EsObject* getFieldValueForName (const tagEntryInfo *, const fieldDefinition *);
static EsObject* setFieldValueForName (tagEntryInfo *, const fieldDefinition *, const EsObject *);
static EsObject* getFieldValueForInput (const tagEntryInfo *, const fieldDefinition *);
static EsObject* getFieldValueForKind (const tagEntryInfo *, const fieldDefinition *);
static EsObject* getFieldValueForTyperef (const tagEntryInfo *, const fieldDefinition *);
static EsObject* setFieldValueForTyperef (tagEntryInfo *, const fieldDefinition *, const EsObject *);
static EsObject* checkFieldValueForTyperef (const fieldDefinition *, const EsObject *);
static EsObject* getFieldValueForScope (const tagEntryInfo *, const fieldDefinition *);
static EsObject* setFieldValueForScope (tagEntryInfo *, const fieldDefinition *, const EsObject *);
static EsObject* checkFieldValueForScope (const fieldDefinition *, const EsObject *);
static EsObject* getFieldValueForExtras (const tagEntryInfo *, const fieldDefinition *);
static EsObject* getFieldValueForAccess (const tagEntryInfo *, const fieldDefinition *);
static EsObject* setFieldValueForAccess (tagEntryInfo *, const fieldDefinition *, const EsObject *);
static EsObject* getFieldValueForSignature (const tagEntryInfo *, const fieldDefinition *);
static EsObject* setFieldValueForSignature (tagEntryInfo *, const fieldDefinition *, const EsObject *);
static EsObject* getFieldValueForRoles (const tagEntryInfo *, const fieldDefinition *);
static EsObject* getFieldValueForLineCommon (const tagEntryInfo *, const fieldDefinition *);
static EsObject* checkFieldValueForLineCommon (const fieldDefinition *, const EsObject *);
static EsObject* setFieldValueForLineCommon (tagEntryInfo *, const fieldDefinition *, const EsObject *);
static EsObject* setFieldValueForInherits (tagEntryInfo *, const fieldDefinition *, const EsObject *);

#define WITH_DEFAULT_VALUE(str) ((str)?(str):FIELD_NULL_LETTER_STRING)

static fieldDefinition fieldDefinitionsFixed [] = {
	[FIELD_NAME] = {
		.letter             = 'N',
		.name               = "name",
		.description        = "tag name",
		.enabled            = true,
		.render             = renderFieldName,
		.renderNoEscaping   = renderFieldNameNoEscape,
		.doesContainAnyChar = doesContainAnyCharInName,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING,
		.getterValueType    = NULL,
		.getValueObject     = getFieldValueForName,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = setFieldValueForName,
	},
	[FIELD_INPUT_FILE] = {
		.letter             = 'F',
		.name               = "input",
		.description        = "input file",
		.enabled            = true,
		.render             = renderFieldInput,
		.renderNoEscaping   = renderFieldInputNoEscape,
		.doesContainAnyChar = doesContainAnyCharInInput,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING,
		.getterValueType    = NULL,
		.getValueObject     = getFieldValueForInput,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = NULL,
	},
	[FIELD_PATTERN] = {
		.letter             = 'P',
		.name               = "pattern",
		.description        = "pattern",
		.enabled            = true,
		.render             = renderFieldPattern,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING|FIELDTYPE_BOOL,
	},
};

static fieldDefinition fieldDefinitionsExuberant [] = {
	[FIELD_COMPACT_INPUT_LINE - FIELD_ECTAGS_START] = {
		.letter             = 'C',
		.name               = "compact",
		.description        = "compact input line (used only in xref output)",
		.enabled            = false,
		.render             = renderFieldCompactInputLine,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING,
	},
	[FIELD_FILE_SCOPE - FIELD_ECTAGS_START] = {
		.letter             = 'f',
		.name               = "file",
		.description        = "File-restricted scoping",
		.enabled            = true,
		.render             = renderFieldFile,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = isFileFieldAvailable,
		.dataType           = FIELDTYPE_BOOL,
	},
	[FIELD_KIND_LONG - FIELD_ECTAGS_START] = {
		.letter             = 'K',
		.name               = NULL,
		.description        = "Kind of tag in long-name form",
		.enabled            = false,
		.render             = renderFieldKindName,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING,
	},
	[FIELD_KIND - FIELD_ECTAGS_START] = {
		.letter             ='k',
		.name               = NULL,
		.description        = "Kind of tag in one-letter form",
		.enabled            = true,
		.render             = renderFieldKindLetter,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING,
	},
	[FIELD_LANGUAGE - FIELD_ECTAGS_START] = {
		.letter             = 'l',
		.name               = "language",
		.description        = "Language of input file containing tag",
		.enabled            = false,
		.render             = renderFieldLanguage,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.dataType           = FIELDTYPE_STRING,
	},
	[FIELD_LINE_NUMBER - FIELD_ECTAGS_START] = {
		.letter             = 'n',
		.name               = "line",
		.description        = "Line number of tag definition",
		.enabled            = false,
		.render             = renderFieldLineNumber,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_INTEGER,
		.getterValueType    = "int",
		.getValueObject     = getFieldValueForLineCommon,
		.setterValueType    = "matchloc|line:int", /* line <= getInputLineNumber(); */
		.checkValueForSetter= checkFieldValueForLineCommon,
		.setValueObject     = setFieldValueForLineCommon,
	},
	[FIELD_SCOPE - FIELD_ECTAGS_START] = {
		.letter				= 's',
		.name				= NULL,
		.description		= "[tags output] scope (kind:name) of tag definition, [xref and json output] name of scope",
		.enabled			= true,
		.render				= renderFieldScope,
		.renderNoEscaping	= renderFieldScopeNoEscape,
		.doesContainAnyChar = doesContainAnyCharInFieldScope,
		.isValueAvailable	= NULL,
		.dataType			= FIELDTYPE_STRING,
	},
	[FIELD_TYPE_REF - FIELD_ECTAGS_START] = {
		.letter				= 't',
		.name				= "typeref",
		.description		= "Type and name of a variable or typedef",
		.enabled			= true,
		.render				= renderFieldTyperef,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= isTyperefFieldAvailable,
		.dataType			= FIELDTYPE_STRING,
		.getterValueType    = "[string string]",
		.getValueObject     = getFieldValueForTyperef,
		.setterValueType    = "[string string]|string|index:int|false",
		.checkValueForSetter= checkFieldValueForTyperef,
		.setValueObject     = setFieldValueForTyperef,
	},
	[FIELD_KIND_KEY - FIELD_ECTAGS_START] = {
		.letter				= 'z',
		.name				= "kind",
		.description		= "[tags output] prepend \"kind:\" to k/ (or K/) field output, [xref and json output] kind in long-name form",
		.enabled			= false,
		/* Following renderer is for handling --_xformat=%{kind};
		   and is not for tags output. */
		.render				= renderFieldKindName,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= NULL,
		.dataType			= FIELDTYPE_STRING,
		.getterValueType    = "name",
		.getValueObject     = getFieldValueForKind,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = NULL,
	},
	[FIELD_INHERITANCE - FIELD_ECTAGS_START] = {
		.letter             = 'i',
		.name               = "inherits",
		.description        = "Inheritance information",
		.enabled            = false,
		.render             = renderFieldInherits,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = isInheritsFieldAvailable,
		.dataType           = FIELDTYPE_STRING|FIELDTYPE_BOOL,
		.getterValueType    = NULL,
		.getValueObject     = NULL,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = setFieldValueForInherits,
	},
	[FIELD_ACCESS - FIELD_ECTAGS_START] = {
		.letter             = 'a',
		.name               = "access",
		.description        = "Access (or export) of class members",
		.enabled            = false,
		.render             = renderFieldAccess,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = isAccessFieldAvailable,
		.dataType           = FIELDTYPE_STRING,
		.getterValueType    = NULL,
		.getValueObject     = getFieldValueForAccess,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = setFieldValueForAccess,
	},
	[FIELD_IMPLEMENTATION - FIELD_ECTAGS_START] = {
		.letter             = 'm',
		.name               = "implementation",
		.description        = "Implementation information",
		.enabled            = false,
		.render             = renderFieldImplementation,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = isImplementationFieldAvailable,
		.dataType           = FIELDTYPE_STRING,

	},
	[FIELD_SIGNATURE - FIELD_ECTAGS_START] = {
		.letter				= 'S',
		.name				= "signature",
		.description		= "Signature of routine (e.g. prototype or parameter list)",
		.enabled			= false,
		.render				= renderFieldSignature,
		.renderNoEscaping	= renderFieldSignatureNoEscape,
		.doesContainAnyChar = doesContainAnyCharInSignature,
		.isValueAvailable	= isSignatureFieldAvailable,
		.dataType			= FIELDTYPE_STRING,
		.getterValueType    = NULL,
		.getValueObject     = getFieldValueForSignature,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = setFieldValueForSignature,
	},
};

static fieldDefinition fieldDefinitionsUniversal [] = {
	[FIELD_REF_MARK - FIELDS_UCTAGS_START] = {
		.letter             = 'R',
		.name				= NULL,
		.description		= "Marker (R or D) representing whether tag is definition or reference",
		.enabled			= false,
		.render				= renderFieldRefMarker,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= NULL,
		.dataType			= FIELDTYPE_STRING,
	},
	[FIELD_SCOPE_KEY - FIELDS_UCTAGS_START] = {
		.letter             = 'Z',
		.name               = "scope",
		.description        = "[tags output] prepend \"scope:\" key to s/scope field output, [xref and json output] the same as s/ field",
		.enabled            = false,
		/* Following renderer is for handling --_xformat=%{scope};
		   and is not for tags output. */
		.render             = renderFieldScope,
		.renderNoEscaping   = renderFieldScopeNoEscape,
		.doesContainAnyChar = doesContainAnyCharInFieldScope,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING,
		.getterValueType    = "int",
		.getValueObject     = getFieldValueForScope,
		.setterValueType    = "int",
		.checkValueForSetter= checkFieldValueForScope,
		.setValueObject     = setFieldValueForScope,
	},
	[FIELD_SCOPE_KIND_LONG - FIELDS_UCTAGS_START] = {
		.letter				= 'p',
		.name				= "scopeKind",
		.description		= "[tags output] no effect, [xref and json output] kind of scope in long-name form",
		.enabled			= false,
		.render				= renderFieldScopeKindName,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= NULL,
		.dataType			= FIELDTYPE_STRING,
	},
	[FIELD_ROLES - FIELDS_UCTAGS_START] = {
		.letter             = 'r',
		.name               = "roles",
		.description        = "Roles",
		.enabled            = false,
		.render             = renderFieldRoles,
		.renderNoEscaping   = NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable   = NULL,
		.dataType           = FIELDTYPE_STRING,
		.getterValueType    = "[ role1:name ... rolen:name ]",
		.getValueObject     = getFieldValueForRoles,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = NULL,
	},
	[FIELD_EXTRAS - FIELDS_UCTAGS_START] = {
		.letter				= 'E',
		.name				= "extras",
		.description		= "Extra tag type information",
		.enabled			= false,
		.render				= renderFieldExtras,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= isExtrasFieldAvailable,
		.dataType			= FIELDTYPE_STRING,
		.getterValueType    = "[ extra1:name ... extran:name ]",
		.getValueObject     = getFieldValueForExtras,
		.setterValueType    = NULL,
		.checkValueForSetter= NULL,
		.setValueObject     = NULL,
	},
	[FIELD_XPATH - FIELDS_UCTAGS_START] = {
		.letter				= 'x',
		.name				= "xpath",
		.description		= "xpath for the tag",
		.enabled			= false,
		.render				= renderFieldXpath,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= isXpathFieldAvailable,
		.dataType			= FIELDTYPE_STRING,
	},
	[FIELD_END_LINE - FIELDS_UCTAGS_START] = {
		.letter				= 'e',
		.name				= "end",
		.description		= "end lines of various items",
		.enabled			= false,
		.render				= renderFieldEnd,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= isEndFieldAvailable,
		.dataType			= FIELDTYPE_INTEGER,
		.getterValueType    = "int",
		.getValueObject     = getFieldValueForLineCommon,
		.setterValueType    = "matchloc|int",
		.checkValueForSetter= checkFieldValueForLineCommon,
		.setValueObject     = setFieldValueForLineCommon,

	},
	[FIELD_EPOCH - FIELDS_UCTAGS_START] = {
		.letter				= 'T',
		.name				= "epoch",
		.description		= "the last modified time of the input file (only for F/file kind tag)",
		.enabled			= true,
		.render				= renderFieldEpoch,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= isEpochAvailable,
		.dataType			= FIELDTYPE_INTEGER,
	},
	[FIELD_NTH - FIELDS_UCTAGS_START] = {
		.letter				= 'o',
		.name				= "nth",
		.description		= "the order in the parent scope",
		.enabled			= false,
		.render				= renderFieldNth,
		.renderNoEscaping	= NULL,
		.doesContainAnyChar = NULL,
		.isValueAvailable	= isNthAvailable,
		.dataType			= FIELDTYPE_INTEGER,
	},
};


static unsigned int       fieldObjectUsed = 0;
static unsigned int       fieldObjectAllocated = 0;
static fieldObject* fieldObjects = NULL;

extern void initFieldObjects (void)
{
	unsigned int i;
	fieldObject *fobj;

	Assert (fieldObjects == NULL);

	fieldObjectAllocated
	  = ARRAY_SIZE (fieldDefinitionsFixed)
	  + ARRAY_SIZE (fieldDefinitionsExuberant)
	  + ARRAY_SIZE (fieldDefinitionsUniversal);
	fieldObjects = xMalloc (fieldObjectAllocated, fieldObject);
	DEFAULT_TRASH_BOX(&fieldObjects, eFreeIndirect);

	fieldObjectUsed = 0;

	for (i = 0; i < ARRAY_SIZE (fieldDefinitionsFixed); i++)
	{
		fobj = fieldObjects + i + fieldObjectUsed;
		fobj->def = fieldDefinitionsFixed + i;
		fobj->buffer = NULL;
		fobj->nameWithPrefix = fobj->def->name;
		fobj->language = LANG_IGNORE;
		fobj->sibling  = FIELD_UNKNOWN;
		fobj->def->ftype = i + fieldObjectUsed;
	}
	fieldObjectUsed += ARRAY_SIZE (fieldDefinitionsFixed);

	for (i = 0; i < ARRAY_SIZE (fieldDefinitionsExuberant); i++)
	{
		fobj = fieldObjects + i + fieldObjectUsed;
		fobj->def = fieldDefinitionsExuberant +i;
		fobj->buffer = NULL;
		fobj->nameWithPrefix = fobj->def->name;
		fobj->language = LANG_IGNORE;
		fobj->sibling  = FIELD_UNKNOWN;
		fobj->def->ftype = i + fieldObjectUsed;
	}
	fieldObjectUsed += ARRAY_SIZE (fieldDefinitionsExuberant);

	for (i = 0; i < ARRAY_SIZE (fieldDefinitionsUniversal); i++)
	{
		char *nameWithPrefix;

		fobj = fieldObjects + i + fieldObjectUsed;
		fobj->def = fieldDefinitionsUniversal + i;
		fobj->buffer = NULL;

		if (fobj->def->name)
		{
			nameWithPrefix = eMalloc (sizeof CTAGS_FIELD_PREFIX + strlen (fobj->def->name) + 1);
			nameWithPrefix [0] = '\0';
			strcat (nameWithPrefix, CTAGS_FIELD_PREFIX);
			strcat (nameWithPrefix, fobj->def->name);
			fobj->nameWithPrefix = nameWithPrefix;
			DEFAULT_TRASH_BOX(nameWithPrefix, eFree);
		}
		else
			fobj->nameWithPrefix = NULL;
		fobj->language = LANG_IGNORE;
		fobj->sibling  = FIELD_UNKNOWN;
		fobj->def->ftype = i + fieldObjectUsed;
	}
	fieldObjectUsed += ARRAY_SIZE (fieldDefinitionsUniversal);

	Assert ( fieldObjectAllocated == fieldObjectUsed );
}

static fieldObject* getFieldObject(fieldType type)
{
	Assert ((0 <= type) && ((unsigned int)type < fieldObjectUsed));
	return fieldObjects + type;
}

extern fieldType getFieldTypeForLetter (char letter)
{
	unsigned int i;

	for (i = 0; i < fieldObjectUsed; i++)
	{
		if (fieldObjects [i].def->letter == letter)
			return i;
	}
	return FIELD_UNKNOWN;
}

extern fieldType getFieldTypeForName (const char *name)
{
	return getFieldTypeForNameAndLanguage (name, LANG_IGNORE);
}

extern fieldType getFieldTypeForNameAndLanguage (const char *fieldName, langType language)
{
	static bool initialized = false;
	unsigned int i;

	if (fieldName == NULL)
		return FIELD_UNKNOWN;

	if (language == LANG_AUTO && (initialized == false))
	{
		initialized = true;
		initializeParser (LANG_AUTO);
	}
	else if (language != LANG_IGNORE && (initialized == false))
		initializeParser (language);

	for (i = 0; i < fieldObjectUsed; i++)
	{
		if (fieldObjects [i].def->name
		    && strcmp (fieldObjects [i].def->name, fieldName) == 0
		    && ((language == LANG_AUTO)
			|| (fieldObjects [i].language == language)))
			return i;
	}

	return FIELD_UNKNOWN;
}

extern const char* getFieldDescription (fieldType type)
{
	fieldObject* fobj;

	fobj = getFieldObject (type);
	return fobj->def->description;
}

extern const char* getFieldName(fieldType type)
{
	fieldObject* fobj;

	fobj = getFieldObject (type);
	if (Option.putFieldPrefix)
		return fobj->nameWithPrefix;
	else
		return fobj->def->name;
}

extern unsigned char getFieldLetter (fieldType type)
{
	fieldObject* fobj = getFieldObject (type);

	return fobj->def->letter == '\0'
		? FIELD_NULL_LETTER_CHAR
		: fobj->def->letter;
}

extern bool doesFieldHaveValue (fieldType type, const tagEntryInfo *tag)
{
	if (getFieldObject(type)->def->isValueAvailable)
		return getFieldObject(type)->def->isValueAvailable(tag,
														   getFieldObject(type)->def);
	else
		return true;
}

static const char *renderAsIs (vString* b CTAGS_ATTR_UNUSED, const char *s)
{
	return s;
}

static const char *renderEscapedString (const char *s,
					const tagEntryInfo *const tag CTAGS_ATTR_UNUSED,
					vString* b)
{
	vStringCatSWithEscaping (b, s);
	return vStringValue (b);
}

static const char *renderEscapedName (const bool isTagName,
				      const char* s,
				      const tagEntryInfo *const tag,
				      vString* b)
{
	int unexpected_byte = 0;

	if (isTagName && (!tag->isPseudoTag) &&  (*s == ' ' || *s == '!'))
	{
		/* Don't allow a leading space or exclamation mark as it conflicts with
		 * pseudo-tags when sorting.  Anything with a lower byte value is
		 * escaped by renderEscapedString() already. */
		unexpected_byte = *s;
		switch (*s)
		{
			case ' ': vStringCatS (b, "\\x20"); s++; break;
			case '!': vStringCatS (b, "\\x21"); s++; break;
			default: AssertNotReached();
		}
	}
	else
	{
		/* Find the first byte needing escaping for the warning message */
		const char *p = s;

		while (*p > 0x1F && *p != 0x7F)
			p++;
		unexpected_byte = *p;
	}

	if (unexpected_byte)
	{
		const kindDefinition *kdef = getTagKind (tag);
		verbose ("Unexpected character %#04x included in a tagEntryInfo: %s\n", unexpected_byte, s);
		verbose ("File: %s, Line: %lu, Lang: %s, Kind: %c\n",
			 tag->inputFileName, tag->lineNumber, getLanguageName(tag->langType), kdef->letter);
		verbose ("Escape the character\n");
	}

	return renderEscapedString (s, tag, b);
}

static const char *renderFieldName (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	return renderEscapedName (true, tag->name, tag, b);
}

static const char *renderFieldNameNoEscape (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	return renderAsIs (b, tag->name);
}

static bool doesContainAnyCharInName (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, const char *chars)
{
	return strpbrk (tag->name, chars)? true: false;
}

static const char *renderFieldInput (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	const char *f = tag->inputFileName;

	if (Option.lineDirectives && tag->sourceFileName)
		f = tag->sourceFileName;
	return renderEscapedString (f, tag, b);
}

static const char *renderFieldInputNoEscape (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	const char *f = tag->inputFileName;

	if (Option.lineDirectives && tag->sourceFileName)
		f = tag->sourceFileName;

	return renderAsIs (b, f);
}

static bool doesContainAnyCharInInput (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, const char *chars)
{
	const char *f = tag->inputFileName;

	if (Option.lineDirectives && tag->sourceFileName)
		f = tag->sourceFileName;

	return strpbrk (f, chars)? true: false;
}

static const char *renderFieldSignature (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	return renderEscapedString (WITH_DEFAULT_VALUE (tag->extensionFields.signature),
				    tag, b);
}

static const char *renderFieldSignatureNoEscape (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	return renderAsIs (b, WITH_DEFAULT_VALUE (tag->extensionFields.signature));
}

static bool doesContainAnyCharInSignature (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, const char *chars)
{
	return (tag->extensionFields.signature && strpbrk(tag->extensionFields.signature, chars))
		? true
		: false;
}

static const char *renderFieldScope (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	const char* scope;

	getTagScopeInformation ((tagEntryInfo *const)tag, NULL, &scope);
	return scope? renderEscapedName (false, scope, tag, b): NULL;
}

static const char *renderFieldScopeNoEscape (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	const char* scope;

	getTagScopeInformation ((tagEntryInfo *const)tag, NULL, &scope);
	return scope? renderAsIs (b, scope): NULL;
}

static bool doesContainAnyCharInFieldScope (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, const char *chars)
{
	const char* scope;

	getTagScopeInformation ((tagEntryInfo *const)tag, NULL, &scope);
	return (scope && strpbrk (scope, chars));
}


static const char *renderFieldInherits (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	return renderEscapedString (WITH_DEFAULT_VALUE (tag->extensionFields.inheritance),
				    tag, b);
}

static const char *renderFieldTyperef (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	/* Return "-" instead of "-:-". */
	if (tag->extensionFields.typeRef [0] == NULL
		&& tag->extensionFields.typeRef [1] == NULL)
		return renderAsIs (b, FIELD_NULL_LETTER_STRING);

	vStringCatS (b, WITH_DEFAULT_VALUE (tag->extensionFields.typeRef [0]));
	vStringPut  (b, ':');
	return renderEscapedName (false, WITH_DEFAULT_VALUE (tag->extensionFields.typeRef [1]), tag, b);
}


static const char* renderFieldCommon (fieldType type,
									  const tagEntryInfo *tag,
									  int index,
									  bool noEscaping)
{
	fieldObject *fobj = fieldObjects + type;
	const char *value;
	fieldRenderer rfn;

	Assert (tag);
	Assert (index < 0 || ((unsigned int)index) < tag->usedParserFields);

	if (index >= 0)
	{
		const tagField *f = getParserFieldForIndex (tag, index);

		value = f->value;
	}
	else
		value = NULL;

	if (noEscaping)
		rfn = fobj->def->renderNoEscaping;
	else
		rfn = fobj->def->render;
	Assert (rfn);

	fobj->buffer = vStringNewOrClearWithAutoRelease (fobj->buffer);
	return rfn (tag, value, fobj->buffer);
}

extern const char* renderField (fieldType type, const tagEntryInfo *tag, int index)
{
	return renderFieldCommon (type, tag, index, false);
}

extern const char* renderFieldNoEscaping (fieldType type, const tagEntryInfo *tag, int index)
{
	return renderFieldCommon (type, tag, index, true);
}

static bool defaultDoesContainAnyChar (const tagEntryInfo *const tag CTAGS_ATTR_UNUSED, const char* value, const char* chars)
{
	return strpbrk (value, chars)? true: false;
}

extern bool  doesFieldHaveTabOrNewlineChar (fieldType type, const tagEntryInfo *tag, int index)
{
	fieldObject *fobj = fieldObjects + type;
	const char *value;
	bool (* doesContainAnyChar) (const tagEntryInfo *const, const char*, const char*) = fobj->def->doesContainAnyChar;

	Assert (tag);
	Assert (index == NO_PARSER_FIELD || ((unsigned int)index) < tag->usedParserFields);

	if (doesContainAnyChar == NULL)
	{
		if (index == NO_PARSER_FIELD)
			return false;
		else
			doesContainAnyChar = defaultDoesContainAnyChar;
	}

	if (index >= 0)
	{
		const tagField *f = getParserFieldForIndex (tag, index);

		value = f->value;
	}
	else
		value = NULL;

	return (* doesContainAnyChar) (tag, value, "\t\n");
}

/*  Writes "line", stripping leading and duplicate white space.
 */
static const char* renderCompactInputLine (vString *b,  const char *const line)
{
	bool lineStarted = false;

	/*  Write everything up to, but not including, the newline.
	 */
	for (const char *p = line; *p != '\n' && *p != '\0'; ++p)
	{
		int c = (unsigned char) *p;
		if (lineStarted  || ! isspace (c))  /* ignore leading spaces */
		{
			lineStarted = true;
			if (isspace (c))
			{
				int next;

				/*  Consume repeating white space.
				 */
				while (next = (unsigned char) *(p+1), isspace (next) && next != '\n')
					++p;
				c = ' ';  /* force space character for any white space */
			}
			if (c != '\r'  ||  *(p + 1) != '\n')
				vStringPut (b, c);
		}
	}
	return vStringValue (b);
}

static const char *renderFieldKindName (const tagEntryInfo *const tag, const char *value CTAGS_ATTR_UNUSED, vString* b)
{
	const char* name = getTagKindName (tag);
	return renderAsIs (b, name);
}

static const char *renderFieldCompactInputLine (const tagEntryInfo *const tag,
						const char *value CTAGS_ATTR_UNUSED,
						 vString* b)
{
	const char *line;
	static vString *tmp;

	if (tag->isPseudoTag)
	{
		Assert (tag->pattern);
		return tag->pattern;
	}

	tmp = vStringNewOrClearWithAutoRelease (tmp);

	line = readLineFromBypassForTag (tmp, tag, NULL);
	if (line)
		renderCompactInputLine (b, line);
	else
	{
		/* If no associated line for tag is found, we cannot prepare
		 * parameter to writeCompactInputLine(). In this case we
		 * use an empty string as LINE.
		 */
		vStringClear (b);
	}

	return vStringValue (b);
}

static const char *renderFieldLineNumber (const tagEntryInfo *const tag,
					  const char *value CTAGS_ATTR_UNUSED,
					  vString* b)
{
	long ln = tag->lineNumber;
	char buf[32] = {[0] = '\0'};

	if (Option.lineDirectives && (tag->sourceLineNumberDifference != 0))
		ln += tag->sourceLineNumberDifference;
	snprintf (buf, sizeof(buf), "%ld", ln);
	vStringCatS (b, buf);
	return vStringValue (b);
}

struct renderRoleData {
	vString* b;
	int nRoleWritten;
};

static void renderRoleByIndex (const tagEntryInfo *const tag, int roleIndex, void *data)
{
	struct renderRoleData *rdata = data;

	if (!isLanguageRoleEnabled (tag->langType, tag->kindIndex, roleIndex))
		return;

	if (rdata->nRoleWritten > 0)
		vStringPut(rdata->b, ',');

	const roleDefinition * role = getTagRole(tag, roleIndex);
	renderRole (role, rdata->b);
	rdata->nRoleWritten++;
}

static roleBitsType foreachRoleBits (const tagEntryInfo *const tag,
									 void (* fn) (const tagEntryInfo *const, int, void *),
									 void *data)
{
	roleBitsType rbits = tag->extensionFields.roleBits;
	if (!rbits)
		return rbits;

	int roleCount = countLanguageRoles (tag->langType, tag->kindIndex);

	for (int roleIndex = 0; roleIndex < roleCount; roleIndex++)
	{
		if ((rbits >> roleIndex) & (roleBitsType)1)
			fn (tag, roleIndex, data);
	}
	return rbits;
}

static const char *renderFieldRoles (const tagEntryInfo *const tag,
				    const char *value CTAGS_ATTR_UNUSED,
				    vString* b)
{
	struct renderRoleData data = { .b = b, .nRoleWritten = 0 };

	if (!foreachRoleBits (tag, renderRoleByIndex, &data))
		vStringCatS (b, ROLE_DEFINITION_NAME);
	return vStringValue (b);
}

static const char *renderFieldLanguage (const tagEntryInfo *const tag,
					const char *value CTAGS_ATTR_UNUSED,
					vString* b)
{
	const char *l;

	if (Option.lineDirectives && (tag->sourceLangType != LANG_IGNORE))
		l = getLanguageName(tag->sourceLangType);
	else
	{
		Assert (tag->langType != LANG_IGNORE);
		l = getLanguageName(tag->langType);
	}

	return renderAsIs (b, WITH_DEFAULT_VALUE(l));
}

static const char *renderFieldAccess (const tagEntryInfo *const tag,
				      const char *value CTAGS_ATTR_UNUSED,
				      vString* b)
{
	return renderAsIs (b, WITH_DEFAULT_VALUE (tag->extensionFields.access));
}

static const char *renderFieldKindLetter (const tagEntryInfo *const tag,
					  const char *value CTAGS_ATTR_UNUSED,
					  vString* b)
{
	static char c[2] = { [1] = '\0' };

	c [0] = getTagKindLetter(tag);

	return renderAsIs (b, c);
}

static const char *renderFieldImplementation (const tagEntryInfo *const tag,
					      const char *value CTAGS_ATTR_UNUSED,
					      vString* b)
{
	return renderAsIs (b, WITH_DEFAULT_VALUE (tag->extensionFields.implementation));
}

static const char *renderFieldFile (const tagEntryInfo *const tag,
				    const char *value CTAGS_ATTR_UNUSED,
				    vString* b)
{
	return renderAsIs (b, tag->isFileScope? "file": FIELD_NULL_LETTER_STRING);
}

static const char *renderFieldPattern (const tagEntryInfo *const tag,
				       const char *value CTAGS_ATTR_UNUSED,
				       vString* b)
{
	if (tag->isFileEntry)
		return NULL;
	else if (tag->pattern)
		vStringCatS (b, tag->pattern);
	else
	{
		char* tmp;

		tmp = makePatternString (tag);
		vStringCatS (b, tmp);
		eFree (tmp);
	}
	return vStringValue (b);
}

static const char *renderFieldRefMarker (const tagEntryInfo *const tag,
					 const char *value CTAGS_ATTR_UNUSED,
					 vString* b)
{
	static char c[2] = { [1] = '\0' };

	c [0] = (tag->extensionFields.roleBits)? 'R': 'D';

	return renderAsIs (b, c);
}

static const char *renderFieldExtras (const tagEntryInfo *const tag,
				     const char *value CTAGS_ATTR_UNUSED,
				     vString* b)
{
	unsigned int i;
	bool hasExtra = false;
	unsigned int c = countXtags();

	for (i = 0; i < c; i++)
	{
		const char *name = getXtagName (i);

		if (!name)
			continue;

		if (isTagExtraBitMarked (tag, i))
		{

			if (hasExtra)
				vStringPut (b, ',');
			vStringCatS (b, name);
			hasExtra = true;
		}
	}

	if (hasExtra)
		return vStringValue (b);
	else
		return NULL;
}

static const char *renderFieldXpath (const tagEntryInfo *const tag,
				     const char *value CTAGS_ATTR_UNUSED,
				     vString* b)
{
#ifdef HAVE_LIBXML
	if (tag->extensionFields.xpath)
		return renderEscapedString (tag->extensionFields.xpath,
					    tag, b);
#endif
	return NULL;
}

static const char *renderFieldScopeKindName(const tagEntryInfo *const tag,
					    const char *value CTAGS_ATTR_UNUSED,
					    vString* b)
{
	const char* kind;

	getTagScopeInformation ((tagEntryInfo *const)tag, &kind, NULL);
	return kind? renderAsIs (b, kind): NULL;
}

static const char *renderFieldEnd (const tagEntryInfo *const tag,
				   const char *value CTAGS_ATTR_UNUSED,
				   vString* b)
{
	static char buf[21];

	if (tag->extensionFields._endLine != 0)
	{
		sprintf (buf, "%lu", tag->extensionFields._endLine);
		return renderAsIs (b, buf);
	}
	else
		return NULL;
}

static const char *renderFieldEpoch (const tagEntryInfo *const tag,
									  const char *value, vString* b)
{
#define buf_len 21
	static char buf[buf_len];
#ifdef _MSC_VER
#define FMTLL "%I64d"
#else
#define FMTLL "%lld"
#endif
	if (snprintf (buf, buf_len, FMTLL, (long long)tag->extensionFields.epoch) > 0)
		return renderAsIs (b, buf);
	else
		return NULL;
#undef FMTLL
#undef buf_len
}

static const char *renderFieldNth (const tagEntryInfo *const tag,
								   const char *value, vString* b)
{
#define buf_len 12
	static char buf[buf_len];

	if (tag->extensionFields.nth > NO_NTH_FIELD
		&& snprintf (buf, buf_len, "%d", (int)tag->extensionFields.nth) > 0)
		return renderAsIs (b, buf);
	else
		return NULL;
#undef buf_len
}

static bool isTyperefFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return (tag->extensionFields.typeRef [0] != NULL
		&& tag->extensionFields.typeRef [1] != NULL)? true: false;
}

static bool isFileFieldAvailable  (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return tag->isFileScope? true: false;
}

static bool isInheritsFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return (tag->extensionFields.inheritance != NULL)? true: false;
}

static bool isAccessFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return (tag->extensionFields.access != NULL)? true: false;
}

static bool isImplementationFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return (tag->extensionFields.implementation != NULL)? true: false;
}

static bool isSignatureFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return (tag->extensionFields.signature != NULL)? true: false;
}

static bool isExtrasFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return isTagExtra (tag);
}

static bool isXpathFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
#ifdef HAVE_LIBXML
	return (tag->extensionFields.xpath != NULL)? true: false;
#else
	return false;
#endif
}

static bool isEndFieldAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return (tag->extensionFields._endLine != 0)? true: false;
}

static bool isEpochAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	return (tag->kindIndex == KIND_FILE_INDEX)
		? true
		: false;
}

static bool isNthAvailable (const tagEntryInfo *const tag, const fieldDefinition *fdef CTAGS_ATTR_UNUSED)
{
	Assert (tag->langType >= NO_NTH_FIELD);
	return (tag->extensionFields.nth != NO_NTH_FIELD)? true: false;
}

extern bool isFieldEnabled (fieldType type)
{
	return getFieldObject(type)->def->enabled;
}

extern bool enableField (fieldType type, bool state)
{
	fieldDefinition *def = getFieldObject(type)->def;
	bool old = def->enabled;
	getFieldObject(type)->def->enabled = state;

	if (isCommonField (type))
		verbose ("enable field \"%s\": %s\n",
				 getFieldObject(type)->def->name,
				 (state? "yes": "no"));
	else
		verbose ("enable field \"%s\"<%s>: %s\n",
				 getFieldObject(type)->def->name,
				 getLanguageName (getFieldLanguage(type)),
				 (state? "yes": "no"));
	return old;
}

extern bool isCommonField (fieldType type)
{
	return (FIELD_BUILTIN_LAST < type)? false: true;
}

extern langType getFieldLanguage (fieldType type)
{
	return getFieldObject(type)->language;
}

extern unsigned int getFieldDataType (fieldType type)
{
	return getFieldObject(type)->def->dataType;
}

extern bool isFieldValueAvailableAlways (fieldType type)
{
	return getFieldObject(type)->def->isValueAvailable == NULL;
}

extern bool doesFieldHaveRenderer (fieldType type, bool noEscaping)
{
	if (noEscaping)
		return getFieldObject(type)->def->renderNoEscaping? true: false;
	else
		return getFieldObject(type)->def->render? true: false;
}

extern unsigned int countFields (void)
{
	return fieldObjectUsed;
}

extern fieldType nextSiblingField (fieldType type)
{
	fieldObject *fobj;

	fobj = fieldObjects + type;
	return fobj->sibling;
}

static void updateSiblingField (fieldType type, const char* name)
{
	int i;
	fieldObject *fobj;

	for (i = type; i > 0; i--)
	{
		fobj = fieldObjects + i - 1;
		if (fobj->def->name && (strcmp (fobj->def->name, name) == 0))
		{
			Assert (fobj->sibling == FIELD_UNKNOWN);
			fobj->sibling = type;
			break;
		}
	}
}

static const char* defaultRenderer (const tagEntryInfo *const tag CTAGS_ATTR_UNUSED,
				    const char *value,
				    vString * buffer CTAGS_ATTR_UNUSED)
{
	return renderEscapedString (value, tag, buffer);
}

static bool isValueAvailableGeneric (const tagEntryInfo *const e, const fieldDefinition *fdef)
{
	return getParserFieldValueForType(e, fdef->ftype)? true: false;
}

extern int defineField (fieldDefinition *def, langType language)
{
	fieldObject *fobj;
	char *nameWithPrefix;
	size_t i;

	Assert (def);
	Assert (def->name);
	for (i = 0; i < strlen (def->name); i++)
	{
		Assert ( isalpha ((unsigned char) def->name [i]) );
	}
	def->letter = NUL_FIELD_LETTER;

	if (fieldObjectUsed == fieldObjectAllocated)
	{
		fieldObjectAllocated *= 2;
		fieldObjects = xRealloc (fieldObjects, fieldObjectAllocated, fieldObject);
	}
	fobj = fieldObjects + (fieldObjectUsed);
	def->ftype = fieldObjectUsed++;

	if (def->render == NULL)
	{
		def->render = defaultRenderer;
		def->renderNoEscaping = NULL;
		def->doesContainAnyChar = NULL;
	}

	if (! def->dataType)
		def->dataType = FIELDTYPE_STRING;

	if (def->isValueAvailable == NULL)
		def->isValueAvailable = isValueAvailableGeneric;

	fobj->def = def;

	fobj->buffer = NULL;

	nameWithPrefix = eMalloc (sizeof CTAGS_FIELD_PREFIX + strlen (def->name) + 1);
	nameWithPrefix [0] = '\0';
	strcat (nameWithPrefix, CTAGS_FIELD_PREFIX);
	strcat (nameWithPrefix, def->name);
	fobj->nameWithPrefix = nameWithPrefix;
	DEFAULT_TRASH_BOX(nameWithPrefix, eFree);

	fobj->language = language;
	fobj->sibling  = FIELD_UNKNOWN;

	updateSiblingField (def->ftype, def->name);
	installOptscriptFieldAccessor (def->ftype);

	return def->ftype;
}

#define FIELD_COL_LETTER      0
#define FIELD_COL_NAME        1
#define FIELD_COL_ENABLED     2
#define FIELD_COL_LANGUAGE    3
#define FIELD_COL_JSTYPE      4
#define FIELD_COL_FIXED       5
#define FIELD_COL_OPERATOR    6
#define FIELD_COL_DESCRIPTION 7

extern struct colprintTable * fieldColprintTableNew (void)
{
	return colprintTableNew ("L:LETTER", "L:NAME", "L:ENABLED",
							 "L:LANGUAGE", "L:JSTYPE", "L:FIXED",
							 "L:OP", "L:DESCRIPTION", NULL);
}

static void  fieldColprintAddLine (struct colprintTable *table, int i)
{
	fieldObject *fobj = getFieldObject(i);
	fieldDefinition *fdef = fobj->def;

	struct colprintLine *line = colprintTableGetNewLine(table);

	colprintLineAppendColumnChar (line,
								  (fdef->letter == NUL_FIELD_LETTER)
								  ? FIELD_NULL_LETTER_CHAR
								  : fdef->letter);

	const char *name = getFieldName (i);
	colprintLineAppendColumnCString (line, name? name: RSV_NONE);
	colprintLineAppendColumnBool (line, fdef->enabled);
	colprintLineAppendColumnCString (line,
									 fobj->language == LANG_IGNORE
									 ? RSV_NONE
									 : getLanguageName (fobj->language));

	char  typefields [] = "---";
	{
		unsigned int bmask, offset;
		unsigned int type = getFieldDataType(i);
		for (bmask = 1, offset = 0;
			 bmask < FIELDTYPE_END_MARKER;
			 bmask <<= 1, offset++)
			if (type & bmask)
				typefields[offset] = fieldDataTypeFlags[offset];
	}
	colprintLineAppendColumnCString (line, typefields);
	colprintLineAppendColumnBool (line, writerDoesTreatFieldAsFixed (i));

	char operator[] = {'-', '-', '\0'};
	if (fdef->getValueObject)
		operator[0] = 'r';
	if (fdef->setValueObject)
		operator[1] = 'w';
	colprintLineAppendColumnCString (line, operator);
	colprintLineAppendColumnCString (line, fdef->description);
}

extern void fieldColprintAddCommonLines (struct colprintTable *table)
{
	for (int i = 0; i <= FIELD_BUILTIN_LAST; i++)
		fieldColprintAddLine(table, i);
}

extern void fieldColprintAddLanguageLines (struct colprintTable *table, langType language)
{
	for (unsigned int i = FIELD_BUILTIN_LAST + 1; i < fieldObjectUsed; i++)
	{
		fieldObject *fobj = getFieldObject(i);
		if (fobj->language == language)
			fieldColprintAddLine (table, i);
	}
}

static int fieldColprintCompareLines (struct colprintLine *a , struct colprintLine *b)
{
	const char *a_fixed  = colprintLineGetColumn (a, FIELD_COL_FIXED);
	const char *b_fixed  = colprintLineGetColumn (b, FIELD_COL_FIXED);
	const char *a_parser = colprintLineGetColumn (a, FIELD_COL_LANGUAGE);
	const char *b_parser = colprintLineGetColumn (b, FIELD_COL_LANGUAGE);

	if ((strcmp (a_fixed, "yes") == 0)
		&& (strcmp (b_fixed, "yes") == 0))
	{
		/* name, input, pattern, compact */
		const char *a_name  = colprintLineGetColumn (a, FIELD_COL_NAME);
		const char *b_name  = colprintLineGetColumn (b, FIELD_COL_NAME);
		const char *ref_name;
		unsigned int a_index = ~0U;
		unsigned int b_index = ~0U;

		for (unsigned int i = 0; i < ARRAY_SIZE(fieldDefinitionsFixed); i++)
		{
			ref_name = fieldDefinitionsFixed [i].name;
			if (strcmp (a_name, ref_name) == 0)
				a_index = i;
			if (strcmp (b_name, ref_name) == 0)
				b_index = i;
			if ((a_index != ~0U) || (b_index != ~0U))
				break;
		}

		if (a_index < b_index)
			return -1;
		else if (a_index == b_index)
			return 0;			/* ??? */
		else
			return 1;
	}
	else if ((strcmp (a_fixed, "yes") == 0)
			  && (strcmp (b_fixed, "yes") != 0))
		return -1;
	else if ((strcmp (a_fixed, "yes") != 0)
			 && (strcmp (b_fixed, "yes") == 0))
		return 1;

	if (strcmp (a_parser, RSV_NONE) == 0
		&& strcmp (b_parser, RSV_NONE) != 0)
		return -1;
	else if (strcmp (a_parser, RSV_NONE) != 0
			 && strcmp (b_parser, RSV_NONE) == 0)
		return 1;
	else if (strcmp (a_parser, RSV_NONE) != 0
			 && strcmp (b_parser, RSV_NONE) != 0)
	{
		int r;
		r = strcmp (a_parser, b_parser);
		if (r != 0)
			return r;

		const char *a_name = colprintLineGetColumn (a, FIELD_COL_NAME);
		const char *b_name = colprintLineGetColumn (b, FIELD_COL_NAME);

		return strcmp(a_name, b_name);
	}
	else
	{
		const char *a_letter = colprintLineGetColumn (a, FIELD_COL_LETTER);
		const char *b_letter = colprintLineGetColumn (b, FIELD_COL_LETTER);

		return strcmp(a_letter, b_letter);
	}
}

extern void fieldColprintTablePrint (struct colprintTable *table,
									 bool withListHeader, bool machinable, FILE *fp)
{
	colprintTableSort (table, fieldColprintCompareLines);
	colprintTablePrint (table, 0, withListHeader, machinable, fp);
}

extern const char * getFieldGetterValueType (fieldType type)
{
	fieldObject *fobj = getFieldObject (type);
	return (fobj? fobj->def->getterValueType: NULL);
}

extern EsObject * getFieldValue (fieldType type, const tagEntryInfo *tag)
{
	fieldObject* fobj;

	fobj = getFieldObject (type);
	if (fobj && fobj->def->getValueObject)
		return fobj->def->getValueObject (tag, fobj->def);
	return es_nil;
}

extern bool hasFieldGetter (fieldType type)
{
	fieldObject *fobj = getFieldObject (type);
	return (fobj && fobj->def->getValueObject);
}

extern const char * getFieldSetterValueType (fieldType type)
{
	fieldObject *fobj = getFieldObject (type);
	return (fobj? fobj->def->setterValueType: NULL);
}

extern EsObject * setFieldValue (fieldType type, tagEntryInfo *tag, const EsObject *val)
{
	fieldObject *fobj;

	fobj = getFieldObject (type);
	if (fobj && fobj->def->setValueObject)
		return fobj->def->setValueObject (tag, fobj->def, val);
	return es_false;
}

extern bool hasFieldSetter (fieldType type)
{
	fieldObject *fobj = getFieldObject (type);
	return (fobj && fobj->def->setValueObject);
}


extern bool hasFieldValueCheckerForSetter (fieldType type)
{
	fieldObject *fobj = getFieldObject (type);
	return (fobj && fobj->def->checkValueForSetter);
}

extern EsObject* checkFieldValueForSetter (fieldType type, const EsObject *val)
{
	fieldObject *fobj = getFieldObject (type);
	return fobj->def->checkValueForSetter (fobj->def, val);
}

static EsObject* getFieldValueForName (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	return opt_string_new_from_cstr (tag->name);
}

static EsObject* setFieldValueForName (tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *val)
{
	eFree ((char*) tag->name);
	const char *cstr = opt_string_get_cstr (val);
	tag->name = eStrdup (cstr);
	return es_false;
}

static EsObject* getFieldValueForInput (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	return opt_string_new_from_cstr (tag->inputFileName);
}

static EsObject* getFieldValueForKind (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	const char *kind_name = getLanguageKindName (tag->langType, tag->kindIndex);
	return opt_name_new_from_cstr (kind_name);
}

static EsObject* getFieldValueForTyperef (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	if (tag->extensionFields.typeRef [0] == NULL
		&& tag->extensionFields.typeRef [1] == NULL)
		return es_nil;

	EsObject *a = opt_array_new ();
	EsObject *e0 = tag->extensionFields.typeRef [0]
		? opt_string_new_from_cstr(tag->extensionFields.typeRef [0])
		: es_false;
	EsObject *e1 = tag->extensionFields.typeRef [1]
		? opt_string_new_from_cstr(tag->extensionFields.typeRef [1])
		: es_false;
	opt_array_put (a, 0, e0);
	opt_array_put (a, 1, e1);
	es_object_unref (e0);
	es_object_unref (e1);
	return a;
}

static EsObject* setFieldValueForTyperef (tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *obj)
{
	const char *tmp[2] = {NULL, NULL};

	for (int i = 0 ; i < 2; i++)
	{
		if (tag->extensionFields.typeRef [i])
			tmp [i] = tag->extensionFields.typeRef [i];
	}

	if (es_boolean_p (obj))
	{
		for (int i = 0 ; i < 2; i++)
		{
			if (tag->extensionFields.typeRef [i])
				tag->extensionFields.typeRef [i] = NULL;
		}
	}
	else if (es_object_get_type (obj) == OPT_TYPE_ARRAY)
	{
		for (int i = 0 ; i < 2; i++)
		{
			EsObject *e = opt_array_get (obj, i);
			if (es_boolean_p (e))
			{
				if (tag->extensionFields.typeRef [i])
					tag->extensionFields.typeRef [i] = NULL;
			}
			else if (es_object_get_type (e) == OPT_TYPE_STRING)
			{
				tag->extensionFields.typeRef [i] = eStrdup (opt_string_get_cstr (e));
			}
		}
	}
	else if (es_object_get_type (obj) == OPT_TYPE_STRING)
	{
		const char *str = opt_string_get_cstr (obj);
		tag->extensionFields.typeRef [0] = eStrdup ("typename");
		tag->extensionFields.typeRef [1] = eStrdup (str);
	}
	else if (es_integer_p (obj))
	{
		int index = es_integer_get (obj);
		tagEntryInfo *e = getEntryInCorkQueue (index);
		if (e)
		{
			const char *name = e->name;
			const char *kindName = getLanguageKindName (e->langType, e->kindIndex);

			tag->extensionFields.typeRef [0] = eStrdup (kindName);
			tag->extensionFields.typeRef [1] = eStrdup (name);
		}
	}
	else
	{
		AssertNotReached();
		return OPT_ERR_TYPECHECK;
	}

	for (int i = 0; i < 2; i++)
		if (tmp [i])
			eFree ((char*)tmp[i]);

	return es_false;
}

static EsObject* checkFieldValueForTyperef (const fieldDefinition *fdef, const EsObject *obj)
{
	if (es_boolean_p (obj))
	{
		if (!es_object_equal (es_false, obj))
			return OPT_ERR_TYPECHECK;
	}
	else if (es_object_get_type (obj) == OPT_TYPE_ARRAY)
	{
		if (opt_array_length (obj) != 2)
			return OPT_ERR_TYPECHECK;

		for (unsigned int i = 0; i < 2; i++)
		{
			EsObject *e = opt_array_get (obj, i);
			if (es_object_get_type (e) != OPT_TYPE_STRING)
				return OPT_ERR_TYPECHECK;
		}
	}
	else if (es_object_get_type (obj) == OPT_TYPE_STRING)
		;
	else if (es_integer_p (obj))
	{
		int index0 = es_integer_get (obj);
		if (index0 < 0)
			return OPT_ERR_RANGECHECK;

		unsigned int index = index0;
		if (index == 0 || index >= countEntryInCorkQueue ())
			return OPTSCRIPT_ERR_NOTAGENTRY;
	}
	else
		return OPT_ERR_TYPECHECK;
	return es_false;
}

static EsObject* getFieldValueForScope (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	return es_integer_new (tag->extensionFields.scopeIndex);
}

static EsObject* setFieldValueForScope (tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *obj)
{
	unsigned int index = es_integer_get (obj);
	if (index < countEntryInCorkQueue ())
	{
		tag->extensionFields.scopeIndex = (int)index;
		return es_false;
	}

	return OPTSCRIPT_ERR_NOTAGENTRY;
}

static EsObject* checkFieldValueForScope (const fieldDefinition *fdef, const EsObject *obj)
{
	if (!es_integer_p (obj))
		return OPT_ERR_TYPECHECK;

	if (es_integer_get (obj) < 0)
		return OPT_ERR_RANGECHECK;

	return es_false;
}

static EsObject* getFieldValueForExtras (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	if (!isTagExtra (tag))
		return es_nil;

	EsObject* a = opt_array_new ();

	for (unsigned int i = 0; i < countXtags (); i++)
	{
		if (!isTagExtraBitMarked (tag, i))
			continue;

		langType lang = getXtagLanguage (i);
		const char *lang_name = (lang == LANG_IGNORE)
			? NULL
			: getLanguageName (lang);
		const char *extra_name = getXtagName (i);

		EsObject *extra;
		if (lang_name == NULL)
			extra = opt_name_new_from_cstr (extra_name);
		else
		{
			vString *tmp = vStringNewInit (lang_name);
			vStringPut (tmp, '.');
			vStringCatS (tmp, extra_name);
			extra = opt_name_new_from_cstr (vStringValue (tmp));
			vStringDelete (tmp);
		}
		opt_array_add (a, extra);
		es_object_unref (extra);
	}
	return a;
}

static EsObject* getFieldValueForCOMMON (const char *field, const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	if (!field)
		return es_nil;
	return (opt_name_new_from_cstr (field));
}

static EsObject* setFieldValueForCOMMON (const char **field, tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *obj)
{
	if (*field)
		eFree ((char *)*field);

	const char *str = opt_string_get_cstr (obj);
	*field = eStrdup (str);
	return es_false;
}

static EsObject* getFieldValueForAccess (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	return getFieldValueForCOMMON(tag->extensionFields.access, tag, fdef);
}

static EsObject* setFieldValueForAccess (tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *obj)
{
	return setFieldValueForCOMMON(&tag->extensionFields.access, tag, fdef, obj);
}

static EsObject* getFieldValueForSignature (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	return getFieldValueForCOMMON(tag->extensionFields.signature, tag, fdef);
}

static EsObject* setFieldValueForSignature (tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *obj)
{
	return setFieldValueForCOMMON(&tag->extensionFields.signature, tag, fdef, obj);
}

static void makeRolesArray (const tagEntryInfo *const tag, int roleIndex, void *data)
{
	EsObject *a = data;

	const roleDefinition *role = getTagRole (tag, roleIndex);
	EsObject *r = opt_name_new_from_cstr (role->name);
	opt_array_add (a, r);
	es_object_unref (r);
}

static EsObject* getFieldValueForRoles (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	EsObject *a = opt_array_new ();

	if (!foreachRoleBits (tag, makeRolesArray, a))
	{
		EsObject *r = opt_name_new_from_cstr (ROLE_DEFINITION_NAME);
		opt_array_add (a, r);
		es_object_unref (r);
	}
	return a;
}

static EsObject* getFieldValueForLineCommon (const tagEntryInfo *tag, const fieldDefinition *fdef)
{
	if (fdef->ftype == FIELD_END_LINE)
		return ((int)tag->extensionFields._endLine == 0)
			? es_nil
			: es_integer_new ((int)tag->extensionFields._endLine);
	else
		return ((int)tag->lineNumber == 0)
			? es_nil
			: es_integer_new ((int)tag->lineNumber);
}

static EsObject* checkFieldValueForLineCommon (const fieldDefinition *fdef, const EsObject *obj)
{
	return es_false;
}

static EsObject* setFieldValueForLineCommon (tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *obj)
{
	unsigned int l;
	if (es_object_get_type (obj) == OPT_TYPE_MATCHLOC)
	{
		matchLoc *loc = es_pointer_get (obj);
		l = loc->line;
	}
	else if (es_integer_p (obj))
	{
		int l0 = es_integer_get (obj);
		if (l0 < 1)
			return OPT_ERR_RANGECHECK;

		l = (unsigned int)l0;
		/* If the new line number is too large,
		   we cannot fill tag->filePosition with
		   getInputFilePositionForLine(); */
		if (fdef->ftype == FIELD_LINE_NUMBER
			&& l > getInputLineNumber())
			return OPT_ERR_RANGECHECK;
	}
	else
		return OPT_ERR_TYPECHECK;

	if (fdef->ftype == FIELD_END_LINE)
		setTagEndLine(tag, (unsigned long)l);
	else
		updateTagLine (tag, l, getInputFilePositionForLine (l));

	return es_false;
}

static EsObject* setFieldValueForInherits (tagEntryInfo *tag, const fieldDefinition *fdef, const EsObject *obj)
{
	if (es_object_get_type (obj) == OPT_TYPE_STRING)
	{
		if (tag->extensionFields.inheritance)
			eFree ((void *)tag->extensionFields.inheritance);
		const char *str = opt_string_get_cstr (obj);
		tag->extensionFields.inheritance = eStrdup (str);
	}
	else if (es_object_equal (es_false, obj))
	{
		if (tag->extensionFields.inheritance)
		{
			eFree ((void *)tag->extensionFields.inheritance);
			tag->extensionFields.inheritance = NULL;
		}
	}
	else
		return OPT_ERR_RANGECHECK; /* true is not acceptable. */

	return es_false;
}
