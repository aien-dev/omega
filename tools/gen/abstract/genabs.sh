# tools/gen/abstract/genabs.sh -- shell library of the GEN-3 extractor and verifier (aien-spark/omega#478).
# Sourced by tools/gen/abstract/extract and tools/gen/abstract/verify, so both run the same checks and write the
# same receipt bytes. Bash (as tools/exp445 uses), no Python. Deterministic: no clock, no randomness, C locale.
#
# ga_init <build dir> <repo root> <work dir>   binaries come from the build dir; corpus files from the root
# Every check calls the repo's own tools: omegese, omega_emit, x445, checker2, ola_runner (through gabs run).
export LC_ALL=C
GA_CORPUS=tests/one_layer/generalization/corpus
GA_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GA_PROPERTY_N=256

ga_init() {
  GA_B="$1" GA_ROOT="$2" GA_W="$3" GA_ERR=""
  GA_OMEGESE="$GA_B/omegese/omegese" GA_EMIT="$GA_B/omega_emit/omega_emit" GA_X445="$GA_B/omega_emit/x445"
  GA_CHK="$GA_B/checker2/checker2" GA_RUNNER="$GA_B/one_layer/ola_runner" GA_GABS="$GA_B/gen_abstract/gabs"
  GA_FAMSIG="${GEN_FAMSIG:-$GA_B/gen_abstract/famsig}"
  local t
  for t in "$GA_OMEGESE" "$GA_EMIT" "$GA_X445" "$GA_CHK" "$GA_RUNNER" "$GA_GABS" "$GA_FAMSIG"; do
    [ -x "$t" ] || { echo "genabs: missing tool $t (make omegese omega_emit checker2 ola gen-abstract)" >&2; return 2; }
  done
  mkdir -p "$GA_W/scratch"
}
ga_sha() { sha256sum "$1" | cut -c1-64; }
ga_shas() { printf '%s' "$1" | sha256sum | cut -c1-64; }

# Tool digests a receipt carries: the extractor's own sources (a change to them changes every receipt).
ga_tool_lines() {
  local f
  for f in extract genabs.sh gabs.c; do printf 'tool tools/gen/abstract/%s %s\n' "$f" "$(ga_sha "$GA_HERE/$f")"; done
}

# ga_chain <prog.omegese> <base> -- reader chain: omegese emit, omega_emit emit, checker2 check.
# Writes <base>.json and <base>.ir. On failure returns 1 with GA_ERR set.
ga_chain() {
  local out
  if ! out="$("$GA_OMEGESE" emit "$1" "$2.json" 2>&1)"; then GA_ERR="omegese emit refused: $(printf '%s' "$out" | head -n 1)"; return 1; fi
  if ! out="$("$GA_EMIT" emit "$2.json" "$2.ir" 2>&1)"; then GA_ERR="omega_emit refused: $(printf '%s' "$out" | head -n 1)"; return 1; fi
  if ! out="$("$GA_CHK" check "$2.ir" 2>&1)"; then GA_ERR="checker2 refused: $(printf '%s' "$out" | head -n 1)"; return 1; fi
  case "$out" in checker2=ACCEPT*) ;; *) GA_ERR="checker2: $out"; return 1;; esac
  return 0
}

# ga_fp <prog.omegese> <base> -- canonical IR fingerprint (RECORD_SCHEMA.md section 6) on stdout.
ga_fp() {
  "$GA_OMEGESE" emit "$1" "$2.fj" > /dev/null 2>&1 || { GA_ERR="omegese emit refused"; return 1; }
  "$GA_GABS" canon "$2.fj" "$2.cj" > /dev/null 2>&1 || { GA_ERR="gabs canon failed"; return 1; }
  "$GA_EMIT" emit "$2.cj" "$2.cir" > /dev/null 2>&1 || { GA_ERR="omega_emit refused the canonical program"; return 1; }
  ga_sha "$2.cir"
}

# Omegese scalar type -> requirement spelling
ga_req() {
  case "$1" in bool) echo bool;; uchar) echo u8;; ushort) echo u16;; uint) echo u32;; ulong) echo u64;;
    byte) echo i8;; short) echo i16;; int) echo i32;; long) echo i64;; *) return 1;; esac
}

# ga_params <fn line> -- sets GA_NAME, GA_PN[] (names), GA_PT[] (types; an array is "view uint 4"), GA_RET
# (Omegese), GA_REQP (params line value, requirement spelling), GA_FLATN (flattened scalar names),
# GA_FLATT (their requirement types), GA_FLATP (" name type" pairs for a wrapper fn line).
ga_params() {
  local w=($1) i=2 t
  GA_NAME="${w[1]}" GA_PN=() GA_PT=() GA_REQP="" GA_FLATN="" GA_FLATT="" GA_FLATP=""
  while [ "$i" -lt "${#w[@]}" ] && [ "${w[$i]}" != to ]; do
    GA_PN+=("${w[$i]}")
    case "${w[$((i + 1))]}" in
      view|edit|own)
        GA_PT+=("${w[$((i + 1))]} ${w[$((i + 2))]} ${w[$((i + 3))]}")
        t="$(ga_req "${w[$((i + 2))]}")" || return 1
        GA_REQP+=" ${w[$((i + 1))]} $t ${w[$((i + 3))]}"
        local j
        for ((j = 0; j < ${w[$((i + 3))]}; j++)); do
          GA_FLATN+=" ${w[$i]}_$j" GA_FLATT+=" $t" GA_FLATP+=" ${w[$i]}_$j ${w[$((i + 2))]}"
        done
        i=$((i + 4)) ;;
      *)
        GA_PT+=("${w[$((i + 1))]}")
        t="$(ga_req "${w[$((i + 1))]}")" || return 1
        GA_REQP+=" $t" GA_FLATN+=" ${w[$i]}" GA_FLATT+=" $t" GA_FLATP+=" ${w[$i]} ${w[$((i + 1))]}"
        i=$((i + 2)) ;;
    esac
  done
  GA_RET="${w[$((i + 1))]}"
}

