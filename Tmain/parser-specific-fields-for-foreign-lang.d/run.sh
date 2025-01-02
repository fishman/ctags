# Copyright: 2024 Masatake YAMATO
# License: GPL-2

. ../utils.sh

CTAGS=$1

V=
# V=valgrind

echo "# output: tags"
${V} ${CTAGS} --options=NONE --options=./knownz.ctags --sort=no --options=./unknownx.ctags \
	 --fields=+l \
	 --fields-unknownx=+'{protection}{signature}' \
	 --fields-knownz=+'{owner}' \
	 -o - input.unknownx

echo "# output: xref"
${V} ${CTAGS} --options=NONE --options=./knownz.ctags --sort=no --options=./unknownx.ctags \
	 --fields=+l \
	 --fields-unknownx=+'{protection}{signature}' \
	 --fields-knownz=+'{owner}' \
	 -x --_xformat="%N %l / owner:%{knownz.owner},len:%{knownz.len},lenplus:%{knownz.lenplus},exported:%{knownz.exported},stb:%{knownz.stb} / %{unknownx.protection}%{unknownx.signature}" \
	 -o - input.unknownx
