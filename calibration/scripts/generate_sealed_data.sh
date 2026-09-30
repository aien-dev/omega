#!/usr/bin/env bash
# Turing calibration (CAL-0 / EXP-001): generate the sealed test data AFTER the
# candidate freeze. calibration/docs/BLINDING_PROTOCOL.md is the full procedure.
#
# usage:
#   generate_sealed_data.sh --commit FREEZE_SHA --profile-digest SHA256 [--n N] [--keep-learning] [--no-fetch]
#   generate_sealed_data.sh --derive-only --commit FREEZE_SHA --profile-digest SHA256 --n N
#   generate_sealed_data.sh --trial-seed S [--commit REF]      (feasibility only; writes to the trial dir)
#
# SEED RULE (turing.cal.sealed.v1). For group g in {1,2} and index j in 0..N-1:
#   msg  = the ASCII bytes  turing.cal.sealed.v1|<commit>|<digest>|g<g>|<j>
#          (<commit> = 40 lowercase hex, <digest> = 64 lowercase hex, <j> decimal,
#           no trailing newline)
#   h    = SHA-256(msg), lowercase hex
#   seed = the first 16 hex chars of h read as a big-endian 64-bit integer with the
#          top bit cleared (so 0 <= seed < 2^63), printed in decimal.
#   A reader can recompute any seed with:
#     printf '%s' 'turing.cal.sealed.v1|<commit>|<digest>|g1|0' | sha256sum
# Group 1 is the primary sealed test set; group 2 is the second-seed replication.
# A seed equal to a burned development seed (0..10 or 20260927) or repeated is
# refused (probability ~2^-59; the script stops rather than skip).
#
# GENERATOR (pinned): crumbs experiment --learner L --seed S --out DIR
#   crumbs = ~/workspace/hive-worktrees/crumbs-v1/target/release/crumbs, SHA-256 CRUMBS_SHA256 below
#   L      = crumbline-learner built by `make crumbline-learner` from a clean
#            `git archive` export of the freeze commit, SHA-256 must equal
#            LEARNER_SHA256 below (the learner is part of the generator: a
#            different learner build changes trace.ctr).
#   Each seed runs inside a bubblewrap jail: no network, generator + learner
#   read-only, only that seed's group directory writable.
#
# REFUSES when: the freeze commit is not an ancestor of origin/main; the commit
# lacks calibration/experiments/EXP-001/candidate_manifest.json or the profile;
# SHA-256 of the profile bytes at the commit differs from --profile-digest (or its
# .sha256 sidecar); the crumbs or learner binary hash differs from the pin;
# the candidate manifest at the commit is not "status": "frozen" or the profile there still holds FILL_AT_FREEZE;
# sealed/<commit>/ already exists; N is missing or disagrees with the profile; a retry differs from the first
# attempt; three attempts already failed.
#
# FAILED ATTEMPTS (FAILURE_REPORTING.md section 2). A generation failure is an infrastructure failure: no score
# exists yet. The partial directory is renamed sealed/<commit>.failed-<k> (k = 1, 2, 3) with a FAILED note
# (reason, exact command, time); it is kept and published with the results, never used. A retry must be the
# byte-identical command. After the third failed attempt the script writes sealed/<commit>.INCONCLUSIVE_INFRA and
# refuses every further attempt: EXP-001 then ends INCONCLUSIVE with reason INFRA.
#
# OUTPUT ~/aien-data/turing-cal/sealed/<commit>/
#   group-<g>/seed-<S>/control/{trace.ctr,samples.cts,evaluations.jsonl,ledger.jsonl,promotion.json}
#   group-<g>/seed-<S>/{report.json,control-outcomes.json}
#   seed_commitment.json  seeds, rule, inputs, generator + learner hashes
#   manifest.json         every file: SHA-256, bytes, records, volatile flag
#   generation.log        per-seed wall time and exit code
#   COMPLETE              written last; its absence means the set is unusable
# The learning condition runs too (the generator always runs both, control first
# and independently); its files are hashed into manifest.json and then deleted
# unless --keep-learning. Directories end mode 0500 and files 0400.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tc_jail_lib.sh
. "$here/tc_jail_lib.sh"