# ga_wrappers <helper body> <pre> <post> <out prog> -- the helper, then gw_call (the helper on flattened
# scalar arguments), gw_pre, gw_post (post with res = the helper's result) and gw_chk (true when pre is false,
# else post). Arrays are packed from their scalars and freed before each return.
ga_wrappers() {
  ga_params "$(head -n 1 "$1")" || { GA_ERR="unknown type in the signature"; return 1; }
  local pack="" free="" args="" i
  for i in "${!GA_PN[@]}"; do
    local tw=(${GA_PT[$i]}) n="${GA_PN[$i]}"
    if [ "${#tw[@]}" = 3 ]; then
      [ "${tw[0]}" != own ] || { GA_ERR="own array parameter (v1 checks view and edit only)"; return 1; }
      pack+="$n = new ${tw[1]} ${tw[2]} 0"$'\n'
      local j
      for ((j = 0; j < tw[2]; j++)); do pack+="put $n $j ${n}_$j"$'\n'; done
      free+="free $n"$'\n'
    fi
    args+=" $n"
  done
  {
    cat "$1"
    printf 'fn gw_call%s to %s\n%sgw_r = %s%s\n%sret gw_r\nend\n' "$GA_FLATP" "$GA_RET" "$pack" "$GA_NAME" "$args" "$free"
    printf 'fn gw_pre%s to bool\n%sgw_r = bool false\nif %s\ngw_r = true\nend\n%sret gw_r\nend\n' "$GA_FLATP" "$pack" "$2" "$free"
    printf 'fn gw_post%s to bool\n%sres = %s%s\ngw_r = bool false\nif %s\ngw_r = true\nend\n%sret gw_r\nend\n' "$GA_FLATP" "$pack" "$GA_NAME" "$args" "$3" "$free"
    printf 'fn gw_chk%s to bool\nif gw_pre%s\nret gw_post%s\nend\nret true\nend\n' "$GA_FLATP" "$GA_FLATN" "$GA_FLATN"
  } > "$4"
}

# ga_run <label> <ir> <entry> <ret req> <inputs> <expected> <types...> -- one harness-runner check; appends
# "<label> inputs=<sha> expected=<sha> <verdict>" to GA_RCPT; returns 1 (GA_ERR) unless ACCEPT.
ga_run() {
  local label="$1" ir="$2" entry="$3" ret="$4" in="$5" exp="$6"; shift 6
  local v; v="$("$GA_GABS" run "$GA_RUNNER" "$GA_W/scratch" "$ir" "$entry" "$ret" "$in" "$exp" "$@" 2>&1)" || true
  GA_RCPT+="$label inputs=$(ga_sha "$in") expected=$(ga_sha "$exp") $v"$'\n'
  case "$v" in "ACCEPT n="*) return 0;; *) GA_ERR="$label check failed: $v"; return 1;; esac
}

