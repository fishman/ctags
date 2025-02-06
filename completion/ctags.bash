#
#  Copyright (c) 2022, Masatake YAMATO
#  Copyright (c) 2022, Red Hat, Inc.
#
#  This source code is released for free distribution under the terms of the
#  GNU General Public License version 2 or (at your option) any later version.
#
# TODO:
# - Don't hard code column numbers when parsing --list-* output.
# - Pass --options=NONE --quiet when running ctags internally; broken .ctags can cause a trouble.
#

_ctags_opts_arg_extras()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local lang
    local L N l

    local letters=()
    local names=()

    case "$prev" in
	(--extras\=)
	    :
	    ;;
	(--extras-*\=)
	    local tmp=${prev#--extras-}
	    lang=${tmp%=*}
	    ;;
	(*)
	    return 1
	    ;;
    esac

    while read L N l; do
	if [[ -z "$lang" && "$l" == "NONE" ]] || [[ "$l" == "$lang" ]]; then
	    if [[ "$L" != '-' ]]; then
		letters+=($L)
	    fi
	    names+=('{'$N'}')
	fi
    done < <($ctags --with-list-header=no \
		    --machinable=yes \
		    --list-extras=$lang 2>/dev/null | cut -f1,2,4 -d$'\t')

    target=${cur##*\{*\}}
    if [[ "${target}" == "${target##*\{}" ]]; then
	OPTS+=(\\\\* + -)
	OPTS+=( "${letters[@]}" "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -W "${OPTS[*]}") )
    else
	target="${target##*\{}"
	cur="${cur%\{*}"
	OPTS+=( "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -P "$cur" -W "${OPTS[*]}" -- "{""$target" ) )
    fi
    return 0
}

_ctags_opts_arg_fields()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local lang
    local L N l

    local letters=()
    local names=()

    case "$prev" in
	(--fields\=)
	    :
	    ;;
	(--fields-*\=)
	    local tmp=${prev#--fields-}
	    lang=${tmp%=*}
	    ;;
	(*)
	    return 1
	    ;;
    esac

    while read L N l; do
	if [[ -z "$lang" && "$l" == "NONE" ]] || [[ "$l" == "$lang" ]]; then
	    if [[ "$L" != '-' ]]; then
		letters+=($L)
	    fi
	    if [[ "$N" != 'NONE' ]]; then
		names+=('{'$N'}')
	    fi
	fi
    done < <($ctags --with-list-header=no \
		    --machinable=yes \
		    --list-fields=$lang 2>/dev/null | cut -f1,2,4 -d$'\t')

    target=${cur##*\{*\}}
    if [[ "${target}" == "${target##*\{}" ]]; then
	OPTS+=(\\\\* + -)
	OPTS+=( "${letters[@]}" "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -W "${OPTS[*]}") )
    else
	target="${target##*\{}"
	cur="${cur%\{*}"
	OPTS+=( "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -P "$cur" -W "${OPTS[*]}" -- "{""$target" ) )
    fi
    return 0
}

_ctags_opts_arg_kinds()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local lang
    local L N

    local letters=()
    local names=()

    local target=
    local OPTS=()

    case "$prev" in
	--kinds-*\=)
	    :
	    ;;
	*)
	    return 1
	    ;;
    esac

    local tmp=${prev#--kinds-}
    lang=${tmp%=*}

    while read L N; do
	letters+=($L)
	names+=('{'$N'}')
    done  < <($ctags --with-list-header=no \
		     --machinable=yes \
		     --list-kinds-full=$lang 2>/dev/null | cut -f1,2 -d$'\t')

    target=${cur##*\{*\}}
    if [[ "${target}" == "${target##*\{}" ]]; then
	OPTS+=(\\\\* + -)
	OPTS+=( "${letters[@]}" "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -W "${OPTS[*]}") )
    else
	target="${target##*\{}"
	cur="${cur%\{*}"
	OPTS+=( "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -P "$cur" -W "${OPTS[*]}" -- "{""$target" ) )
    fi
    return 0
}

_ctags_opts_arg_ptags()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local N

    local names=()

    case "$prev" in
	(--pseudo-tags\=)
	    :
	    ;;
	(*)
	    return 1
	    ;;
    esac

    while read N; do
	names+=('{'$N'}')
    done < <($ctags --with-list-header=no \
		    --machinable=yes \
		    --list-pseudo-tags 2>/dev/null | cut -f1 -d$'\t')

    target=${cur##*\{*\}}
    if [[ "${target}" == "${target##*\{}" ]]; then
	OPTS+=(\\\\* + -)
	OPTS+=( "${letters[@]}" "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -W "${OPTS[*]}") )
    else
	target="${target##*\{}"
	cur="${cur%\{*}"
	OPTS+=( "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -P "$cur" -W "${OPTS[*]}" -- "{""$target" ) )
    fi
    return 0
}

_ctags_opts_arg_roles()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local lang kind
    local K R

    local roles=()

    local target=
    local OPTS=()

    case "$prev" in
	--roles-*\=)
	    :
	    ;;
	*)
	    return 1
	    ;;
    esac

    local tmp=${prev#--roles-}
    tmp=${tmp%=}
    lang=${tmp%.*}
    kind=${tmp#*.}

    while read K R; do
	if [[ "${kind:0:1}" == '{' ]]; then
	    if [[ '{'${K:2}'}' == "$kind" ]]; then
		roles+=('{'$R'}')
	    fi
	else
	    if [[ ${K:0:1} == "$kind" ]]; then
		roles+=('{'$R'}')
	    fi
	fi
    done  < <($ctags --with-list-header=no \
		     --machinable=yes \
		     --list-roles=$lang.$kind 2>/dev/null | cut -f1,2 -d$'\t')

    local target=${cur##*\{*\}}
    if [[ "${target}" == "${target##*\{}" ]]; then
	OPTS+=(\\\\* + -)
	OPTS+=( "${roles[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -W "${OPTS[*]}") )
    else
	target="${target##*\{}"
	cur="${cur%\{*}"
	OPTS+=( "${roles[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -P "$cur" -W "${OPTS[*]}" -- "{""$target" ) )
    fi
    return 0
}

_ctags_opts_arg_yes_or_no()
{
    local prev=$1
    local cur=$2

    case $prev in
	(--filter\=|\
	    --links\=|\
	    --recurse\=|\
	    --append\=|\
	    --use-slash-as-filename-separator\=|\
	    --if0\=|\
	    --line-directives\=|\
	    --machinable\=|\
	    --with-list-header\=|\
	    --quiet=|\
	    --verbose\=)
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "yes no" -- "$cur") )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_arg_language_or_all()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local OPTS

    case $prev in
	(--language-force\=|\
	    --list-aliases\=|\
	    --list-extras\=|\
	    --list-fields\=|\
	    --list-kinds\=|\
	    --list-kinds-full\=|\
	    --list-map-extensions\=|\
	    --list-map-patterns\=|\
	    --list-maps\=|\
	    --list-params\=)
	    OPTS=(
		all
		$($ctags --list-languages | grep -v '\[disabled\]')
	    )
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "${OPTS[*]}" -- "$cur") )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_arg_language_having_subparsers_or_all()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local OPTS

    case $prev in
	(--list-subparsers\=)
	    OPTS=(
		all
		$($ctags --with-list-header=no \
			 --machinable=yes \
			 --list-subparsers 2>/dev/null | cut -f2 -d$'\t' | uniq)
	    )
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "${OPTS[*]}" -- "$cur") )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_arg_language_kind()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local L P N
    local OPTS=()

    case $prev in
	(--list-roles\=)
	;;
	(*)
	    return 1
	    ;;
    esac

    local lang=${cur%%.*}
    local kinds=${cur#"$lang".}

    if [[ "$lang" == "${cur}" ]]; then
	OPTS+=(all)
	while read L; do
	    OPTS+=(${L}.)
	done < <($ctags --list-languages | grep -v '\[disabled\]')
	compopt -o nospace
	COMPREPLY=( $(compgen -W "${OPTS[*]}" -- "$lang") )
    elif [[ "$lang" == "all" ]]; then
	:
    else
	local letters=()
	local names=()

	while read L N; do
	    letters+=($L)
	    names+=('{'$N'}')
	done  < <($ctags --with-list-header=no \
			 --machinable=yes \
			 --list-kinds-full=$lang 2>/dev/null | cut -f1,2 -d$'\t')

	local target=${cur##*\{*\}}
	if [[ "${target}" == "${target##*\{}" ]]; then
	    OPTS+=(\\\\*)
	    OPTS+=( "${letters[@]}" "${names[@]}" )
	    compopt -o nospace
	    COMPREPLY=( $(compgen -W "${OPTS[*]}") )
	else
	    target="${target##*\{}"
	    cur="${cur%\{*}"
	    OPTS+=( "${names[@]}" )
	    compopt -o nospace
	    COMPREPLY=( $(compgen -P "$cur" -W "${OPTS[*]}" -- "{""$target" ) )
	fi
    fi
    return 0
}

_ctags_opts_arg_language-list_or_all()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local OPTS=( all )

    case $prev in
	(--languages\=)
	;;
	(*)
	    return 1
	    ;;
    esac

    if [[ -z "${cur}" ]]; then
	OPTS+=(+ -)
    fi

    local target prefix
    local c=${cur:0:1}
    local cur0
    case "$c" in
	(+|-)
	    cur0=${cur:1}
	    ;;
	(*)
	    c=
	    cur0=$cur
    esac

    local target=${cur0##*,}
    local prefix=$c${cur0%${target}}

    while read L; do
	OPTS+=(${L})
	if [[ "$L" == "$target" ]]; then
	    included="${L},"
	fi
    done < <($ctags --list-languages |
		 grep -v '\[disabled\]')

    OPTS+=($included)

    COMPREPLY=( $(compgen -P "$prefix" -W "${OPTS[*]}" -- "$target") )
    compopt -o nospace
    compopt -o nosort
    return 0
}

_ctags_opts_arg_file()
{
    local prev=$1

    case $prev in
	(--etags-include\=|\
	    --options\=|\
	    --options-maybe\=)
	    local IFS=$'\n'
	    compopt -o filenames
	    COMPREPLY=( $(compgen -f -- ${cur:-"/"}) )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_arg_dir()
{
    local prev=$1

    case $prev in
	(--optib-dir\=)
	    local IFS=$'\n'
	    compopt -o dirnames
	    COMPREPLY=( $(compgen -d -- ${cur:-"/"}) )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_arg()
{
    local ctags=$1
    local prev=$2
    local cur=$3

    local OPTS

    _ctags_opts_arg_yes_or_no "$prev" "$cur" && return 0
    _ctags_opts_arg_language-list_or_all "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_language_or_all "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_language_having_subparsers_or_all "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_language_kind "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_fields "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_extras "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_kinds "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_ptags "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_roles "$ctags" "$prev" "$cur" && return 0
    _ctags_opts_arg_file "$prev" && return 0
    _ctags_opts_arg_dir "$prev" && return 0

    case $prev in
	(--format\=)
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "1 2" -- "$cur") )
	    return 0
	    ;;
	(--output-format\=)
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "u-ctags e-ctags etags xref json" -- "$cur") )
	    return 0
	    ;;
	(--sort\=)
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "yes no foldcase" -- "$cur") )
	    return 0
	    ;;
	(--excmd\=)
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "number pattern mix combine" -- "$cur") )
	    return 0
	    ;;
	(--tag-relative\=)
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "yes no always never" -- "$cur") )
	    return 0
	    ;;
	(--totals\=)
	    compopt -o nosort
	    COMPREPLY=( $(compgen -W "yes no extra" -- "$cur") )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_eq()
{
    local cur=$1

    case $cur in
	(--filter |\
	    --links |\
	    --recurse |\
	    --append |\
	    --use-slash-as-filename-separator |\
	    --if0 |\
	    --line-directives |\
	    --list-aliases |\
	    --list-extras |\
	    --list-fields |\
	    --list-kinds |\
	    --list-kinds-full |\
	    --list-map-extensions |\
	    --list-map-patterns |\
	    --list-maps |\
	    --list-params |\
	    --list-roles |\
	    --list-subparsers |\
	    --machinable |\
	    --with-list-header |\
	    --quiet |\
	    --totals |\
	    --verbose)
	    compopt -o nospace
	    COMPREPLY=( $(compgen -P $cur -W "=") )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_list()
{
    local OPTS=(
	--list-aliases
	--list-excludes
	--list-extras
	--list-features
	--list-fields
	--list-kinds
	--list-kinds-full
	--list-languages
	--list-map-extensions
	--list-map-patterns
	--list-maps
	--list-mline-regex-flags
	--list-params
	--list-pseudo-tags
	--list-regex-flags
	--list-roles
	--list-subparsers)

    case $cur in
	(--list-*)
	    COMPREPLY=( $(compgen -W "${OPTS[*]}" -- "$cur") )
	    compopt -o nospace
	    return 0
	    ;;
    esac
    return 1
}

_ctags_file()
{
    local prev=$1
    local cur=$2

    case $prev in
	(-L|-f|-o)
	    local IFS=$'\n'
	    compopt -o filenames
	    COMPREPLY=('-'  $(compgen -f -- ${cur:-"/"}) )
	    return 0
	    ;;
    esac
    return 1
}

_ctags_opts_lang_eq()
{
    local ctags=$1
    local cur=$2
    local L P
    local OPTS=()

    case "$cur" in
	(--input-endocing-*)
	    P=--input-encoding-
	    cur=${cur#--input-encoding-}
	    ;;
	(--alias-*)
	    P=--alias-
	    cur=${cur#--alias-}
	    ;;
	(--map-*)
	    P=--map-
	    cur=${cur#--map-}
	    ;;
	(--extras-*)
	    P=--extras-
	    cur=${cur#--extras-}
	    ;;
	(--fields-*)
	    P=--fields-
	    cur=${cur#--fields-}
	    ;;
	(--kinds-*)
	    P=--kinds-
	    cur=${cur#--kinds-}
	    ;;
	(*)
	    return 1
	    ;;
    esac

    while read L; do
	OPTS+=(${L}=)
    done < <($ctags --list-languages |
		 grep -v '\[disabled\]')
    COMPREPLY=( $(compgen -P $P -W "${OPTS[*]}" -- "$cur") )
    [[ ${COMPREPLY-} == *= ||  ${COMPREPLY-} == *- ]] && compopt -o nospace
    return 0
}

_ctags_opts_lang_param_eq()
{
    local ctags=$1
    local cur=$2
    local L P N
    local OPTS=()

    case "$cur" in
	(--param-*)
	    P=--param-
	    cur=${cur#--param-}
	    ;;
	(*)
	    return 1
	    ;;
    esac

    local lang=${cur%%.*}
    local param=${cur#"$lang".}

    if [[ "$lang" == "${cur}" ]]; then
	while read L; do
	    OPTS+=(${L}.)
	done < <($ctags --with-list-header=no \
			--machinable=yes \
			--list-params 2>/dev/null | cut -f1 -d$'\t')
	compopt -o nospace
	COMPREPLY=( $(compgen -P $P -W "${OPTS[*]}" -- "$lang") )
    else
	while read N; do
	    OPTS+=(${N}'=')
	done < <($ctags --with-list-header=no \
			--machinable=yes \
			--list-params=$lang 2>/dev/null | cut -f1 -d$'\t')
	compopt -o nospace
	COMPREPLY=( $(compgen -P $P$lang. -W "${OPTS[*]}" -- "$param") )
    fi
    return 0
}

_ctags_opts_lang_kind_eq()
{
    local ctags=$1
    local cur=$2
    local L P N
    local OPTS=()

    case "$cur" in
	(--roles-*)
	    P=--roles-
	    cur=${cur#--roles-}
	    ;;
	(*)
	    return 1
	    ;;
    esac

    local lang=${cur%%.*}
    local kind=${cur#"$lang".}

    if [[ "$lang" == "${cur}" ]]; then
	while read L; do
	    OPTS+=(${L}.)
	done < <($ctags --list-languages | grep -v '\[disabled\]')
	compopt -o nospace
	COMPREPLY=( $(compgen -P $P -W "${OPTS[*]}" -- "$lang") )
    else
	local letters=()
	local names=()

	while read L N; do
	    letters+=($L'=')
	    names+=('{'$N'}=')
	done < <($ctags --with-list-header=no \
			--machinable=yes \
			--list-kinds-full=$lang 2>/dev/null | cut -f1,2 -d$'\t')

	OPTS+=( "${letters[@]}" "${names[@]}" )
	compopt -o nospace
	COMPREPLY=( $(compgen -P $P${lang}. -W "${OPTS[*]}" -- "$kind") )
    fi
    return 0
}

_ctags()
{
    local ctags="${COMP_WORDS[0]}"
    local pprev cur OPTS pprev
    COMPREPLY=()

    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    pprev="${COMP_WORDS[COMP_CWORD-2]}"

    _ctags_opts_arg "$ctags" "$pprev$prev" "$cur" && return 0
    _ctags_opts_eq "$cur" && return 0
    _ctags_opts_list "$cur" && return 0
    _ctags_opts_lang_eq "$ctags" "$cur" && return 0
    _ctags_opts_lang_param_eq "$ctags" "$cur" && return 0
    _ctags_opts_lang_kind_eq "$ctags" "$cur" && return 0
    _ctags_file "$prev" "$cur" && return 0

    case $cur in
	(-*)
	    OPTS=(
		# Input/Output File Options
		--exclude=
		--exclude-exception=
		--filter
		--filter-terminator=
		--links
		--maxdepth=
		--recurse -R
		-L
		--append -a
		-f -o
		--format=
		# Output Format Options
		--output-format= -e -x
		--sort= -u
		--etags-include=
		## TODO encoding {{
		--input-encoding=
		--input-encoding-
		--output-encoding=
		## }}
		# Language Selection and Mapping Options
		--language-force=
		--languages=
		--alias-
		--guess-language-eagerly -G
		## TODO {{
		--langmap=
		## }}
		--map-
		# Tags File Contents Options
		--excmd=
		-n -N
		--extras=
		--extras-
		--fields=
		--fields-
		--kinds-
		--pattern-length-limit=
		--pseudo-tags=
		--put-field-prefix
		--roles-
		--tag-relative=
		--use-slash-as-filename-separator
		-B -F
		# Option File Options
		--options=
		--options-maybe=
		--optib-dir=
		# optlib Options
		# Language Specific Options
		--if0
		--line-directives
		-D
		-h
		-I
		--param-
		# Listing Options
		--list-
		--machinable
		--with-list-header
		# Miscellaneous Options
		--help '-?'
		--help-full
		--license
		--print-language
		--quiet
		--totals
		--verbose -V
		--version
	    )
	    COMPREPLY=( $(compgen -W "${OPTS[*]}" -- $cur) )
	    [[ ${COMPREPLY-} == *= ||  ${COMPREPLY-} == *- ]] && compopt -o nospace
	    return 0
	    ;;
	(\=)
	    cur=${cur:1}
	    _ctags_opts_arg "$ctags" "$prev"= "$cur" && return 0
	    ;;
    esac

    local IFS=$'\n'
    compopt -o filenames
    COMPREPLY=( $(compgen -f -- ${cur:-"/"}) )
    return 0
}
complete -F _ctags ctags
