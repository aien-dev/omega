#!/usr/bin/env bash
# test_st_holdout_sig_cli.sh - end-to-end check of `searchtrace holdout sign`
# and `verify --strict` with freshly generated random TEST keys (never an
# owner key; the keys are deleted at the end). If openssl is installed, an
# OpenSSL-made Ed25519 TEST key (PKCS#8/SPKI DER, the owner ceremony's
# format) is also used, and OpenSSL independently checks the signature.
# Usage: bash tests/searchtrace/test_st_holdout_sig_cli.sh <searchtrace binary>
set -u
tool="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
umask 077
w="$(mktemp -d /tmp/st_holdout_sig_cli_XXXXXX)"
trap 'rm -rf "$w"' EXIT
fail=0
ok()  { echo "PASS $1"; }
bad() { echo "FAIL $1"; fail=1; }
expect() { # expect <exit code> <name> <command...>
    local want="$1" name="$2"; shift 2
    "$@" >"$w/out" 2>"$w/err"; local rc=$?
    if [[ $rc -eq $want ]]; then ok "$name"; else bad "$name (exit $rc, want $want: $(head -c 200 "$w/err"))"; fi
}
hex2bin() { local h; h="$(cat)"; printf "$(printf '%s' "$h" | sed 's/../\\x&/g')"; }

printf 'task alpha\ntask beta\ntask gamma\n' >"$w/taskset.txt"
head -c 32 /dev/urandom >"$w/salt.bin"
head -c 32 /dev/urandom >"$w/TEST-ONLY-random-key-1.sk"
head -c 32 /dev/urandom >"$w/TEST-ONLY-random-key-2.sk"
head -c 32 /dev/urandom >"$w/random-public-key.pk"
mkdir "$w/rec" "$w/k2"

rec="$("$tool" holdout commit g3-cli-test "$w/taskset.txt" "$w/salt.bin" "$w/rec")" \
    && ok "commit" || bad "commit"
d="$(basename "$rec" .txt)"; d="${d#g3-commit-}"
sig="$w/rec/g3-sig-$d.txt"

expect 2 "strict verify refuses an unsigned record" "$tool" holdout verify --strict "$w/random-public-key.pk" "$rec"
expect 0 "sign with random TEST key 1" "$tool" holdout sign "$rec" "$w/TEST-ONLY-random-key-1.sk" "$w/rec"
[[ -f "$sig" ]] && ok "signature file is g3-sig-<record digest>.txt" || bad "signature file name"
expect 0 "plain verify of the record unchanged" "$tool" holdout verify "$rec"
expect 2 "sign with key 2 refuses to replace key 1's signature" "$tool" holdout sign "$rec" "$w/TEST-ONLY-random-key-2.sk" "$w/rec"
chmod 644 "$w/TEST-ONLY-random-key-2.sk"
expect 2 "sign refuses a world-readable key file" "$tool" holdout sign "$rec" "$w/TEST-ONLY-random-key-2.sk" "$w/k2"
chmod 600 "$w/TEST-ONLY-random-key-2.sk"
mkfifo "$w/fifo.sk" && chmod 600 "$w/fifo.sk"
expect 2 "sign refuses a named pipe as key file (no hang)" timeout 20 "$tool" holdout sign "$rec" "$w/fifo.sk" "$w/k2"

if command -v openssl >/dev/null 2>&1; then
    openssl genpkey -algorithm ed25519 -out "$w/TEST-ONLY-openssl.pem" 2>/dev/null
    openssl pkey -in "$w/TEST-ONLY-openssl.pem" -outform DER -out "$w/TEST-ONLY-openssl.der.sk" 2>/dev/null
    openssl pkey -in "$w/TEST-ONLY-openssl.pem" -pubout -outform DER -out "$w/TEST-ONLY-openssl.der.pk" 2>/dev/null
    mkdir "$w/o"
    expect 0 "sign with an OpenSSL-made PKCS#8 TEST key" "$tool" holdout sign "$rec" "$w/TEST-ONLY-openssl.der.sk" "$w/o"
    osig="$w/o/g3-sig-$d.txt"
    expect 0 "strict verify with the SPKI public key" "$tool" holdout verify --strict "$w/TEST-ONLY-openssl.der.pk" "$rec" "$osig"
    expect 2 "strict verify of key 1's signature under the OpenSSL key refused" "$tool" holdout verify --strict "$w/TEST-ONLY-openssl.der.pk" "$rec" "$sig"
    # Independent check: OpenSSL verifies the same signature over
    # "omega.g3.holdout.sig.v1" 0x00 || record.
    { printf 'omega.g3.holdout.sig.v1\0'; cat "$rec"; } >"$w/msg.bin"
    sed -n 's/^signature //p' "$osig" | hex2bin >"$w/sig.bin"
    if [[ $(stat -c %s "$w/sig.bin") -eq 64 ]] && openssl pkeyutl -verify -pubin -inkey "$w/TEST-ONLY-openssl.der.pk" -keyform DER \
        -rawin -in "$w/msg.bin" -sigfile "$w/sig.bin" >/dev/null 2>&1; then
        ok "OpenSSL independently verifies the signature"
    else
        bad "OpenSSL independently verifies the signature"
    fi
    printf 'x' >>"$w/msg.bin"
    if openssl pkeyutl -verify -pubin -inkey "$w/TEST-ONLY-openssl.der.pk" -keyform DER \
        -rawin -in "$w/msg.bin" -sigfile "$w/sig.bin" >/dev/null 2>&1; then
        bad "OpenSSL refuses a changed message"
    else
        ok "OpenSSL refuses a changed message"
    fi
    kid="$(tail -c 32 "$w/TEST-ONLY-openssl.der.pk" | sha256sum | cut -c1-64)"
    grep -qx "key_id $kid" "$osig" && ok "key_id is sha256 of the public key" || bad "key_id is sha256 of the public key"
    # tamper: one signature hex digit changed (end line now stale)
    awk '/^signature /{c=substr($2,1,1); $2=(c=="0"?"1":"0") substr($2,2)} {print}' "$osig" >"$w/tampered.txt"
    expect 2 "strict verify refuses a tampered signature file" "$tool" holdout verify --strict "$w/TEST-ONLY-openssl.der.pk" "$rec" "$w/tampered.txt"
    head -c 100 "$osig" >"$w/trunc.txt"
    expect 2 "strict verify refuses a truncated signature file" "$tool" holdout verify --strict "$w/TEST-ONLY-openssl.der.pk" "$rec" "$w/trunc.txt"
    cp "$rec" "$w/o/rec.txt"; printf 'x' >>"$w/o/rec.txt"
    expect 2 "strict verify refuses a tampered commitment record" "$tool" holdout verify --strict "$w/TEST-ONLY-openssl.der.pk" "$w/o/rec.txt" "$osig"
    rm -f "$osig"
    expect 2 "strict verify refuses once the signature is removed" "$tool" holdout verify --strict "$w/TEST-ONLY-openssl.der.pk" "$rec" "$osig"
else
    echo "NOTE openssl not found: OpenSSL interop cross-check NOT_RUN (the C unit test still covers sign/verify)"
fi

if [[ $fail -eq 0 ]]; then echo "ST_HOLDOUT_SIG_CLI: PASS"; else echo "ST_HOLDOUT_SIG_CLI: FAIL"; exit 1; fi