# ga_abs_checks <id> <helper body> <spec-like file: desc pre post example limit counter lines> <work prefix>
# Runs every check of an abstraction and leaves the receipt text in GA_RCPT. Returns 1 with GA_ERR on the first
# failure. The receipt holds the exact stated lines, so any edit of pre/post/example/counter changes it.
ga_abs_checks() {
  local id="$1" body="$2" spec="$3" w="$4" pre post l
  GA_RCPT="gen-receipt 1"$'\n'"record $id"$'\n'"$(ga_tool_lines)"$'\n'
  pre="$(sed -n 's/^pre //p' "$spec")" post="$(sed -n 's/^post //p' "$spec")"
  ga_chain "$body" "$w.h" || return 1
  GA_RCPT+="helper_ir $(ga_sha "$w.h.ir")"$'\n'"checker2 ACCEPT"$'\n'
  GA_RCPT+="$(grep -E '^(pre|post|example|counter) ' "$spec")"$'\n'
  ga_wrappers "$body" "$pre" "$post" "$w.wrap.omegese" || return 1
  ga_chain "$w.wrap.omegese" "$w.wrap" || { GA_ERR="wrapper program: $GA_ERR (pre or post is not a valid bool value)"; return 1; }
  local ret; ret="$(ga_req "$GA_RET")"
  # property: chk on generated inputs must be true everywhere
  "$GA_GABS" inputs "$GA_PROPERTY_N" $GA_FLATT > "$w.pin" || { GA_ERR="gabs inputs failed"; return 1; }
  local i; for ((i = 0; i < GA_PROPERTY_N; i++)); do echo 1; done > "$w.pexp"
  ga_run "property" "$w.wrap.ir" gw_chk bool "$w.pin" "$w.pexp" $GA_FLATT || return 1
  # the stated example
  l="$(sed -n 's/^example //p' "$spec")"
  printf '%s\n' "${l% -> *}" > "$w.ein"; printf '%s\n' "${l##* -> }" > "$w.eexp"
  ga_run "example" "$w.wrap.ir" gw_call "$ret" "$w.ein" "$w.eexp" $GA_FLATT || return 1
  # every counterexample: pre is false there, the helper gives the stated result, and post is false for it
  local k=0
  while IFS= read -r l; do
    k=$((k + 1))
    printf '%s\n' "${l% -> *}" > "$w.cin"
    printf '0\n' > "$w.c0"
    ga_run "counter$k-pre-false" "$w.wrap.ir" gw_pre bool "$w.cin" "$w.c0" $GA_FLATT || return 1
    printf '%s\n' "${l##* -> }" > "$w.cexp"
    ga_run "counter$k-result" "$w.wrap.ir" gw_call "$ret" "$w.cin" "$w.cexp" $GA_FLATT || return 1
    case "${l##* -> }" in
      TRAP*) ;;
      *) ga_run "counter$k-post-false" "$w.wrap.ir" gw_post bool "$w.cin" "$w.c0" $GA_FLATT || return 1 ;;
    esac
  done < <(sed -n 's/^counter //p' "$spec")
  return 0
}

# ga_raw_checks <id> <task dir> <work prefix> -- reader chain, x445 check against the requirement, and the
# harness runner on the task's public tests. Receipt text in GA_RCPT.
ga_raw_checks() {
  local id="$1" d="$2" w="$3" out
  GA_RCPT="gen-receipt 1"$'\n'"record $id"$'\n'"$(ga_tool_lines)"$'\n'
  ga_chain "$d/solution.omegese" "$w.s" || return 1
  GA_RCPT+="solution_ir $(ga_sha "$w.s.ir")"$'\n'"checker2 ACCEPT"$'\n'
  out="$("$GA_X445" check oscir "$w.s.ir" "$d/requirement" 2>&1)" || { GA_ERR="x445 check: $out"; return 1; }
  GA_RCPT+="x445 check oscir $out"$'\n'
  local p ret
  p="$(sed -n 's/^params//p' "$d/requirement")" ret="$(sed -n 's/^ret //p' "$d/requirement")"
  ga_run "tests" "$w.s.ir" "$(sed -n 's/^entry //p' "$d/requirement")" "$ret" "$d/inputs" "$d/expected" $p || return 1
  return 0
}

# ga_scan -- every corpus task: solution digest, whole-solution fingerprint, and each helper (all functions but
# the last). Writes $GA_W/scan (sorted):
#   S <task> <split corpus|probe> <solution sha256> <solution fingerprint>
#   H <task> <k> <name> <calls|-> <helper fingerprint|-> <helper file>   (fingerprint "-" for a non-leaf helper)
ga_scan() {
  local root="$GA_ROOT/$GA_CORPUS" t d split fns line k name calls n fp
  : > "$GA_W/scan.u"
  for d in "$root"/tasks/*/; do
    t="$(basename "$d")"
    split=corpus; awk -v t="$t" '$1 == t { f = 1 } END { exit !f }' "$root/PROBES" && split=probe
    fp="$(ga_fp "$d/solution.omegese" "$GA_W/sol.$t")" || { echo "genabs: corpus solution $t does not pass the reader chain" >&2; return 1; }
    echo "S $t $split $(ga_sha "$d/solution.omegese") $fp" >> "$GA_W/scan.u"
    fns="$("$GA_GABS" split "$d/solution.omegese" "$GA_W/fn.$t")" || { echo "genabs: split $t failed" >&2; return 1; }
    n="$(printf '%s\n' "$fns" | wc -l)"
    while read -r _ k name calls _; do
      [ "$k" -lt $((n - 1)) ] || continue
      fp=-
      [ "$calls" != - ] || fp="$(ga_fp "$GA_W/fn.$t.$k.omegese" "$GA_W/hfp.$t.$k")" || { echo "genabs: helper $t/$name does not pass the reader chain" >&2; return 1; }
      echo "H $t $k $name $calls $fp $GA_W/fn.$t.$k.omegese" >> "$GA_W/scan.u"
    done <<< "$fns"
  done
  sort "$GA_W/scan.u" > "$GA_W/scan"
}

# ga_meta_token <task> -- family token of the task's META shape (famsig token; the hash is not reimplemented)
ga_meta_token() {
  local shape; shape="$(awk -v t="$1" '$1 == t { print $9 }' "$GA_ROOT/$GA_CORPUS/META")"
  [ -n "$shape" ] || { GA_ERR="no META line for $1"; return 1; }
  "$GA_FAMSIG" token "$shape"
}