CRUMBS="${TC_CRUMBS:-$HOME/workspace/hive-worktrees/crumbs-v1/target/release/crumbs}"
CRUMBS_SHA256=72e396b15532aa93afb1f215521e04dca8470f779f3de35c10b04b07bff96638
# crumbline-learner built from omega 1b75aa8 (reproduces final-20260927 byte-exactly).
LEARNER_SHA256="${TC_LEARNER_SHA256:-8159bdff248efa67fa2cf75bb0b506bc359f4a2269139d03479753c0fc393416}"
TRIAL_ROOT="$HOME/aien-data/turing-cal/trial"
DOMAIN="turing.cal.sealed.v1"
PROFILE_PATH=calibration/profiles/Turing-profile-v1.0.toml
SIDECAR_PATH=calibration/profiles/Turing-profile-v1.0.sha256
CANDIDATE_MANIFEST=calibration/experiments/EXP-001/candidate_manifest.json
PROFILE_N_KEY=sealed_seeds_per_group

die() { echo "generate_sealed_data: $*" >&2; exit 2; }
sha() { sha256sum -- "$1" | cut -d' ' -f1; }

commit="" digest="" n="" derive_only=0 trial_seed="" keep_learning=0 fetch=1
orig_cmd="generate_sealed_data.sh $*"
repo="$(git -C "$here" rev-parse --show-toplevel)"
while [ $# -gt 0 ]; do
    case "$1" in
    --commit) commit="$2"; shift 2 ;;
    --profile-digest) digest="$2"; shift 2 ;;
    --n) n="$2"; shift 2 ;;
    --derive-only) derive_only=1; shift ;;
    --trial-seed) trial_seed="$2"; shift 2 ;;
    --keep-learning) keep_learning=1; shift ;;
    --no-fetch) fetch=0; shift ;;
    --repo) repo="$2"; shift 2 ;;
    *) die "unknown option '$1'" ;;
    esac
done

derive_seed() { # commit digest group index
    local h top
    h="$(printf '%s' "$DOMAIN|$1|$2|g$3|$4" | sha256sum | cut -c1-16)"
    top=$((16#${h:0:1} & 7))
    printf '%d\n' "$((16#$(printf '%x' "$top")${h:1}))"
}

is_burned() { [ "$1" -le 10 ] || [ "$1" = 20260927 ]; }

# Build the learner from a clean export of REF into $1; echo its path.
build_learner() {
    local ref="$1" work="$2"
    mkdir -p "$work/src" "$work/build" "$work/no-physics"
    git -C "$repo" archive "$ref" | tar -x -C "$work/src"
    make -s -C "$work/src" crumbline-learner OUT_DIR="$work/build" PHYSICS_DIR="$work/no-physics" \
        >"$work/build.log" 2>&1 || { tail -20 "$work/build.log" >&2; die "learner build failed"; }
    echo "$work/build/crumbline-learner"
}

check_binaries() { # learner
    [ -x "$CRUMBS" ] || die "generator binary missing: $CRUMBS"
    [ "$(sha "$CRUMBS")" = "$CRUMBS_SHA256" ] || die "generator SHA-256 $(sha "$CRUMBS") != pinned $CRUMBS_SHA256"
    [ "$(sha "$1")" = "$LEARNER_SHA256" ] ||
        die "learner SHA-256 $(sha "$1") != pinned $LEARNER_SHA256 (set TC_LEARNER_SHA256 only with a recorded reason)"
}

# Run one seed in the generation jail. Args: learner seed groupdir. Prints wall ns.
run_seed() {
    local learner="$1" seed="$2" gdir="$3" out t0 t1 rc
    out="$gdir/seed-$seed"
    [ -e "$out" ] && die "$out already exists (the generator appends; refusing)"
    tc_base_args
    TC_ARGS+=(--ro-bind "$(tc_real "$CRUMBS")" /opt/tc/crumbs --ro-bind "$(tc_real "$learner")" /opt/tc/crumbline-learner)
    tc_rw "$gdir"
    t0=$(date +%s%N)
    set +e
    "$TC_BWRAP" "${TC_ARGS[@]}" -- /opt/tc/crumbs experiment --learner /opt/tc/crumbline-learner \
        --seed "$seed" --out "$(tc_real "$out")" >"$gdir/seed-$seed.stdout" 2>&1
    rc=$?
    set -e
    t1=$(date +%s%N)
    echo "$((t1 - t0)) $rc"
}

records_of() { # file -> record count (or -1 when the size is not a whole number of records)
    local f="$1" b
    b=$(stat -c %s "$f")
    case "$f" in
    *.ctr) [ $((b % 247)) = 0 ] && echo $((b / 247)) || echo -1 ;;
    *.cts) [ $((b % 268)) = 0 ] && echo $((b / 268)) || echo -1 ;;
    *.jsonl) wc -l <"$f" | tr -d ' ' ;;
    *) echo null ;;
    esac
}

