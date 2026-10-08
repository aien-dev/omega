#!/bin/bash
# bash_verify.sh: prove the expectations in bash_vectors.tsv against REAL bash 5.2 (not the reference lexer).
# For each vector it runs the input under bash and the "equivalent" script (the continuation-free form, or the
# script rendered from the expected tokens when the equiv column is "-") and requires identical transcripts
# (stdout, stderr, and the files written). Usage: bash_verify.sh [bash_vectors.tsv]   Writes bash_verified.log.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tsv=${1:-$here/bash_vectors.tsv}
ver=$(bash --version | head -1)
case $BASH_VERSION in 5.2.*) ;; *) echo "bash_verify: need bash 5.2.x, have $BASH_VERSION" >&2; exit 2;; esac
tmp=$(mktemp -d "${TMPDIR:-/tmp}/bashverify.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/prel.sh" <<'P'
e() { printf 'out:'; printf '[%s]' "$@"; printf '\n'; echo errmark >&2; }
P
transcript() { # script text -> transcript on stdout
	local d; d=$(mktemp -d "$tmp/run.XXXXXX")
	printf '%s' "$1" > "$d/in.sh"
	( cd "$d" && HOME=/HOMEX bash -c '. "$1"; . ./in.sh; e "x=$x"' bash "$tmp/prel.sh" 2>&1 )
	local f
	for f in "$d"/*; do
		[ "$(basename "$f")" = in.sh ] && continue
		printf 'file %s:' "$(basename "$f")"; cat "$f"
	done
}
mask() { sed -E -e '/: line [0-9]+: `/d' -e 's/line [0-9]+/line N/g' -e 's/[0-9]{4,}/PID/g' -e 's#/[a-z/]*/bashverify\.[A-Za-z0-9]+#TMP#g'; }
dec() { printf '%b' "$1"; }
squote() { local s=$1; printf "'%s'" "${s//\'/\'\\\'\'}"; }
render() { # expect tokens -> script
	local out="" t w
	for t in $1; do
		case $t in
		W:*) w=$(dec "${t#W:}"; printf x); out+=" $(squote "${w%x}")" ;;
		PIPE) out+=" |" ;; OR) out+=" ||" ;; AND) out+=" &&" ;; SEMI) out+=" ;" ;;
		NL) out+=$'\n' ;;
		LT:*) out+=" ${t#LT:}<" ;; GT:*) out+=" ${t#GT:}>" ;; APPEND:*) out+=" ${t#APPEND:}>>" ;;
		DUPO:*) out+=" ${t#DUPO:}>&" ;; DUPI:*) out+=" ${t#DUPI:}<&" ;;
		*) echo "bash_verify: bad token $t" >&2; return 1 ;;
		esac
	done
	printf '%s' "$out"
}
n=0; bad=0
log="$here/bash_verified.log"
{ echo "bash: $ver"; echo "file: bash_vectors.tsv"; } > "$log"
while IFS=$'\t' read -r id inp exp eq; do
	case $id in c*) ;; *) continue ;; esac
	n=$((n + 1))
	in=$(dec "$inp"; printf x); in=${in%x}
	if [ "$eq" = "-" ]; then
		set -- $exp
		shift # ok
		alt=$(render "$*"; printf x); alt=${alt%x}
	else
		alt=$(dec "$eq"; printf x); alt=${alt%x}
	fi
	t1=$(transcript "$in" | mask); t2=$(transcript "$alt" | mask)
	if [ "$t1" = "$t2" ]; then
		echo "$id same $(printf '%s' "$t1" | tr '\n' '|')" >> "$log"
	else
		bad=$((bad + 1)); echo "$id DIFFERENT" | tee -a "$log"
		printf '  input:    %s\n  equiv:    %s\n' "$(printf '%s' "$t1" | tr '\n' '|')" "$(printf '%s' "$t2" | tr '\n' '|')" | tee -a "$log"
	fi
done < "$tsv"
echo "vectors: $n, mismatches: $bad ($ver)" | tee -a "$log"
[ "$n" -ge 40 ] && [ "$bad" -eq 0 ]
