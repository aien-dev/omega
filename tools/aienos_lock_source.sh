#!/bin/bash
# aienos_lock_source.sh -- resolve and prove the aienos source pinned by aienos.lock.
#
# The trusted capability root is the native C library at the FULL commit in aienos.lock
# (spec C3). A directory name, an abbreviated SHA or an environment override is not proof
# of that identity. This tool proves it from git objects:
#   * the lock must be one full 40-hex commit id;
#   * a repository counts only if it holds that commit object; replace refs and grafts
#     are disabled (GIT_NO_REPLACE_OBJECTS) and every object read is re-hashed, so the
#     content returned is the content the commit id names;
#   * a directory counts only if every file under the asked subpaths has the raw bytes
#     (no filters, no attributes) and mode of the locked commit and no other file (build
#     outputs included) sits beside them.
#
# Usage:
#   aienos_lock_source.sh identity                 one JSON object: lock, repo, tree, status
#   aienos_lock_source.sh show <path>              file content at the locked commit
#   aienos_lock_source.sh blob <path>              blob id of <path> at the locked commit
#   aienos_lock_source.sh verify-dir <dir> <subpath>...   prove <dir> holds the locked subpaths
#   aienos_lock_source.sh materialize <dir> <subpath>...  extract missing subpaths into <dir>
#                                                  (only if empty of them), then verify-dir
# Exit status: 0 proven; 1 verified mismatch or verified absence (the locked commit was read
# and the path is missing, or the directory differs); 2 unavailable (no repository holds
# the locked commit, or the lock is malformed). 2 never means "absent".
#
# Environment:
#   AIENOS_LOCK_FILE   lock file (default <omega>/aienos.lock)
#   AIENOS_LOCK        lock value instead of the file (tests only; must still be full 40-hex)
#   AIENOS_LOCK_REPO   repository to read objects from (default <omega>/../aienos-argus-cap);
#                      a place to look, not proof: the commit object is the proof
#   AIENOS_R7_DIR      if it lies inside a git repository, that repository is also tried
set -u
export GIT_NO_REPLACE_OBJECTS=1 GIT_GRAFT_FILE=/nonexistent/aienos-lock-source-grafts
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

die2() { echo "aienos_lock_source: UNAVAILABLE: $*" >&2; exit 2; }
die1() { echo "aienos_lock_source: MISMATCH: $*" >&2; exit 1; }

LOCK=${AIENOS_LOCK:-$(head -n 1 "${AIENOS_LOCK_FILE:-$HERE/aienos.lock}" 2>/dev/null)}
[[ "$LOCK" =~ ^[0-9a-f]{40}$ ]] || die2 "aienos.lock is not one full 40-hex commit id: '${LOCK}'"

# rehash <repo> <type> <id>: the object's content hashes to its id.
rehash() {
    local got
    got=$(git -C "$1" cat-file "$2" "$3" 2>/dev/null | git hash-object -t "$2" --stdin 2>/dev/null) || return 1
    [ "$got" = "$3" ]
}

REPO=""
for cand in "${AIENOS_LOCK_REPO:-$HERE/../aienos-argus-cap}" \
            "$( [ -n "${AIENOS_R7_DIR:-}" ] && [ -d "$AIENOS_R7_DIR" ] && git -C "$AIENOS_R7_DIR" rev-parse --show-toplevel 2>/dev/null)"; do
    [ -n "$cand" ] && [ -d "$cand" ] || continue
    [ "$(git -C "$cand" rev-parse --verify -q "$LOCK^{commit}" 2>/dev/null)" = "$LOCK" ] || continue
    rehash "$cand" commit "$LOCK" || continue
    REPO=$(cd "$cand" && pwd); break
done
TREE=""
if [ -n "$REPO" ]; then
    TREE=$(git -C "$REPO" rev-parse --verify -q "$LOCK^{tree}")
    rehash "$REPO" tree "$TREE" || die2 "tree object $TREE of $LOCK fails its hash in $REPO"
fi
need_repo() { [ -n "$REPO" ] || die2 "no repository holds commit $LOCK (AIENOS_LOCK_REPO=${AIENOS_LOCK_REPO:-unset})"; }

# check_trees <subpath>: every tree object on the way to and under <subpath> re-hashes.
check_trees() {
    local mode type id path
    while read -r mode type id path; do
        [ "$type" = tree ] || continue
        rehash "$REPO" tree "$id" || die2 "tree $id ($path) fails its hash"
    done < <(git -C "$REPO" ls-tree -r -t "$LOCK" -- "$1")
}

cmd=${1:-}; shift || true
case "$cmd" in
identity)
    if [ -n "$REPO" ]; then
        printf '{"aienos_lock": "%s", "source": "git-object", "repo": "%s", "commit_tree": "%s", "status": "available"}\n' "$LOCK" "$REPO" "$TREE"
    else
        printf '{"aienos_lock": "%s", "source": "none", "repo": "", "commit_tree": "", "status": "unavailable"}\n' "$LOCK"
        exit 2
    fi ;;
