#!/usr/bin/env bash
# tools/gen/runner/run.sh -- generation driver of the GEN compositional-generalization experiment (omega#477,
# GEN-2; master #475). Formats: tools/gen/runner/FORMATS.md (run row v1, prompt manifest v1). Shell + curl +
# jq + the repo's C tools; no Python. Reuses tools/exp445/reader.sh by sourcing it (request building, the
# omegese reader chain, transport `ask`, gen_row, G0 derivation) and tools/exp445/sampling-gate.sh by calling it.
#
#   run.sh --tasks <public tasks dir> --out <run dir>
#
# <public tasks dir> holds ONLY tasks/cases/<id>/{requirement,prompt} (ola.sh tasks-public output): any other
# file (hidden/, inputs, expected, references, grader output) refuses the run before anything is sent.
# Settings come from tools/gen/runner/PARAMS (GEN_PARAMS names another file only with GEN_TEST=1, tests only;
# a non-test run refuses a draft revision or any PLACEHOLDER value). The model pin is configuration
# <config> of the EXP445 PARAMS (<exp445_params>): model id, endpoint, request extra, server, weight list.
# Order of work:
#   1 checks: clean source tree (TOOL_PATHS of reader.sh; GEN_TEST=1 allows dirty and records -dirty),
#     mask=off, public-only tasks, weights.check + server.json in <out> (tools/exp445/check_weights.sh),
#     provider = genret-cli-v2 (a stub only in test mode), corpus manifest sha256 = the pinned one = records/SET.sha256
#   2 prompt manifests for A, B, C, D x every task (content-addressed, FORMATS.md section 3): system =
#     exp445 arms/common.md + arms/omegese.md + worked examples (A: examples_one, B C D: examples_many) as the
#     fixed prefix, then the retrieved suffix (genret suffix.txt; C --kind raw, D --kind abstraction; none for
#     A, B). The query is the public requirement + prompt files only.
#   3 meta.txt: every pin and digest. A resume rebuilds 1-2 and refuses unless meta.txt is byte-identical
#     (changed manifest, model pin, prompt, protocol, source, params, tasks, provider or corpus, dirty tree).
#   4 greedy gate (gate/greedy): the first greedy_gate_tasks dev tasks in condition B, temperature 0, each sent
#     twice: PASS needs byte-identical content and mode=greedy in every reply. No PASS: no primary, exit 1.
#   5 attempt 1 (greedy, the primary), order task then condition: round 1, the omegese reader + public check
#     (x445 check), one repair round when refused (secondary). cands/<c>-1 = round-1 candidates,
#     cands/<c>-1-final = final candidates.
#   6 G0 (gate/g0) before attempt 2: the EXP445 G0 rule on g0_task in condition B (sampling-gate.sh at its
#     pinned sha256 + one repeat of the first seed; reader.sh g0_derive / g0_pass). FAIL: attempts 2..N are
#     not run and pass@3 is UNAVAILABLE (gate/g0/verdict.txt says so); exit 0.
#   7 attempts 2..N sampled (temperature, top_p, seed = reader.sh seed_of(seed_base, attempt, task index)).
# Transport retries (reader.sh ask: retries + 1 tries, each in rN.transport) are never new samples; a failed
# round is final (REQUEST FAILED, no candidate). A committed round is never requested again; rerunning on a
# finished run dir sends nothing. Writes SHA256SUMS over every file of the run dir.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
E445="$ROOT/tools/exp445"
die() { echo "gen run.sh: $*" >&2; exit 2; }
TEST="${GEN_TEST:-0}"
PARAMS="$HERE/PARAMS"
if [ "$TEST" = 1 ]; then PARAMS="${GEN_PARAMS:-$PARAMS}"; else unset GEN_PARAMS GEN_TEST_KILL; fi
[ -f "$PARAMS" ] || die "no PARAMS file $PARAMS"
kv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$PARAMS"; }
pv() { kv "$1" | sed 's/^PLACEHOLDER *//'; }
rp() { case "$1" in /*|"") echo "$1" ;; *) echo "$ROOT/$1" ;; esac; }
sha() { sha256sum < "$1" | cut -d' ' -f1; }
shas() { printf '%s' "$1" | sha256sum | cut -d' ' -f1; }
command -v jq > /dev/null || die "jq is required"
command -v curl > /dev/null || die "curl is required"
. "$E445/reader.sh"

TASKS="" OUT="" CONTROL=none
while [ $# -gt 0 ]; do
  case "$1" in
    --tasks) TASKS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --control) CONTROL="$2"; shift 2 ;;
    *) die "unknown argument $1 (usage: run.sh --tasks DIR --out DIR [--control cross-family])" ;;
  esac
done
[ -n "$TASKS" ] && [ -n "$OUT" ] || die "usage: run.sh --tasks DIR --out DIR [--control cross-family]"

# ---- 1. checks ------------------------------------------------------------------------------------------
if [ "$TEST" != 1 ]; then
  case "$(kv revision)" in *draft*|"") die "PARAMS revision '$(kv revision)' is a draft: a real run waits for GEN_PROTOCOL_V1" ;; esac
  ! grep -v '^#' "$PARAMS" | grep -q PLACEHOLDER || die "PARAMS still holds PLACEHOLDER values ($(grep -v '^#' "$PARAMS" | grep -c PLACEHOLDER)): a real run waits for the frozen protocol, corpus and provider"
fi
DIRTY="$(git -C "$ROOT" status --porcelain -- "${TOOL_PATHS[@]}")"
if [ -n "$DIRTY" ] && [ "$TEST" != 1 ]; then die "dirty source tree ($(head -n 1 <<< "$DIRTY")): commit first; a run is tied to one clean commit"; fi
COMMIT="$(git -C "$ROOT" rev-parse HEAD)$([ -z "$DIRTY" ] || echo -dirty)"
[ "$(kv mask)" = off ] || die "mask must be off (the Omegese mask is not qualified; plan decision 1)"
EP="$(rp "$(kv exp445_params)")" CONFIG="$(kv config)"
[ -f "$EP" ] || die "no EXP445 PARAMS $EP"
ekv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$EP"; }
[ -z "$(ekv "config.$CONFIG.unavailable")" ] || die "config $CONFIG is unavailable: $(ekv "config.$CONFIG.unavailable")"
MODEL="$(ekv "config.$CONFIG.model")" BASE="$(ekv "config.$CONFIG.base_url")" EXTRA="$(ekv "config.$CONFIG.extra")" SERVER="$(ekv "config.$CONFIG.server")"
[ -n "$MODEL" ] && [ -n "$BASE" ] && [ -n "$EXTRA" ] && [ -n "$SERVER" ] || die "config $CONFIG is not named in $EP (model, base_url, extra, server)"
jq -e 'type == "object" and ([keys[] | select(IN("model", "messages", "max_tokens", "temperature", "top_p", "seed", "stream"))] | length == 0)' \
  <<< "$EXTRA" > /dev/null 2>&1 || die "config.$CONFIG.extra must be a JSON object that sets none of the fixed request fields"
EXTRA="$(jq -c . <<< "$EXTRA")"
[ "$EXTRA" = "{}" ] || jq -e 'tostring | test("mask|grammar|constrain"; "i") | not' <<< "$EXTRA" > /dev/null || die "the request extra names a mask or grammar field: masking is off in every condition"
case "$SERVER" in PLACEHOLDER*) [ "$TEST" = 1 ] || die "config $CONFIG server is a placeholder in $EP" ;; esac
WLIST="$(rp "$(ekv "config.$CONFIG.weights")")"
IFS=, read -r -a CONDS <<< "$(kv conditions)"
[ "$(kv conditions)" = A,B,C,D ] || die "conditions must be A,B,C,D (plan decision 4)"
ATT="$(kv attempts)" MAXT="$(kv max_tokens)" GTEMP="$(kv greedy_temperature)" GTOPP="$(kv greedy_top_p)" TEMP="$(kv temperature)" TOPP="$(kv top_p)"
SB="$(kv seed_base)" TIMEOUT="$(kv timeout_s)" RETRIES="$(kv retries)"
[[ "$ATT" =~ ^[1-9]$ ]] || die "attempts 1..9"
[[ "$MAXT" =~ ^[1-9][0-9]*$ ]] || die "max_tokens must be a positive integer"
[ "$GTEMP" = 0 ] || die "greedy_temperature must be 0 (attempt 1 is greedy)"
[[ "$RETRIES" =~ ^[0-9]$ ]] && [[ "$TIMEOUT" =~ ^[1-9][0-9]*$ ]] && [[ "$SB" =~ ^[0-9]+$ ]] || die "retries 0..9, timeout_s > 0, seed_base integer"
for v in "$TEMP" "$TOPP" "$GTOPP"; do [[ "$v" =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "temperature/top_p must be numbers"; done
[[ "${EXP445_TEST_BACKOFF_S:-5}" =~ ^[0-9]+$ ]] || die "EXP445_TEST_BACKOFF_S must be an integer"
[ -d "$TASKS/tasks/cases" ] || die "$TASKS/tasks/cases missing (use ola.sh tasks-public)"
bad="$(cd "$TASKS" && find . -mindepth 1 \( -type l -o \( -type f ! -regex '\./tasks/cases/[a-z0-9_-]+/\(requirement\|prompt\)' \) -o \( -type d ! -regex '\./tasks\(/cases\(/[a-z0-9_-]+\)?\)?' \) \) -print -quit)"
[ -z "$bad" ] || die "$TASKS holds $bad: the driver sees only tasks/cases/<id>/requirement and prompt (no hidden data, ever)"
mapfile -t IDS < <(ls "$TASKS/tasks/cases" | LC_ALL=C sort)
[ "${#IDS[@]}" -gt 0 ] || die "no tasks"
for t in "${IDS[@]}"; do [ -f "$TASKS/tasks/cases/$t/requirement" ] && [ -f "$TASKS/tasks/cases/$t/prompt" ] || die "task $t lacks requirement or prompt"; done
DEVD="$(rp "$(pv dev_tasks)")/tasks/cases"
[ -d "$DEVD" ] || die "no dev tasks $DEVD"
mapfile -t DEVIDS < <(ls "$DEVD" | LC_ALL=C sort)
NG="$(kv greedy_gate_tasks)" G0T="$(kv g0_task)" G0N="$(kv g0_seeds)"
[[ "$NG" =~ ^[1-9][0-9]*$ ]] && [ "${#DEVIDS[@]}" -ge "$NG" ] || die "greedy_gate_tasks $NG: only ${#DEVIDS[@]} dev tasks"
[[ "$G0N" =~ ^[1-9][0-9]?$ ]] && [ -f "$DEVD/$G0T/requirement" ] && [ -f "$DEVD/$G0T/prompt" ] || die "g0_task $G0T / g0_seeds $G0N"
GT=("${DEVIDS[@]:0:$NG}")
weights_ok "$OUT" "$WLIST" "$MODEL" ||
  die "$OUT/weights.check + server.json missing or not all OK: run EXP445_PARAMS=$EP tools/exp445/check_weights.sh $CONFIG $OUT first"
PROV="$(rp "$(pv provider)")"
[ -x "$PROV" ] || die "provider $PROV is not executable"
PVER="$("$PROV" version 2> /dev/null || true)"
[[ "$PVER" == genret-cli-v2* ]] || die "provider $PROV is not genret-cli-v2 (version: '$PVER')"
[[ "$PVER" != *stub* ]] || [ "$TEST" = 1 ] || die "provider $PROV is the test stub: a real run needs Agent E's genret"
PCS="$(kv provider_contract_sha256)"
if [ -f "$ROOT/tools/gen/retrieval/CONTRACT.md" ]; then [ "$(sha "$ROOT/tools/gen/retrieval/CONTRACT.md")" = "$PCS" ] || die "tools/gen/retrieval/CONTRACT.md is not the pinned genret contract ($PCS)"; fi
CM="$(rp "$(pv corpus_manifest)")" CMS="$(pv corpus_manifest_sha256)"
[ -f "$CM" ] && [ "$(sha "$CM")" = "$CMS" ] || die "corpus manifest $CM missing or not the pinned record set ($CMS)"
[ ! -f "$(dirname "$CM")/SET.sha256" ] || [ "$(tr -d '\n' < "$(dirname "$CM")/SET.sha256")" = "$CMS" ] || die "$(dirname "$CM")/SET.sha256 is not the pinned record set ($CMS)"
RCK="$(rp "$(pv recheck)")"
[ -x "$RCK" ] || die "recheck $RCK is not executable (make gen-abstract builds build/gen_abstract/recheck)"
RMODE="$(kv retrieval_mode)" K="$(kv k)" CAP="$(kv cap_bytes)"
[ "$K" = 3 ] && [ "$CAP" = 4096 ] && [ "$RMODE" = normal ] || [ "$TEST" = 1 ] || die "k=$K cap_bytes=$CAP retrieval_mode=$RMODE: the preregistered values are k=3 cap_bytes=4096 normal"
case "$CONTROL" in none) ;; cross-family)
  FS="$(rp "$(pv famsig)")" DM="$(rp "$(pv dev_meta)")"
  [ -x "$FS" ] && [ -f "$DM" ] || die "cross-family control needs famsig ($FS) and the dev META ($DM)"
  declare -A EXT
  for t in "${IDS[@]}"; do
    shp="$(awk -v t="$t" '$1 == t && $2 == "dev" && NF == 9 { print $9 }' "$DM")"
    [[ "$t" == dev* ]] && [ -n "$shp" ] || die "cross-family control runs on DEV tasks only: $t is not a dev task of $DM"
    EXT[$t]="$("$FS" token "$shp")" || die "famsig token $shp failed"
    [[ "${EXT[$t]}" =~ ^gs1-[0-9a-f]{16}$ ]] || die "famsig token of $t is '${EXT[$t]}', not gs1-<16 hex>"
  done ;;
  *) die "--control $CONTROL: only cross-family" ;; esac
[[ "$K" =~ ^[1-9][0-9]?$ ]] && [[ "$CAP" =~ ^[1-9][0-9]*$ ]] || die "k / cap_bytes"
case "$RMODE" in normal|irrelevant|empty) ;; *) die "retrieval_mode $RMODE (length-matched runs through a separate ablation revision)" ;; esac
GATE_SH="$E445/sampling-gate.sh" GSHA="$(ekv g0_gate_sha256)"
[ -n "$GSHA" ] && [ "$(sha "$GATE_SH")" = "$GSHA" ] || die "$GATE_SH is not the pinned sampling gate (EXP445 PARAMS g0_gate_sha256)"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit omegese >&2
X="$ROOT/build/omega_emit/x445" EMIT="$ROOT/build/omega_emit/omega_emit" G="$ROOT/build/omegese/omegese"
mkdir -p "$OUT"; OUT="$(realpath -e "$OUT")"
exec 9> "$OUT/.lock"; flock -n 9 || die "another run.sh holds $OUT"
W="$(mktemp -d "${TMPDIR:-/tmp}/gen-run.XXXXXXXX")"; trap 'rm -rf "$W"' EXIT

# ---- 2. prompt manifests ----------------------------------------------------------------------------------
ED="$(rp "$(pv examples_dir)")" L1="$(kv examples_one)" LM="$(kv examples_many)"
[ -s "$ED/$L1" ] && [ -s "$ED/$LM" ] || die "example lists $ED/$L1, $ED/$LM"
AR="$W/arms"; mkdir -p "$AR/examples"
cp "$E445/arms/common.md" "$E445/arms/omegese.md" "$AR/"
cp "$ED/$L1" "$AR/examples/one.list"; cp "$ED/$LM" "$AR/examples/many.list"
mapfile -t EXS < <(cat "$ED/$L1" "$ED/$LM" | grep . | LC_ALL=C sort -u)
for e in "${EXS[@]}"; do [[ "$e" =~ ^[a-z0-9_-]+$ ]] && [ -d "$ED/$e" ] || die "example $e"; ln -s "$ED/$e" "$AR/examples/$e"; done
EXSHA="$( (cd "$ED" && sha256sum "$L1" "$LM" && for e in "${EXS[@]}"; do sha256sum "$e/requirement" "$e/prompt" "$e/program.omegese"; done) | sha256sum | cut -d' ' -f1)"
PFX_A="$(system_prompt "$AR" omegese one)" || die "cannot build the one-example prefix"
PFX_B="$(system_prompt "$AR" omegese many)" || die "cannot build the many-example prefix"
S="$W/stage"; mkdir -p "$S/blobs" "$S/manifests" "$S/retrieval"
blob() { local h; h="$(shas "$1")"; [ -f "$S/blobs/$h" ] || printf '%s' "$1" > "$S/blobs/$h"; echo "$h"; }
HA="$(blob "$PFX_A")" HB="$(blob "$PFX_B")" HE="$(blob "")"
PSHA="$(sha "$PROV")"
declare -A MAN PFXH SUFH
: > "$S/manifests.tsv"
for t in "${IDS[@]}"; do
  req="$TASKS/tasks/cases/$t/requirement" pr="$TASKS/tasks/cases/$t/prompt"
  user="$(user_msg "$req" "$pr")"; hu="$(blob "$user")"
  for cd in "${CONDS[@]}"; do
    c="$(tr A-Z a-z <<< "$cd")" ph="$HB" suffix="" sh="$HE" pc=- pcs=- ps=- cms=- kind=- md=- kk=- cap=- ex=- qs=- rs=- ret=-
    [ "$cd" = A ] && ph="$HA"
    if [ "$cd" = C ] || [ "$cd" = D ]; then
      kind=raw; [ "$cd" = D ] && kind=abstraction
      rd="$S/retrieval/$c/$t"; mkdir -p "$rd"
      args=(query --corpus "$CM" --manifest-sha256 "$CMS" --recheck "$RCK" --kind "$kind" --k "$K" --cap-bytes "$CAP" --requirement "$req" --prompt "$pr" --out "$rd/out" --mode "$RMODE")
      [ "$CONTROL" = cross-family ] && args+=(--exclude-family "${EXT[$t]}")
      printf '%s\n' "${args[@]}" | sed -e "s|^$CM\$|<corpus_manifest>|" -e "s|^$req\$|<requirement>|" -e "s|^$pr\$|<prompt>|" -e "s|^$rd/out\$|<out>|" > "$rd/argv"
      "$PROV" "${args[@]}" 2> "$W/prov.err" || die "provider failed closed for $cd $t: $(head -c 300 "$W/prov.err")"
      (cd "$rd/out" && sha256sum -c --quiet --strict SHA256SUMS > /dev/null 2>&1) || die "provider output for $cd $t does not verify its SHA256SUMS"
      suffix="$(cat "$rd/out/suffix.txt"; printf x)"; suffix="${suffix%x}"
      sh="$(blob "$suffix")"
      pc=genret-cli-v2 pcs="$PCS" ps="$PSHA" cms="$CMS" md="$RMODE" kk="$K" cap="$CAP" ex="$([ "$CONTROL" = cross-family ] && echo "${EXT[$t]}" || echo -)"
      qs="$(sed -n 's/^query_sha256=//p' "$rd/out/receipt.txt")" rs="$(sha "$rd/out/SHA256SUMS")"
      ret="$(tail -n +2 "$rd/out/selected.tsv" | awk -F'\t' '{ printf "%s%s:%s:%s:%s", s, $1, $2, $3, $6; s = "," }')"; [ -n "$ret" ] || ret=-
    fi
    sys="$(cat "$S/blobs/$ph")$suffix"
    m="$(printf '%s\n' "schema=omega.gen.prompt_manifest.v1" "condition=$cd" "task=$t" "prefix_sha256=$ph" "suffix_sha256=$sh" \
      "suffix_bytes=$(printf '%s' "$suffix" | wc -c)" "system_sha256=$(shas "$sys")" "user_sha256=$hu" "provider_contract=$pc" \
      "provider_contract_sha256=$pcs" "provider_sha256=$ps" "corpus_manifest_sha256=$cms" "kind=$kind" "mode=$md" "k=$kk" "cap_bytes=$cap" \
      "target_bytes=-" "exclude_family=$ex" "query_sha256=$qs" "retrieval_sha256=$rs" "retrieved=$ret")"
    mh="$(printf '%s\n' "$m" | sha256sum | cut -d' ' -f1)"
    printf '%s\n' "$m" > "$S/manifests/$mh"
    printf '%s\t%s\t%s\n' "$cd" "$t" "$mh" >> "$S/manifests.tsv"
    MAN[$cd:$t]="$mh" PFXH[$cd:$t]="$ph" SUFH[$cd:$t]="$sh"
  done
done
LC_ALL=C sort -o "$S/manifests.tsv" "$S/manifests.tsv"
mv_() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$S/manifests/$2"; }

# ---- 3. meta.txt and resume -------------------------------------------------------------------------------
srcsha="$( (cd "$ROOT" && find tools/gen/runner tools/gen/reducer -type f ! -path '*/testdata/*' -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
  sha256sum tools/exp445/reader.sh tools/exp445/sampling-gate.sh tools/exp445/arms/common.md tools/exp445/arms/omegese.md) | sha256sum | cut -d' ' -f1)"
PROTO="$(pv protocol)"; PROTOSHA=PLACEHOLDER; [ -n "$PROTO" ] && [ -f "$(rp "$PROTO")" ] && PROTOSHA="$(sha "$(rp "$PROTO")")"
pubsha() { (cd "$1" && find tasks -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1); }
devsha="$(for t in "${DEVIDS[@]}"; do sha256sum "$DEVD/$t/requirement" "$DEVD/$t/prompt" | sed "s|$DEVD/||"; done | sha256sum | cut -d' ' -f1)"
{
  echo "schema=omega.gen.run_meta.v1"; echo "revision=$(kv revision)"; echo "test_mode=$([ "$TEST" = 1 ] && echo yes || echo no)"
  echo "params_sha256=$(sha "$PARAMS")"; echo "protocol_sha256=$PROTOSHA"; echo "exp445_params_sha256=$(sha "$EP")"
  echo "config=$CONFIG"; echo "model=$MODEL"; echo "base_url=$BASE"; echo "request_extra=$EXTRA"; echo "server=$SERVER"
  echo "weights_list_sha256=$(sha "$WLIST")"; echo "mask=off"; echo "conditions=$(kv conditions)"; echo "attempts=$ATT"
  echo "max_tokens=$MAXT"; echo "greedy_temperature=$GTEMP"; echo "greedy_top_p=$GTOPP"; echo "temperature=$TEMP"; echo "top_p=$TOPP"
  echo "seed_base=$SB"; echo "timeout_s=$TIMEOUT"; echo "retries=$RETRIES"; echo "repair_rounds=1"; echo "order=attempt,task,condition"
  echo "commit=$COMMIT"; echo "source_sha256=$srcsha"
  echo "prefix_sha256.A=$HA"; echo "prefix_sha256.B=$HB"; echo "examples_sha256=$EXSHA"
  echo "provider_version=$PVER"; echo "provider_sha256=$PSHA"; echo "provider_contract_sha256=$PCS"; echo "recheck_sha256=$(sha "$RCK")"; echo "corpus_manifest_sha256=$CMS"
  echo "retrieval_mode=$RMODE"; echo "k=$K"; echo "cap_bytes=$CAP"; echo "control=$CONTROL"; echo "famsig_sha256=$([ "$CONTROL" = none ] && echo - || sha "$FS")"; echo "dev_meta_sha256=$([ "$CONTROL" = none ] && echo - || sha "$DM")"
  echo "gate_sha256=$GSHA"; echo "greedy_gate_tasks=$(IFS=,; echo "${GT[*]}")"; echo "g0_task=$G0T"; echo "g0_seeds=$G0N"
  echo "tasks=${#IDS[@]}"; echo "tasks_public_sha256=$(pubsha "$TASKS")"; echo "dev_public_sha256=$devsha"
  echo "manifests_sha256=$(sha "$S/manifests.tsv")"
} > "$S/meta.txt"
if [ -f "$OUT/meta.txt" ]; then
  cmp -s "$OUT/meta.txt" "$S/meta.txt" || { diff "$OUT/meta.txt" "$S/meta.txt" | grep '^[<>]' | head -n 6 >&2 || true
    die "resume refused: the run's identity changed (meta.txt differs above); a changed input is a new experimental revision in a new run dir"; }
  cmp -s "$OUT/manifests.tsv" "$S/manifests.tsv" || die "resume refused: manifests.tsv in the run dir differs from the rebuilt manifests"
  echo "gen run.sh: resuming $OUT (identity unchanged)" >&2
else
  [ -z "$(ls -A "$OUT" | grep -vxE 'weights\.check|server\.json|\.lock' || true)" ] || die "$OUT holds files but no meta.txt: refused"
  cp -R "$S/blobs" "$S/manifests" "$S/retrieval" "$S/manifests.tsv" "$OUT/"
  mv "$S/meta.txt" "$OUT/meta.txt"
fi

run_sums() { (cd "$OUT" && find . -type f ! -path ./SHA256SUMS ! -path ./.lock ! -path './gate/g0.tmp/*' -printf '%P\n' | LC_ALL=C sort | tr '\n' '\0' | xargs -0 sha256sum) > "$OUT/SHA256SUMS"; }
sys_of() { local s; s="$(cat "$OUT/blobs/${SUFH[$1]}"; printf x)"; printf '%s%s' "$(cat "$OUT/blobs/${PFXH[$1]}")" "${s%x}"; }
cmode() { jq -r '.usage.aien_sampling.mode | if . == null then "-" elif type == "string" and test("^[a-z_]{1,16}$") then . else "BAD" end' "$1" 2> /dev/null || echo BAD; }
csha() { jq -j '.choices[0].message.content' "$1" | sha256sum | cut -d' ' -f1; }

# ---- 4. greedy gate ----------------------------------------------------------------------------------------
GG="$OUT/gate/greedy"
if [ ! -f "$GG/verdict.txt" ]; then
  mkdir -p "$GG"; : > "$W/gg"; gok=1
  declare -A DI; i=0; for t in "${DEVIDS[@]}"; do i=$((i + 1)); DI[$t]=$i; done
  for t in "${GT[@]}"; do
    d="$GG/$t"; mkdir -p "$d"
    q="$(request_json "$MODEL" "$(first_msgs "$PFX_B" "$(user_msg "$DEVD/$t/requirement" "$DEVD/$t/prompt")")" "$MAXT" "$GTEMP" "$GTOPP" "$(seed_of "$SB" 1 "${DI[$t]}")" "$EXTRA")"
    ask "$d" 1 "$q" > /dev/null || true; ask "$d" 2 "$q" > /dev/null || true
    if round_ok "$d/r1" && round_ok "$d/r2"; then
      m1="$(cmode "$d/r1.resp.json")" m2="$(cmode "$d/r2.resp.json")" same=no
      [ "$(csha "$d/r1.resp.json")" = "$(csha "$d/r2.resp.json")" ] && same=yes
      [ "$same" = yes ] && [ "$m1" = greedy ] && [ "$m2" = greedy ] || gok=0
      echo "task=$t identical=$same mode_r1=$m1 mode_r2=$m2" >> "$W/gg"
    else gok=0; echo "task=$t identical=no transport=FAILED" >> "$W/gg"; fi
  done
  { cat "$W/gg"; echo "rule=PASS needs every task's two replies byte-identical and mode=greedy in every reply"; echo "GREEDY $([ "$gok" = 1 ] && echo PASS || echo FAIL)"; } > "$GG/verdict.txt.tmp"
  mv "$GG/verdict.txt.tmp" "$GG/verdict.txt"
fi
if [ "$(tail -n 1 "$GG/verdict.txt")" != "GREEDY PASS" ]; then
  run_sums; echo "gen run.sh: greedy gate FAILED ($GG/verdict.txt): no primary attempt is run" >&2; exit 1
fi

# ---- 5-7. attempts -----------------------------------------------------------------------------------------
HDR="$(printf '%s\t' set task rounds prompt_tokens completion_tokens_r1 completion_tokens_r2 finish_r1 finish_r2 wall_s check_r1 check_r2 candidate_sha256 \
  condition attempt seed temperature top_p max_tokens prompt_tokens_r1 prompt_tokens_r2 tries_r1 tries_r2 mode_r1 mode_r2 seed_reported_r1 wall_s_r1 \
  candidate_r1_sha256 manifest_sha256 retrieval_sha256 retrieved suffix_bytes)"; HDR="${HDR%$'\t'}"
[ -f "$OUT/log.tsv" ] || printf '%s\n' "$HDR" > "$OUT/log.tsv"
[ "$(head -n 1 "$OUT/log.tsv")" = "$HDR" ] || die "log.tsv header is not run row v1"
declare -A TI; i=0; for t in "${IDS[@]}"; do i=$((i + 1)); TI[$t]=$i; done
hook() { if [ "$TEST" = 1 ] && [ "${GEN_TEST_KILL:-}" = "$1:$set_id:$task" ]; then kill -KILL $$; fi; }

g0_gate() { # sets G0V to PASS or FAIL; writes gate/g0 once (EXP445 G0 layout and rule)
  local g="$OUT/gate/g0" w="$OUT/gate/g0.tmp" k s q der ge=0 base ti
  if [ -f "$g/verdict.txt" ]; then G0V="$(tail -n 1 "$g/verdict.txt" | sed -n 's/^G0 \(PASS\|FAIL\).*/\1/p')"; return; fi
  rm -rf "$w"; mkdir -p "$w"; cp "$GATE_SH" "$w/"
  printf '%s' "$PFX_B" > "$w/prompt.system.txt"
  ti="$(printf '%s\n' "${DEVIDS[@]}" | awk -v t="$G0T" '$0 == t { print NR }')"
  local msgs; msgs="$(first_msgs "$PFX_B" "$(user_msg "$DEVD/$G0T/requirement" "$DEVD/$G0T/prompt")")"
  for k in $(seq 1 $((G0N + 1))); do
    s="$(g0_seed "$SB" "$([ "$k" -le "$G0N" ] && echo "$k" || echo 1)" "$ti")"
    printf '%s\n' "$(request_json "$MODEL" "$msgs" "$MAXT" "$TEMP" "$TOPP" "$s" "$EXTRA")" > "$w/r$k.req.json"
  done
  base="${BASE%/}"; base="${base%/v1}"
  (cd "$w" && timeout "$(( (28 + G0N) * TIMEOUT ))" env -u GREEDY_GOLDEN REDACT_REPRO=0 ./sampling-gate.sh "$base" "$MODEL" gate $(seq -f 'r%g.req.json' 1 "$G0N")) > "$w/gate.out" 2>&1 || ge=$?
  [ "$ge" = 0 ] || [ "$ge" = 1 ] || die "the sampling gate could not complete (exit $ge: $(tail -n 2 "$w/gate.out" | tr '\n' ' ')); no G0 record written, rerun to resume"
  q="$(cat "$w/r$((G0N + 1)).req.json")"
  ask "$w" "$((G0N + 1))" "$q" > /dev/null || true
  round_ok "$w/r$((G0N + 1))" || die "the G0 repeat got no response; no G0 record written, rerun to resume"
  der="$(g0_derive "$w" "$G0N")"
  if g0_pass "$der" "$G0N"; then G0V=PASS; else G0V=FAIL; fi
  { echo "task=$G0T"; echo "condition=B"; echo "temperature=$TEMP"; echo "top_p=$TOPP"; echo "gate_sha256=$GSHA"; echo "gate_exit=$ge"
    echo "rule=EXP445 G0 (reader.sh g0_pass): sampling gate VERDICT PASS, the repeat byte-identical, every reply mode=sampled with its own seed"
    echo "derived=$der"; [ "$G0V" = PASS ] && echo "pass3=AVAILABLE" || echo "pass3=UNAVAILABLE (attempts 2..$ATT not run)"; echo "G0 $G0V"; } > "$w/verdict.txt"
  (cd "$w" && g0_files . | tr '\n' '\0' | xargs -0 sha256sum) > "$w/SHA256SUMS"
  mv "$w" "$g"
}

