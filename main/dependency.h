/*
 *
 *  Copyright (c) 2016, Red Hat, Inc.
 *  Copyright (c) 2016, Masatake YAMATO
 *
 *  Author: Masatake YAMATO <yamato@redhat.com>
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 *
 */
#ifndef CTAGS_MAIN_DEPENDENCY_H
#define CTAGS_MAIN_DEPENDENCY_H

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include "types.h"


/*
*   DATA DECLARATIONS
*/
typedef enum eDepType {
	DEPTYPE_KIND_OWNER,
	DEPTYPE_SUBPARSER,
	DEPTYPE_FOREIGNER,
	COUNT_DEPTYPES,
} depType;

struct sParserDependency {
	depType type;
	const char *upperParser;
	void *data;
};

struct sSlaveParser {
	depType type;
	langType id;
	void *data;
	slaveParser *next;
};

/* These are for CPreProcessor.
 * Don't use in the other parsers. */
extern void notifyInputStart (void);
extern void notifyInputEnd   (void);

#endif	/* CTAGS_MAIN_DEPENDENCY_H */