volatile_of() { # files whose bytes carry uuid-v7 ids or wall-clock times
    case "$1" in
    */ledger.jsonl | */report.json | *-outcomes.json | *.stdout) echo true ;;
    *) echo false ;;
    esac
}

# ---------------------------------------------------------------- trial mode
if [ -n "$trial_seed" ]; then
    [[ "$trial_seed" =~ ^[0-9]+$ ]] || die "--trial-seed must be decimal"
    is_burned "$trial_seed" && die "trial seed is a burned development seed"
    tc_have_bwrap
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    learner="$(build_learner "${commit:-HEAD}" "$work")"
    check_binaries "$learner"
    gdir="$TRIAL_ROOT/jail-trial"
    mkdir -p "$gdir"
    [ -e "$gdir/seed-$trial_seed" ] && die "$gdir/seed-$trial_seed already exists"
    ns="" rc=""
    read -r ns rc < <(run_seed "$learner" "$trial_seed" "$gdir") || true
    [ -n "$rc" ] || die "generation jail did not start"
    echo "trial seed $trial_seed rc=$rc wall_ms=$((ns / 1000000)) learner=$(sha "$learner")"
    echo "control/trace.ctr $(sha "$gdir/seed-$trial_seed/control/trace.ctr")"
    exit "$rc"
fi

# ------------------------------------------------------------- common checks
[[ "$commit" =~ ^[0-9a-f]{40}$ ]] || die "--commit must be a full 40-hex commit id"
[[ "$digest" =~ ^[0-9a-f]{64}$ ]] || die "--profile-digest must be 64 lowercase hex"

if [ "$derive_only" = 1 ]; then
    [[ "$n" =~ ^[1-9][0-9]*$ ]] || die "--derive-only needs --n"
    for g in 1 2; do
        for ((j = 0; j < n; j++)); do echo "g$g $j $(derive_seed "$commit" "$digest" "$g" "$j")"; done
    done
    exit 0
fi

[ "$fetch" = 1 ] && git -C "$repo" fetch -q origin main
git -C "$repo" cat-file -e "$commit^{commit}" 2>/dev/null || die "commit $commit not found"
git -C "$repo" merge-base --is-ancestor "$commit" origin/main || die "commit $commit is not an ancestor of origin/main"
git -C "$repo" cat-file -e "$commit:$CANDIDATE_MANIFEST" 2>/dev/null || die "$CANDIDATE_MANIFEST missing at $commit"
git -C "$repo" cat-file -e "$commit:$PROFILE_PATH" 2>/dev/null || die "$PROFILE_PATH missing at $commit"
pd="$(git -C "$repo" show "$commit:$PROFILE_PATH" | sha256sum | cut -d' ' -f1)"
[ "$pd" = "$digest" ] || die "profile bytes at $commit hash to $pd, not --profile-digest $digest"
git -C "$repo" show "$commit:$CANDIDATE_MANIFEST" | grep -q "^  \"status\": \"frozen\"," ||
    die "$CANDIDATE_MANIFEST at $commit is not frozen (the commit must be the freeze commit C_f)"
git -C "$repo" show "$commit:$PROFILE_PATH" | grep -q FILL_AT_FREEZE && die "profile at $commit still holds FILL_AT_FREEZE"
ctime_utc="$(date -u -d "@$(git -C "$repo" show -s --format=%ct "$commit")" +%Y-%m-%dT%H:%M:%SZ)"
if git -C "$repo" cat-file -e "$commit:$SIDECAR_PATH" 2>/dev/null; then
    sd="$(git -C "$repo" show "$commit:$SIDECAR_PATH" | awk '{print $1; exit}')"
    [ "$sd" = "$digest" ] || die "sidecar digest $sd != --profile-digest $digest"
