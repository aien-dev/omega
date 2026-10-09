#!/bin/sh
# Stamp AT0_CASE_V1 identities onto a hand-written case body.
#   tests/mkcase.sh <body-file> > <case-file>
# The body holds every line from "OMEGA-AT0-CASE v1" through "end acceptance".
# Appends case_id, acceptance_id and "end" per AT0_CASE_V1 section 5
# (SHA-256 of domain tag, one zero byte, then the block lines). No Python.
set -eu
body=$1
tagged() { { printf '%s' "$1"; printf '\0'; sed -n "/^begin $2\$/,/^end $2\$/p" "$body"; } | sha256sum | cut -c1-64; }
cat "$body"
printf 'case_id %s\n' "$(tagged omega.at0.case.v1 semantic)"
printf 'acceptance_id %s\n' "$(tagged omega.at0.acceptance.v1 acceptance)"
printf 'end\n'
