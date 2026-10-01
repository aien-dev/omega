# COMPOSITION-2 receipts

Receipts are named by the sha256 of their content and are never edited. This
index records which receipt is authoritative; older receipts are kept as history.

## Authoritative (Lane 17, 2026-10-01, main 4aa71bf)
- `3f43152b2da43bd1b16349bd60aec2f99eb8eab241378ec11e4a1f76747d56aa.json`: host gate
  (`tools/composition_gate.sh`), 14/14 steps x 2 runs, verdict PASS, commit
  4aa71bf5ea182a79cd42b3f0ada35b8310e8818a. It is the first gate receipt taken after close reclaims
  everything and two isolated compositions per World (#139).
- `4957ef16e1e53543c8b74ef297d7129cd238e4459c8c84f27c3a6719ac08c0bc.json`: R13 living World,
  silicon build (`make test-r13-silicon`, GB10 resident seat), run_commit 4aa71bf, tree_dirty false.
  `fabric_host_phase`: PASS, build silicon, 105 checks, 0 failures. Gate R13_LIVING_SYSTEM=SILICON_PASS_UNBOUND.
  `composition_host_phase` is NOT_RUN in silicon by design: it needs the -DRXC_TEST_HOOKS rogue hook (#135).
  The Fabric link is an in-process loopback stand-in; this is not a network or TRUST-1 qualification.

## Superseded (kept, not edited)
- `ca74b119bbb9008306638b68ae0e82f31d05c4c9ee35ba2b5bb0991f787778f6.json` (host gate, b4199ab): superseded by 3f43152b...
- `7a98ee26...`, `951410f2...`, `ea084f3f...` (host gate, earlier commits): already superseded by ca74b119... (Lane 3).

## GPU tier (still current for its scope)
- `b6d6eed1b92fda28226e0bd21c8ad5d598e6f85a89c7a80748e28c00f8542686.json` (`--gpu`, Lane 13, #131):
  both Skills executed on the GB10. Not rerun by Lane 17.