G0V=""
for at in $(seq 1 "$ATT"); do
  if [ "$at" -ge 2 ]; then
    [ -n "$G0V" ] || g0_gate
    if [ "$G0V" != PASS ]; then echo "gen run.sh: G0 FAILED: attempts 2..$ATT are not run, pass@3 = UNAVAILABLE" >&2; break; fi
  fi
  tt="$GTEMP" tp="$GTOPP"; [ "$at" = 1 ] || { tt="$TEMP" tp="$TOPP"; }
  for task in "${IDS[@]}"; do
    req="$TASKS/tasks/cases/$task/requirement"
    user="$(user_msg "$req" "$TASKS/tasks/cases/$task/prompt")"
    for cd in "${CONDS[@]}"; do
      c="$(tr A-Z a-z <<< "$cd")" set_id="$(tr A-Z a-z <<< "$cd")-$at"; d="$OUT/gen/$set_id/$task"
      [ -f "$d/done" ] && continue
      mkdir -p "$d"
      seed="$(seed_of "$SB" "$at" "${TI[$task]}")"
      msgs="$(first_msgs "$(sys_of "$cd:$task")" "$user")"
      if ask "$d" 1 "$(request_json "$MODEL" "$msgs" "$MAXT" "$tt" "$tp" "$seed" "$EXTRA")"; then
        a1="$(content_of "$d/r1.resp.json")"; printf '%s' "$a1" > "$d/r1.content"
        reader "$X" "$EMIT" omegese "$d/r1.content" "$d/r1" "$req"
        hook after-r1
        k1="$(cat "$d/r1.check")"
        if [ "$k1" != OK ] && ask "$d" 2 "$(request_json "$MODEL" "$(repair_msgs "$msgs" "$a1" "$k1")" "$MAXT" "$tt" "$tp" "$seed" "$EXTRA")"; then
          printf '%s' "$(content_of "$d/r2.resp.json")" > "$d/r2.content"
          reader "$X" "$EMIT" omegese "$d/r2.content" "$d/r2" "$req"
        fi
      fi
      gen_row "$set_id" "$task" "$d" "$d"
      pt1=- pt2=- tr1=0 tr2=- m1=- m2=- sr=- c1=-
      [ -f "$d/r1.transport" ] && tr1="$(grep -c . "$d/r1.transport")"
      [ -f "$d/r2.transport" ] && tr2="$(grep -c . "$d/r2.transport")"
      if round_ok "$d/r1"; then pt1="$(usage_of "$d/r1.resp.json" | cut -f1)"; m1="$(cmode "$d/r1.resp.json")"
        sr="$(jq -r '.usage.aien_sampling.seed | if type == "number" and . == floor then tostring else "-" end' "$d/r1.resp.json" 2> /dev/null || echo -)"; fi
      if round_ok "$d/r2"; then pt2="$(usage_of "$d/r2.resp.json" | cut -f1)"; m2="$(cmode "$d/r2.resp.json")"; fi
      w1="$(if [ -f "$d/r1.transport" ]; then sed -n 's/.* time_s=\([^ ]*\).*/\1/p' "$d/r1.transport"; fi | awk '{ s += $1 } END { printf "%.6f", s }')"
      [ -f "$d/r1.candidate.oscir" ] && c1="$(sha "$d/r1.candidate.oscir")"
      mh="${MAN[$cd:$task]}"
      ROW="$ROW$(printf '\t%s' "$cd" "$at" "$seed" "$tt" "$tp" "$MAXT" "$pt1" "$pt2" "$tr1" "$tr2" "$m1" "$m2" "$sr" "$w1" "$c1" "$mh" \
        "$(mv_ retrieval_sha256 "$mh")" "$(mv_ retrieved "$mh")" "$(mv_ suffix_bytes "$mh")")"
      rm -rf "$OUT/cands/$set_id/$task" "$OUT/cands/$set_id-final/$task"
      if [ -f "$d/r1.candidate.oscir" ]; then mkdir -p "$OUT/cands/$set_id/$task"; cp "$d/r1.candidate.oscir" "$OUT/cands/$set_id/$task/candidate.oscir"; fi
      if [ -n "$FINAL" ] && [ -f "$d/r$FINAL.candidate.oscir" ]; then mkdir -p "$OUT/cands/$set_id-final/$task"; cp "$d/r$FINAL.candidate.oscir" "$OUT/cands/$set_id-final/$task/candidate.oscir"; fi
      mkdir -p "$OUT/cands/$set_id" "$OUT/cands/$set_id-final"
      have="$(awk -F'\t' -v s="$set_id" -v t="$task" '$1 == s && $2 == t' "$OUT/log.tsv")"
      if [ -z "$have" ]; then printf '%s\n' "$ROW" >> "$OUT/log.tsv"
      elif [ "$have" != "$ROW" ]; then die "log.tsv already holds a different row for $set_id $task: refused (gen/ is the record)"; fi
      hook after-log
      gen_sums "$d"
      touch "$d/done"
      echo "gen run.sh: $(cut -f1-3,10,11 <<< "$ROW" | tr '\t' ' ')" >&2
    done
  done
done
run_sums
echo "gen run.sh: done, $(($(wc -l < "$OUT/log.tsv") - 1)) generations in $OUT"
