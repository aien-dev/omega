#!/usr/bin/env bash
# EXP-001 dataset manifest (turing.cal.dataset_manifest.v1): the evaluator's
# only description of the data it scores. See calibration/docs/EVALUATOR.md.
#
#   make_dataset_manifest.sh --sealed SEALED_ROOT OUT.json
#       SEALED_ROOT = <sealed root>/<freeze commit> written by generate_sealed_data.sh
#       (needs COMPLETE, seed_commitment.json, manifest.json). Every control trace is
#       re-hashed and must equal the sealed manifest entry.
#   make_dataset_manifest.sh --dev RUN_DIR "G1 SEEDS" "G2 SEEDS" PROFILE_SHA256 OUT.json
#       Development data only (dry run): RUN_DIR/seed-<S>/control/trace.ctr.
#       split = development, released_utc = NOT_SEALED. Never evidence.
#
# One file entry per line (the evaluator parses line by line). payload_digest =
# SHA-256 over the LF-terminated lines "<g> <index> <seed> <sha256> <bytes>" in
# group then index order.
set -euo pipefail
die() { echo "make_dataset_manifest: $*" >&2; exit 2; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
jstr() { grep -m1 "\"$1\":" "$2" | sed -E 's/.*"'"$1"'": *"([^"]*)".*/\1/'; }

mode="${1:-}"
case "$mode" in
--sealed)
    [ $# = 3 ] || die "usage: --sealed SEALED_ROOT OUT.json"
    root="${2%/}" out="$3"
    [ -f "$root/COMPLETE" ] || die "$root/COMPLETE missing (generation incomplete)"
    sc="$root/seed_commitment.json" sm="$root/manifest.json"
    [ "$(sha "$sc")" = "$(tr -d ' \n' <"$root/COMPLETE")" ] || die "COMPLETE does not match seed_commitment.json"
    [ "$(sha "$sm")" = "$(jstr manifest_sha256 "$sc")" ] || die "manifest.json does not match seed_commitment.json"
    commit="$(jstr freeze_commit "$sc")" profile="$(jstr profile_digest "$sc")"
    released="$(jstr started_utc "$sc")" finished="$(jstr finished_utc "$sc")"
    gen="$(grep -m1 '"generator"' "$sc" | sed -E 's/.*"sha256": *"([0-9a-f]{64})".*/\1/')"
    lrn="$(grep -m1 '"learner"' "$sc" | sed -E 's/.*"sha256": *"([0-9a-f]{64})".*/\1/')"
    split=sealed_test dsid="EXP-001-sealed-$commit" scsha="$(sha "$sc")" smsha="$(sha "$sm")"
    rows=()
    while IFS= read -r l; do
        g="$(sed -E 's/.*"group": *([0-9]+).*/\1/' <<<"$l")"
        j="$(sed -E 's/.*"index": *([0-9]+).*/\1/' <<<"$l")"
        s="$(sed -E 's/.*"seed": *([0-9]+).*/\1/' <<<"$l")"
        rel="group-$g/seed-$s/control/trace.ctr"
        f="$root/$rel"
        [ -f "$f" ] || die "missing $rel"
        h="$(sha "$f")" b="$(stat -c %s "$f")"
        grep -q "\"path\": \"$rel\", \"sha256\": \"$h\", \"bytes\": $b," "$sm" || die "$rel differs from the sealed manifest"
        rows+=("$g $j $s $h $b $f")
    done < <(grep '"group":.*"index":.*"seed":' "$sc")
    ;;
--dev)
    [ $# = 6 ] || die "usage: --dev RUN_DIR \"G1 SEEDS\" \"G2 SEEDS\" PROFILE_SHA256 OUT.json"
    run="${2%/}" profile="$5" out="$6"
    [[ "$profile" =~ ^[0-9a-f]{64}$ ]] || die "bad profile sha256"
    commit=NONE released=NOT_SEALED finished=NOT_SEALED gen=NOT_RECORDED lrn=NOT_RECORDED
    split=development dsid="EXP-001-dev-$(basename "$run")" scsha=NONE smsha=NONE
    rows=()
    for g in 1 2; do
        j=0
        seeds="$3"; [ "$g" = 2 ] && seeds="$4"
        for s in $seeds; do
            f="$run/seed-$s/control/trace.ctr"
            [ -f "$f" ] || die "missing $f"
            rows+=("$g $j $s $(sha "$f") $(stat -c %s "$f") $f")
            j=$((j + 1))
        done
    done
    ;;
*) die "usage: --sealed SEALED_ROOT OUT.json | --dev RUN_DIR \"G1 SEEDS\" \"G2 SEEDS\" PROFILE_SHA256 OUT.json" ;;
esac

[ "${#rows[@]}" -gt 0 ] || die "no files"
payload="$(for r in "${rows[@]}"; do read -r g j s h b _ <<<"$r"; echo "$g $j $s $h $b"; done | sha256sum | cut -d' ' -f1)"
{
    echo "{"
    echo "  \"schema\": \"turing.cal.dataset_manifest.v1\","
    echo "  \"dataset_id\": \"$dsid\","
    echo "  \"experiment\": \"EXP-001\","
    echo "  \"split\": \"$split\","
    echo "  \"profile_digest\": \"$profile\","
    echo "  \"freeze_commit\": \"$commit\","
    echo "  \"generator_digest\": \"$gen\","
    echo "  \"learner_digest\": \"$lrn\","
    echo "  \"parameter_commitment\": \"crumbs experiment --learner L --seed S --out DIR, control arm\","
    echo "  \"seed_commitment_sha256\": \"$scsha\","
    echo "  \"sealed_manifest_sha256\": \"$smsha\","
    echo "  \"released_utc\": \"$released\","
    echo "  \"generation_finished_utc\": \"$finished\","
    echo "  \"trajectory_count\": ${#rows[@]},"
    echo "  \"payload_digest\": \"$payload\","
    echo "  \"files\": ["
    n=0
    for r in "${rows[@]}"; do
        read -r g j s h b f <<<"$r"
        [ "$n" = 0 ] || echo ","
        printf '    {"group": %s, "index": %s, "seed": %s, "sha256": "%s", "bytes": %s, "path": "%s"}' "$g" "$j" "$s" "$h" "$b" "$f"
        n=$((n + 1))
    done
    echo
    echo "  ]"
    echo "}"
} >"$out"
echo "dataset manifest: $out ($split, ${#rows[@]} files, payload $payload)"
