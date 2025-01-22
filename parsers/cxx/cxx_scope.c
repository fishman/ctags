/*
*   Copyright (c) 2016, Szymon Tomasz Stefanek
*
*   This source code is released for free distribution under the terms of the
*   GNU General Public License version 2 or (at your option) any later version.
*
*   This module contains functions for parsing and scanning C++ source files
*/

#include "cxx_scope.h"

#include "vstring.h"
#include "debug.h"

#include "cxx_tag.h"
#include "cxx_debug.h"
#include "cxx_token_chain.h"

#ifdef CXX_DO_DEBUGGING
#include "cxx_parser_internal.h"
#endif

// The tokens defining current scope
static CXXTokenChain * g_pScope = NULL;
static vString * g_szScopeName = NULL;
static bool g_bScopeNameDirty = true;

void cxxScopeInit(void)
{
	g_pScope = cxxTokenChainCreate();
}

void cxxScopeDone(void)
{
	cxxTokenChainDestroy(g_pScope);
	if(g_szScopeName)
	{
		vStringDelete(g_szScopeName);
		g_szScopeName = NULL;
	}
}

void cxxScopeClear(void)
{
	if(g_pScope)
		cxxTokenChainClear(g_pScope);
	if(g_szScopeName)
	{
		vStringDelete(g_szScopeName);
		g_szScopeName = NULL;
	}
}

bool cxxScopeIsGlobal(void)
{
	return (g_pScope->iCount < 1);
}

bool cxxScopeIsExported(void)
{
	if (g_pScope->pTail)
		return g_pScope->pTail->bInternalScopeExported;
	return false;
}

enum CXXScopeType cxxScopeGetType(void)
{
	if(g_pScope->iCount < 1)
		return CXXScopeTypeNamespace;
	return (enum CXXScopeType)g_pScope->pTail->uInternalScopeType;
}

unsigned int cxxScopeGetVariableKind(void)
{
	switch(cxxScopeGetType())
	{
		case CXXScopeTypeClass:
		case CXXScopeTypeUnion:
		case CXXScopeTypeStruct:
			return CXXTagKindMEMBER;
		break;
		case CXXScopeTypeFunction:
			return CXXTagKindLOCAL;
		break;
		//case CXXScopeTypePrototype:
		//case CXXScopeTypeNamespace:
		//case CXXScopeTypeEnum:
		default:
			// fall down
		break;
	}
	return CXXTagKindVARIABLE;
}


unsigned int cxxScopeGetKind(void)
{
	CXX_DEBUG_ASSERT(g_pScope->iCount >= 0,"Must not be called in global scope");

	switch(g_pScope->pTail->uInternalScopeType)
	{
		case CXXScopeTypeNamespace:
			CXX_DEBUG_ASSERT(cxxParserCurrentLanguageIsCPP(),"C++ only");
			return CXXTagCPPKindNAMESPACE;
		case CXXScopeTypeClass:
			CXX_DEBUG_ASSERT(cxxParserCurrentLanguageIsCPP(),"C++ only");
			return CXXTagCPPKindCLASS;
		case CXXScopeTypeEnum:
			return CXXTagKindENUM;
		case CXXScopeTypeFunction:
			return CXXTagKindFUNCTION;
		case CXXScopeTypePrototype:
			return CXXTagKindPROTOTYPE;
		case CXXScopeTypeStruct:
			return CXXTagKindSTRUCT;
		case CXXScopeTypeUnion:
			return CXXTagKindUNION;
		case CXXScopeTypeVariable:
			return CXXTagKindVARIABLE;
		case CXXScopeTypeTypedef:
			return CXXTagKindTYPEDEF;
		case CXXScopeTypeModule:
			return CXXTagCPPKindMODULE;
		default:
			CXX_DEBUG_ASSERT(false,"Unhandled scope type!");
			break;
	}

	return CXXTagKindFUNCTION;
}


enum CXXScopeAccess cxxScopeGetAccess(void)
{
	if(g_pScope->iCount < 1)
		return CXXScopeAccessUnknown;
	return (enum CXXScopeAccess)g_pScope->pTail->uInternalScopeAccess;
}

