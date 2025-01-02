# Copyright: 2025 Masatake YAMATO
# License: GPL-2

. ../utils.sh

CTAGS=$1

V=
# V=valgrind

is_feature_available "${CTAGS}" json

echo "# output: json"
${V} ${CTAGS} --options=NONE --options=./knownz.ctags --sort=no --options=./unknownx.ctags \
	 --fields=+l \
	 --fields-unknownx=+'{protection}{signature}' \
	 --fields-knownz=+'{owner}' \
	 --output-format=json \
	 -o - input.unknownx
