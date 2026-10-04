#!/bin/sh
# Test of pd0b_run.sh law-record retention and validity, with a fake harness in a throw-away git repo.
set -eu
SRC=$(pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0; check() { if ! eval "$1"; then echo "FAIL: $1"; fail=1; fi; }
mkdir -p "$T/tests/physics0/learner" "$T/src/physics0/learner" "$T/src/physics0/ladder" "$T/src/physics0/score" "$T/src/physics0/controls" "$T/tests/physics0/verify"
cp tests/physics0/learner/pd0b_run.sh tests/physics0/learner/pd0b_freeze.sh "$T/tests/physics0/learner/"
for d in learner ladder score controls; do echo x > "$T/src/physics0/$d/f.c"; done; echo x > "$T/tests/physics0/verify/f.c"
cat > "$T/harness" <<'EOS'
#!/bin/sh
# fake harness: world - level seed outdir. FAKE=nolaw: REPLICATED but no law record; FAKE=reject: law file but verifier rejected
L=$3; S=$4; O=$5; [ "$L" = null ] || L=L$L; [ "$L" = nullL ] && L=null
st=REPLICATED; lrc=0
[ "${FAKE:-}" != nolaw ] || lrc=-1
[ "${FAKE:-}" != reject ] || lrc=3
if [ "$lrc" != -1 ]; then echo "law-bytes-$L-$S" > "$O/pd0l-$L-s$S.law"; fi
echo ledger > "$O/pd0l-$L-s$S.ledger"
echo "PD0L level=$L seed=$S state=$st code=0 law=$lrc score=PASS score_code=0"
echo "PD0F level=$L seed=$S final=00 final_status=0 score_before_final=0 shape_before_final=0 play_after_final=0 dup_final=0 audit_ok=1 mode=SEALED s_star=2 n_vars=2 n_latent=0 n_channels=1 valid=VALID"
EOS
chmod +x "$T/harness"; printf '#!/bin/sh\nexit 0\n' > "$T/world"; chmod +x "$T/world"
for s in 1 2 3 4 5; do echo "0 $s"; done > "$T/seeds.txt"
( cd "$T" && git init -q && git add -A && git -c user.email=t@t -c user.name=t commit -q -m t )
run() { ( cd "$T" && env PD0B_LEDGER_ARCHIVE="$T/archive" "$@" sh tests/physics0/learner/pd0b_run.sh ./harness ./world seeds.txt note ) > "$T/out.txt" 2>&1; }
# good run
run FAKE=none; rc=$?; R=$(ls "$T"/evidence/physics0/pd0b/PD0B_RUN-*.txt | head -1)
check "[ -f '$R' ]"; check "grep -q '^run_validity: VALID' '$R'"
check "grep -q '^validity_rule:.*law=1' '$R'"
D="${R%.txt}-laws"; check "[ -d '$D' ]"
check "[ -f '$D/pd0l-L0-s1.law' ] && [ ! -e '$D/pd0l-L0-s1.ledger' ]"
A="$T/archive/$(basename "${R%.txt}")-ledgers"; check "[ -f '$A/pd0l-L0-s1.ledger' ]"
nled=$(ls "$A" | wc -l); nman=$(wc -l < "$D/LEDGERS.sha256")
check "[ $nled -eq 5 ] && [ $nman -eq 5 ]"
check "(cd '$A' && sha256sum -c '$D/LEDGERS.sha256' >/dev/null)"
check "grep -q '^ledger_archive: $A\$' '$R'"
msha=$(sha256sum "$D/LEDGERS.sha256" | cut -d' ' -f1)
check "grep -q '^ledgers_manifest_sha256: $msha' '$R'"
sha=$(sha256sum "$D/pd0l-L0-s1.law" | cut -d' ' -f1)
check "grep -q '^level=L0 seed=1 state=REPLICATED code=0 score=PASS score_code=0 law=1 law_sha256=$sha\$' '$R'"
check "[ \$(grep -c ' law=1 law_sha256=' '$R') -eq 5 ]"
# the laws directory is never overwritten, nor the receipt
run FAKE=none && { echo "FAIL: second run succeeded"; fail=1; } || true
check "grep -q 'refusing to overwrite' '$T/out.txt'"
rm -rf "$T/evidence" "$T/archive"
# REPLICATED without a law record: INVALID, law=0, law_sha256=-
run FAKE=nolaw && { echo "FAIL: nolaw run succeeded"; fail=1; } || true
R=$(ls "$T"/evidence/physics0/pd0b/PD0B_RUN-*.txt | head -1)
check "grep -q '^run_validity: INVALID' '$R'"; check "grep -q 'seed=1 state=REPLICATED code=0 score=PASS score_code=0 law=0 law_sha256=-\$' '$R'"
rm -rf "$T/evidence" "$T/archive"
# law bytes present but the verifier rejected them: law=0, INVALID
run FAKE=reject && { echo "FAIL: reject run succeeded"; fail=1; } || true
R=$(ls "$T"/evidence/physics0/pd0b/PD0B_RUN-*.txt | head -1)
check "grep -q '^run_validity: INVALID' '$R'"; check "grep -q 'seed=2 state=REPLICATED.* law=0 law_sha256=-\$' '$R'"
[ "$fail" = 0 ] && echo "PD0B_RUN_TEST: PASS" || { echo "PD0B_RUN_TEST: FAIL"; exit 1; }