else
    die "$SIDECAR_PATH missing at $commit"
fi
pn="$(git -C "$repo" show "$commit:$PROFILE_PATH" | sed -n "s/^[[:space:]]*$PROFILE_N_KEY[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p" | head -1)"
if [ -n "$pn" ] && [ -n "$n" ] && [ "$pn" != "$n" ]; then die "--n $n disagrees with profile $PROFILE_N_KEY = $pn"; fi
n="${n:-$pn}"
[[ "$n" =~ ^[1-9][0-9]*$ ]] || die "N unknown: profile has no '$PROFILE_N_KEY = <int>' and no --n given"

dest="$TC_SEALED_ROOT/$commit"
[ -e "$dest.INCONCLUSIVE_INFRA" ] && die "three generation attempts already failed for $commit: EXP-001 is INCONCLUSIVE (INFRA)"
attempt=1
while [ -e "$dest.failed-$attempt" ]; do
    first_cmd="$(sed -n "s/^command: //p" "$dest.failed-1/FAILED")"
    [ "$first_cmd" = "$orig_cmd" ] || die "retry differs from attempt 1 ($first_cmd); a retry must be byte-identical"
    attempt=$((attempt + 1))
done
[ "$attempt" -le 3 ] || die "attempt $attempt: at most three attempts"
[ -e "$dest" ] && die "$dest already exists (sealed data is generated exactly once per freeze commit)"
tc_have_bwrap

# Derive and check all seeds before generating anything.
declare -A seen=()
seeds=()
for g in 1 2; do
    for ((j = 0; j < n; j++)); do
        s="$(derive_seed "$commit" "$digest" "$g" "$j")"
        is_burned "$s" && die "derived seed g$g/$j = $s is a burned development seed"
        [ -n "${seen[$s]:-}" ] && die "derived seed $s repeats"
        seen[$s]=1
        seeds+=("$g $j $s")
    done
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
learner="$(build_learner "$commit" "$work")"
check_binaries "$learner"

# A failed attempt: keep the partial set under a numbered name with a FAILED note, then stop.
fail_attempt() {
    local k="$attempt" fd="$dest.failed-$attempt"
    mv "$dest" "$fd"
    printf 'reason: %s\ncommand: %s\nattempt: %s\ntime_utc: %s\n' "$1" "$orig_cmd" "$k" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$fd/FAILED"
    if [ "$k" -ge 3 ]; then
        printf 'EXP-001 INCONCLUSIVE reason INFRA: three sealed generation attempts failed (%s.failed-1..3)\n' "$dest" >"$dest.INCONCLUSIVE_INFRA"
        die "attempt $k failed ($1); three attempts failed: EXP-001 is INCONCLUSIVE (INFRA); publish all FAILED notes"
    fi
    die "attempt $k failed ($1); kept as $fd; retry with the byte-identical command (at most 3 attempts)"
}
umask 077
mkdir -p "$TC_SEALED_ROOT"
chmod 0700 "$TC_SEALED_ROOT"
mkdir "$dest"
log="$dest/generation.log"
started="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "started $started commit $commit committed_utc $ctime_utc attempt $attempt profile_digest $digest n_per_group $n" >"$log"
for row in "${seeds[@]}"; do
    read -r g j s <<<"$row"
    gdir="$dest/group-$g"
    mkdir -p "$gdir"
    [ -e "$gdir/seed-$s" ] && fail_attempt "$gdir/seed-$s already exists (the generator appends; refusing)"
    ns="" rc=""
    read -r ns rc < <(run_seed "$learner" "$s" "$gdir") || true
    [ -n "$rc" ] || fail_attempt "generation jail did not start for seed $s"
    echo "g$g j=$j seed=$s rc=$rc wall_ms=$((ns / 1000000))" >>"$log"
    [ "$rc" = 0 ] || fail_attempt "generator failed for seed $s (rc $rc)"
done

# Manifest: every file, learning included, hashed before learning is dropped.
seedlist_sha="$(printf '%s\n' "${seeds[@]}" | sha256sum | cut -d' ' -f1)"
{
    echo "{"
    echo "  \"schema\": \"turing.cal.sealed_manifest.v1\","
    echo "  \"freeze_commit\": \"$commit\","
    echo "  \"profile_digest\": \"$digest\","
    echo "  \"files\": ["
    first=1
    while IFS= read -r f; do
        rel="${f#"$dest"/}"
        [ "$rel" = generation.log ] && continue
        kept=true
        case "$rel" in */learning/* | */learning-outcomes.json) [ "$keep_learning" = 1 ] || kept=false ;; esac
        [ "$first" = 1 ] || echo ","
        first=0
        printf '    {"path": "%s", "sha256": "%s", "bytes": %s, "records": %s, "volatile": %s, "kept": %s}' \
            "$rel" "$(sha "$f")" "$(stat -c %s "$f")" "$(records_of "$f")" "$(volatile_of "$f")" "$kept"
    done < <(find "$dest" -type f | LC_ALL=C sort)
    echo
    echo "  ]"
    echo "}"
} >"$work/manifest.json"
if [ "$keep_learning" = 0 ]; then
    find "$dest" -mindepth 3 -maxdepth 3 -type d -name learning -exec rm -rf {} +
    find "$dest" -mindepth 3 -maxdepth 3 -type f -name learning-outcomes.json -delete
