#!/bin/sh

# Copyright: 2024 Masatake YAMATO
# License: GPL-2

READTAGS=$3
#V="valgrind --leak-check=full --track-origins=yes -v"
V=

. ../utils.sh

skip_if_no_readtags "$READTAGS"

echo '# FILTER' &&
${V} ${READTAGS} -t input.tags -Q '(#/.*CWD.*/ $name)' -D &&

echo '# FORMATTER' &&
${V} ${READTAGS} -t input.tags -Q '(#/.*CWD.*/ $name)' -F '(list $input #t)' -D &&

echo '# SORTING INPUT' &&
${V} ${READTAGS} -t input.tags -S '(<> $input &input)' -D &&

echo '# SORTING ! INPUT' &&
${V} ${READTAGS} -t input.tags -S '(*- (<> $input &input))' -D
