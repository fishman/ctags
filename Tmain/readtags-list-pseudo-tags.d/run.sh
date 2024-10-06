#!/bin/sh

# Copyright: 2020 Masatake YAMATO
# License: GPL-2

READTAGS=$3
#V="valgrind --leak-check=full --track-origins=yes -v"
V=

. ../utils.sh

skip_if_no_readtags "$READTAGS"

echo '# SORT=NO (-D)'
${V} ${READTAGS} -t ./ptag-sort-no.tags -D

echo '# SORT=NO (-P)'
${V} ${READTAGS} -t ./ptag-sort-no.tags -P no-such-tags

echo '# SORT=YES (-D)'
${V} ${READTAGS} -t ./ptag-sort-yes.tags -D

echo '# SORT=YES (-P)'
${V} ${READTAGS} -t ./ptag-sort-yes.tags -P no-such-tags

echo '# -D + FILTER (-D)'
${V} ${READTAGS} -t ./ptag-sort-yes.tags -Q '(#/MA/i $name)' -D

echo '# -P -l + FILTER (-D)'
${V} ${READTAGS} -t ./ptag-sort-yes.tags -Q '(#/MA/i $name)' -P -l
