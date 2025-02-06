/*
 *
 *  Copyright (c) 2017, Red Hat, Inc.
 *  Copyright (c) 2017, Masatake YAMATO
 *
 *  Author: Masatake YAMATO <yamato@redhat.com>
 *
 *   This source code is released for free distribution under the terms of the
 *   GNU General Public License version 2 or (at your option) any later version.
 *
 */
#ifndef CTAGS_MAIN_SUBPARSER_PRIVATE_H
#define CTAGS_MAIN_SUBPARSER_PRIVATE_H

/*
*   INCLUDE FILES
*/
#include "general.h"  /* must always come first */

#include "colprint_p.h"
#include "dependency_p.h"
#include "types.h"

/*
*   FUNCTION PROTOTYPES
*/
extern subparser *getFirstSubparser(struct slaveControlBlock *controlBlock);

/* A base parser doesn't have to call the following three functions.
   The main part calls them internally. */
extern void notifyMakeTagEntry (const tagEntryInfo *info, int corkIndex);

extern void setupSubparsersInUse (struct slaveControlBlock *controlBlock);
extern subparser* teardownSubparsersInUse (struct slaveControlBlock *controlBlock);

extern struct colprintTable * subparserColprintTableNew (void);
extern void subparserColprintAddSubparsers (struct colprintTable *table,
											struct slaveControlBlock *scb);
extern void subparserColprintTablePrint (struct colprintTable *table,
										 bool withListHeader, bool machinable, FILE *fp);

extern void useDefaultSubparsers (struct slaveControlBlock *controlBlock);
extern void useSpecifiedSubparser (struct slaveControlBlock *controlBlock, subparser *s);

#endif	/* CTAGS_MAIN_SUBPARSER_PRIVATE_H */