blob|show)
    [ $# = 1 ] || die2 "usage: $cmd <path>"
    need_repo
    check_trees "$1"
    id=$(git -C "$REPO" rev-parse --verify -q "$LOCK:$1") || die1 "$1 is absent at $LOCK"
    [ "$(git -C "$REPO" cat-file -t "$id")" = blob ] || die1 "$1 is not a file at $LOCK"
    rehash "$REPO" blob "$id" || die2 "blob $id ($1) fails its hash"
    if [ "$cmd" = blob ]; then echo "$id"; else git -C "$REPO" cat-file blob "$id"; fi ;;
verify-dir|materialize)
    [ $# -ge 2 ] || die2 "usage: $cmd <dir> <subpath>..."
    dir=$1; shift
    need_repo
    for s in "$@"; do
        git -C "$REPO" rev-parse --verify -q "$LOCK:$s" >/dev/null || die1 "$s is absent at $LOCK"
        check_trees "$s"
    done
    if [ "$cmd" = materialize ]; then
        missing=()
        for s in "$@"; do [ -e "$dir/$s" ] || missing+=("$s"); done
        if [ ${#missing[@]} -gt 0 ]; then
            mkdir -p "$dir" || die2 "cannot create $dir"
            git -C "$REPO" archive "$LOCK" "${missing[@]}" | tar -x -C "$dir" || die2 "extraction into $dir failed"
        fi
    fi
    [ -d "$dir" ] || die1 "$dir does not exist"
    dir=$(cd "$dir" && pwd)
    # Compare raw bytes, outside git's checkout machinery: no index, no ignore rules,
    # no .gitattributes (a planted "* text eol=lf" would hide CRLF edits), no config
    # filters. Each file is hashed with --no-filters and must equal the locked blob;
    # its type and executable bit must equal the locked mode; and the subpaths must
    # hold no other file (build outputs included: the authority is built out of tree).
    tmpd=$(mktemp -d "${TMPDIR:-/tmp}/aienos-lock-vd.XXXXXX") || die2 "mktemp failed"
    trap 'rm -rf "$tmpd"' EXIT
    git -C "$REPO" ls-tree -r -z --full-tree "$LOCK" -- "$@" > "$tmpd/tree" || die2 "ls-tree $LOCK failed"
    : > "$tmpd/want"; : > "$tmpd/regular"; : > "$tmpd/regular.ids"; bad=""
    while IFS= read -r -d '' ent; do
        meta=${ent%%$'\t'*}; path=${ent#*$'\t'}
        read -r mode type id <<< "$meta"
        case "$path" in *$'\n'*) die2 "path with a newline at $LOCK: refusing" ;; esac
        printf '%s\n' "$path" >> "$tmpd/want"
        f=$dir/$path
        case "$mode" in
        120000)
            if [ -L "$f" ] && [ "$(readlink "$f" | tr -d '\n' | git -C "$REPO" hash-object --no-filters --stdin)" = "$id" ]; then :
            else bad+="$path(symlink) "; fi ;;
        100644|100755)
            if [ -L "$f" ] || [ ! -f "$f" ]; then bad+="$path(not a file) "; continue; fi
            if [ "$mode" = 100755 ] && [ ! -x "$f" ]; then bad+="$path(mode) "; continue; fi
            if [ "$mode" = 100644 ] && [ -x "$f" ]; then bad+="$path(mode) "; continue; fi
            printf '%s\n' "$f" >> "$tmpd/regular"; printf '%s\n' "$id" >> "$tmpd/regular.ids" ;;
        *) die2 "$path has unsupported mode $mode ($type) at $LOCK" ;;
        esac
    done < "$tmpd/tree"
    [ -s "$tmpd/want" ] || die1 "no files under $* at $LOCK"
    if [ -s "$tmpd/regular" ]; then
        git -C "$REPO" hash-object --no-filters --stdin-paths < "$tmpd/regular" > "$tmpd/regular.got" 2>/dev/null \
            || die1 "cannot hash the files under $dir"
        bad+=$(paste -d ' ' "$tmpd/regular.ids" "$tmpd/regular.got" "$tmpd/regular" \
               | awk -v d="$dir/" '$1 != $2 { p = substr($0, length($1) + length($2) + 3); sub("^" d, "", p); printf "%s(content) ", p }')
    fi
    [ -z "$bad" ] || die1 "$dir differs from $LOCK: $(echo "$bad" | cut -c1-300)"
    # Every non-directory entry under the subpaths must be a locked path.
    (cd "$dir" && find "$@" ! -type d -print0 2>/dev/null) | tr '\0' '\n' | sort > "$tmpd/have"
    sort -o "$tmpd/want" "$tmpd/want"
    extra=$(comm -13 "$tmpd/want" "$tmpd/have")
    [ -z "$extra" ] || die1 "$dir has files not in $LOCK (build outputs included; build out of tree): $(echo "$extra" | head -5 | tr '\n' ' ')"
    echo "aienos_lock_source: $dir matches $LOCK for $*" >&2 ;;
*)
    die2 "unknown command '$cmd' (identity | show | blob | verify-dir | materialize)" ;;
esac