const char * cxxScopeGetName(void)
{
	if(g_pScope->iCount < 1)
		return NULL;
	return vStringValue(g_pScope->pTail->pszWord);
}

int cxxScopeGetDefTag(void)
{
	if(g_pScope->iCount < 1)
		return CORK_NIL;
	return g_pScope->pTail->iCorkIndex;
}

int cxxScopeGetSize(void)
{
	return g_pScope->iCount;
}

const char * cxxScopeGetFullName(void)
{
	if(!g_bScopeNameDirty)
		return g_szScopeName ? vStringValue(g_szScopeName): NULL;

	if(g_pScope->iCount < 1)
	{
		g_bScopeNameDirty = false;
		return NULL;
	}

	g_szScopeName = vStringNewOrClear(g_szScopeName);

	cxxTokenChainJoinInString(
			g_pScope,
			g_szScopeName,
			"::",
			CXXTokenChainJoinNoTrailingSpaces
		);

	g_bScopeNameDirty = false;
	return vStringValue(g_szScopeName);
}

vString * cxxScopeGetFullNameAsString(void)
{
	vString * ret;

	if(!g_bScopeNameDirty)
	{
		ret = g_szScopeName;
		g_szScopeName = NULL;
		g_bScopeNameDirty = true;
		return ret;
	}

	if(g_pScope->iCount < 1)
		return NULL;

	g_szScopeName = vStringNewOrClear(g_szScopeName);

	cxxTokenChainJoinInString(
			g_pScope,
			g_szScopeName,
			"::",
			CXXTokenChainJoinNoTrailingSpaces
		);

	ret = g_szScopeName;
	g_szScopeName = NULL;
	return ret;
}

vString * cxxScopeGetFullNameExceptLastComponentAsString(void)
{
	if(g_pScope->iCount < 2)
		return NULL;

	return cxxTokenChainJoinRange(
			g_pScope->pHead,
			g_pScope->pTail->pPrev,
			"::",
			CXXTokenChainJoinNoTrailingSpaces
		);
}


void cxxScopeSetAccess(enum CXXScopeAccess eAccess)
{
	if(g_pScope->iCount < 1)
		return; // warning?
	g_pScope->pTail->uInternalScopeAccess = (unsigned char)eAccess;
}

void cxxScopePushTop(CXXToken * t)
{
	CXX_DEBUG_ASSERT(
			cxxTokenTypeIs(t, CXXTokenTypeIdentifier),
			"The scope name must be an identifier"
		);
	CXX_DEBUG_ASSERT(
			t->pszWord,
			"The scope name should have a text"
		);

	// Inherit the export'ed status from the parent scope.
	// You can override the inherited status with cxxScopePushExported().
	if (g_pScope->pTail && g_pScope->pTail->bInternalScopeExported)
		t->bInternalScopeExported = true;

	cxxTokenChainAppend(g_pScope,t);
	g_bScopeNameDirty = true;

	CXX_DEBUG_PRINT("Pushed scope: '%s'",cxxScopeGetFullName() ?: "");
}

CXXToken * cxxScopeTakeTop(void)
{
	CXX_DEBUG_ASSERT(
			g_pScope->iCount > 0,
			"When popping as scope there must be a scope to pop"
		);
	CXX_DEBUG_PRINT("Popped scope: '%s'",cxxScopeGetFullName() ?: "");

	CXXToken * t = cxxTokenChainTakeLast(g_pScope);
	g_bScopeNameDirty = true;

	return t;
}

void cxxScopePush(
		CXXToken * t,
		enum CXXScopeType eScopeType,
		enum CXXScopeAccess eInitialAccess
	)
{
	t->uInternalScopeType = (unsigned char)eScopeType;
	t->uInternalScopeAccess = (unsigned char)eInitialAccess;
	cxxScopePushTop(t);
}

void cxxScopePushExported(
		CXXToken * t,
		enum CXXScopeType eScopeType,
		enum CXXScopeAccess eInitialAccess,
		bool exported
	)
{
	cxxScopePush(t, eScopeType, eInitialAccess);
	// Overrite the default value inherited from the parent scope.
	t->bInternalScopeExported = exported;
}

void cxxScopePop(void)
{
	cxxTokenDestroy(cxxScopeTakeTop());
}
