#!/bin/sh
# corpus_guard.sh: the J1 driver refuses a corpus it must not trust, with a typed line.
#   corpus_guard.sh DRIVER UNIT CORPUS CASE_OFFSET WORKDIR
# CASE_OFFSET is the "case section at byte N" the driver printed for the real corpus. Each guard
# edits a copy and expects one "corpus refused: CODE" line and exit 3:
#   G1 trailer byte flipped                         -> CORPUS_FINGERPRINT
#   G2 label byte changed, trailer recomputed       -> CORPUS_PIN (parity alone would still pass)
#   G3 first case position set to 0xFFFFFFF0        -> CORPUS_RANGE (pin overridden to reach the check)
#   G4 first case base index set to 0xFFFF          -> CORPUS_RANGE (pin overridden)
# POSIX sh, dd, head, sha256sum, xxd.
set -u
drv=${1:?driver} unit=${2:?unit} corpus=${3:?corpus} off=${4:?case offset} dir=${5:?workdir}
mkdir -p "$dir"
fails=0

poke() { # file offset octal-escaped-bytes
	printf "$3" | dd of="$1" bs=1 seek="$2" conv=notrunc status=none
}
reseal() { # recompute the SHA-256 trailer of a copy; prints the new whole-file hash (the pin form)
	n=$(($(wc -c < "$1") - 32))
	h=$(head -c "$n" "$1" | sha256sum | cut -c1-64)
	printf '%s' "$h" | xxd -r -p | dd of="$1" bs=1 seek="$n" conv=notrunc status=none
	sha256sum "$1" | cut -c1-64
}
expect() { # name code file [pin override]
	name=$1 code=$2 file=$3 ov=${4:-}
	SYN_CORPUS_PIN_OVERRIDE=$ov "$drv" "$unit" "$file" 0 > "$dir/$name.out" 2>&1
	rc=$?
	line=$(grep '^corpus refused: ' "$dir/$name.out" | head -1)
	case "$rc:$line" in
	"3:corpus refused: $code:"*) echo "$name: OK exit 3, $line" ;;
	*) echo "$name: WRONG exit $rc, want $code; got: $(grep -v '^FAIL' "$dir/$name.out" | tail -2 | tr '\n' ' ')"; fails=$((fails + 1)) ;;
	esac
}

n=$(wc -c < "$corpus")
cp "$corpus" "$dir/g1.bin"
last=$(tail -c 1 "$dir/g1.bin" | xxd -p)
if [ "$last" = 00 ]; then poke "$dir/g1.bin" $((n - 1)) '\001'; else poke "$dir/g1.bin" $((n - 1)) '\000'; fi
expect G1 CORPUS_FINGERPRINT "$dir/g1.bin"

cp "$corpus" "$dir/g2.bin"
lab=$(grep -obUa 'fixture record 0' "$dir/g2.bin" | head -1 | cut -d: -f1)
poke "$dir/g2.bin" "$lab" 'F'
reseal "$dir/g2.bin" > /dev/null
expect G2 CORPUS_PIN "$dir/g2.bin"

cp "$corpus" "$dir/g3.bin"
poke "$dir/g3.bin" $((off + 3)) '\360\377\377\377'
h3=$(reseal "$dir/g3.bin")
expect G3 CORPUS_RANGE "$dir/g3.bin" "$h3"

cp "$corpus" "$dir/g4.bin"
poke "$dir/g4.bin" "$off" '\377\377'
h4=$(reseal "$dir/g4.bin")
expect G4 CORPUS_RANGE "$dir/g4.bin" "$h4"

if [ "$fails" -eq 0 ]; then echo "corpus guard: PASS (4 of 4 refused with the expected code)"; else echo "corpus guard: FAIL ($fails wrong)"; exit 1; fi