fi
mv "$work/manifest.json" "$dest/manifest.json"

{
    echo "{"
    echo "  \"schema\": \"turing.cal.seed_commitment.v1\","
    echo "  \"rule\": \"seed = (first 16 hex of SHA-256(ASCII '$DOMAIN|<commit>|<profile_digest>|g<group>|<index>')) with top bit cleared, decimal\","
    echo "  \"domain\": \"$DOMAIN\","
    echo "  \"freeze_commit_time_utc\": \"$ctime_utc\","
    echo "  \"attempt\": $attempt,"
    echo "  \"freeze_commit\": \"$commit\","
    echo "  \"profile_digest\": \"$digest\","
    echo "  \"n_per_group\": $n,"
    echo "  \"groups\": {\"1\": \"primary sealed test\", \"2\": \"second-seed replication\"},"
    echo "  \"seeds\": ["
    i=0
    for row in "${seeds[@]}"; do
        read -r g j s <<<"$row"
        [ "$i" = 0 ] || echo ","
        i=1
        printf '    {"group": %s, "index": %s, "seed": %s}' "$g" "$j" "$s"
    done
    echo
    echo "  ],"
    echo "  \"seed_list_sha256\": \"$seedlist_sha\","
    echo "  \"seed_list_format\": \"one line per seed: '<g> <index> <seed>' in group then index order, LF-terminated\","
    echo "  \"generator\": {\"path\": \"$CRUMBS\", \"sha256\": \"$CRUMBS_SHA256\","
    echo "    \"command\": \"crumbs experiment --learner <learner> --seed <seed> --out <dir>\"},"
    echo "  \"learner\": {\"sha256\": \"$(sha "$learner")\", \"build\": \"git archive $commit | make crumbline-learner\"},"
    echo "  \"learning_condition_kept\": $([ "$keep_learning" = 1 ] && echo true || echo false),"
    echo "  \"started_utc\": \"$started\","
    echo "  \"finished_utc\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
    echo "  \"manifest_sha256\": \"$(sha "$dest/manifest.json")\""
    echo "}"
} >"$dest/seed_commitment.json"

# Hard links would let a path outside the sealed root reach sealed bytes.
bad="$(find "$dest" -type f -links +1 | head -1)"
[ -z "$bad" ] || die "sealed file has extra hard links: $bad"
echo "complete $(date -u +%Y-%m-%dT%H:%M:%SZ) seed_commitment_sha256 $(sha "$dest/seed_commitment.json")" >>"$log"
sha "$dest/seed_commitment.json" >"$dest/COMPLETE"
find "$dest" -type f -exec chmod 0400 {} +
find "$dest" -type d -exec chmod 0500 {} +
echo "sealed data ready: $dest"
echo "seed_commitment.json sha256 $(cat "$dest/COMPLETE")"
