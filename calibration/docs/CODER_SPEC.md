# EXP-001 coder specification (lane B)

Status: implemented and tested on branch `feat/turing-exp001-b`. Sections 1 to 8
are normative. Section 9 records the measured envelope; its rule is the profile field
`coder_envelope` and freezes with the profile.

Implementation: `src/turing/tc_pstream.[ch]` (formats), `src/turing/tc_range.[ch]`
(coder A), `src/turing/tc_rans.[ch]` (coder B), `src/turing/tc_produce.[ch]`
(predictor side), `src/turing/tc_tool.c` (CLI `turing-coder`). Tests:
`tests/turing/test_tc.c`, `tests/turing/test_tc_produce.c`, make target
`test-turing-exp001-coders` in `mk/turing_exp001_b.mk`.

## 1. Scope and roles

A predictor (a TYM0 model walking a CTR1 trace) emits a probability stream
(TPS1) and the observed symbols (TSY1) before any coding happens. Two reference
coders then turn (TPS1, TSY1) into a coded file, and decode (TPS1, coded file)
back to symbols. The coders never see the model: the coder sources do not
include `ty_model.h`, the make target greps for this, and `test_tc` is linked
without `ty_model.c`.

The TPS1 depends on the observed data (context keys come from past symbols), so
decoding with a TPS1 in hand checks the coder arithmetic and the bitstream, not
the causality of the predictor. Causality is a property of the producer
(section 7), which uses only past symbols of the same crumb.

Conventions: all integers little-endian. SHA-256 as in `src/sha256.c`.
"Domain digest" D(tag, bytes) = SHA-256(tag || 0x00 || bytes).

## 2. Probability stream TPS1

File = header (128 bytes) || count records || trailer (32 bytes).

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `TPS1` |
| 4 | 2 | version | 1 |
| 6 | 1 | K | alphabet size, 2..16 |
| 7 | 1 | qbits | 16 |
| 8 | 4 | flags | 0 |
| 12 | 8 | count | number of records n |
| 20 | 4 | record_bytes | 24 + 2K |
| 24 | 4 | reserved | 0 |
| 28 | 32 | profile_digest | the profile digest as 32 raw bytes: SHA-256 of the profile TOML file bytes, which is the 64-hex value on the sidecar line (DATA_FORMAT.md section 2), not the hash of the sidecar file |
| 60 | 32 | model_digest | `ty_model_digest` = SHA-256("turing.ymodel.v0" 0x00 \|\| .tym bytes) |
| 92 | 32 | dataset_digest | plain SHA-256 of the CTR1 file bytes |
| 124 | 4 | reserved | 0 |

Record t (t = 0..n-1), 24 + 2K bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | observation_index, must equal t |
| 8 | 8 | context_key (section 7) |
| 16 | 4 | crumb ordinal in the dataset file (0-based, section 7) |
| 20 | 4 | norm_sum, must equal 65536 |
| 24 | 2K | q[0..K-1], u16 each |

Probability of symbol x at step t is q[x] / 65536. Every entry must be at least
1 (zero-probability policy: refuse) and the K entries must sum to exactly 65536
(normalization policy: exact). The float vector and the quantized vector are the
same object: TYM0 models are quantized tables.

Trailer = D("turing.tc.pstream.v1", header || records). This value is "the TPS1
digest"; coded files bind to it.

A reader refuses, in this order: size below 160 bytes or wrong magic (FORMAT);
trailer mismatch (DIGEST); version, qbits, flags, reserved or K out of range
(FORMAT); record_bytes or file size disagreeing with count (FORMAT); then per
record: observation_index != t (INDEX), any q = 0 (ZERO), sum or norm_sum != 65536
(NORM).

## 3. Symbol file TSY1

File = header (56 bytes) || n symbol bytes || trailer (32 bytes).

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | magic | `TSY1` |
| 4 | 2 | version | 1 |
| 6 | 1 | K | 2..16 |
| 7 | 1 | reserved | 0 |
| 8 | 8 | count | n |
| 16 | 8 | reserved | 0 |
| 24 | 32 | dataset_digest | as in TPS1 |

Trailer = D("turing.tc.symbols.v1", header || symbols). Every symbol must be
below K. A (TPS1, TSY1) pair is valid only when K, count and dataset_digest agree
(else COUNT or DATASET).

## 4. Coded file header (both coders)

Coded file = header (56 bytes) || payload.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic: `TCR1` (range) or `TCA1` (rANS) |
| 4 | 1 | version = 1 |
| 5 | 1 | coder_id: 1 = range, 2 = rANS |
| 6 | 2 | reserved = 0 |
| 8 | 8 | count = TPS1 count |
| 16 | 32 | TPS1 digest (section 2 trailer) |
| 48 | 8 | payload_bytes |

The decoder checks, in this order: length below 56 or wrong magic (HEADER);
version, coder_id or reserved (HEADER); TPS1 digest (BINDING); count (COUNT);
file shorter than 56 + payload_bytes (TRUNC); file longer (TRAIL).

**Accounting rule.** Coded length in bits = 8 x (every byte of the coded file),
header and flush included. Overhead = coded bits minus the ideal length
(section 8). The 56-byte header alone is 448 bits.

## 5. Coder A: range coder (`TCR1`, id 1)

State: `low` u64 (only 33 bits used), `range` u32, `cache` u8, `have_cache`
flag, `pending` count of deferred 0xFF bytes. Constant TOP = 2^24. All products
below fit in u32 because range < 2^32 and r = range >> 16 < 2^16, q < 2^16.

For each record t, let q = row t, s = symbol t, cum = q[0] + ... + q[s-1].

Encoder:

```
low = 0; range = 0xFFFFFFFF; have_cache = 0; pending = 0
for t in 0..n-1:
    r = range >> 16
    low += r * cum
    range = (s == K-1) ? range - r*cum : r * q[s]     # last symbol takes the slack
    while range < TOP: range <<= 8; shift_low()
repeat 5 times: shift_low()

shift_low():
    if (low mod 2^32) < 0xFF000000 or (low >> 32) != 0:
        carry = low >> 32                               # 0 or 1
        if have_cache: emit(cache + carry)
        while pending > 0: emit(0xFF + carry mod 256); pending -= 1
        cache = (low >> 24) & 0xFF; have_cache = 1
    else:
        pending += 1
    low = (low & 0x00FFFFFF) << 8
```

There is no leading dummy byte: the first `shift_low` that resolves records the
cache without emitting. A carry while `have_cache` = 0 cannot occur and is
refused (CORRUPT).

Decoder (payload bytes b[0..L-1]):

```
if L < 4: refuse TRUNC
code = b[0..3] big-endian; pos = 4; range = 0xFFFFFFFF
for t in 0..n-1:
    if code >= range: refuse CORRUPT
    r = range >> 16
    s = smallest x in 0..K-2 with code < r * (q[0]+...+q[x]); else s = K-1
    code -= r * cum(s)
    range = (s == K-1) ? range - r*cum(s) : r * q[s]
    while range < TOP:
        if pos == L: refuse TRUNC
        code = (code << 8) | b[pos++]; range <<= 8
termination: code must be 0 (else CORRUPT); pos must equal L (else TRAIL)
```

## 6. Coder B: rANS (`TCA1`, id 2)

Constants: L = 2^23, scale 16 (M = 65536), byte renormalization. State x is u32
in [L, 2^31) between steps.

Encoder (symbols processed in reverse, bytes written back to front):

```
x = L
for t in n-1 down to 0:
    f = q[s]; c = cum(s)
    x_max = ((L >> 16) << 8) * f        # = 32768 * f
    while x >= x_max: push_front(x & 0xFF); x >>= 8
    x = ((x / f) << 16) + (x mod f) + c
push_front the 4 bytes of x, little-endian (so the payload starts x0 x1 x2 x3)
```

Decoder:

```
if L_bytes < 4: refuse TRUNC
x = b[0..3] little-endian; pos = 4
if x < 2^23 or x >= 2^31: refuse CORRUPT
for t in 0..n-1:
    m = x mod 65536
    s = the x with cum(x) <= m < cum(x) + q[x]
    x = q[s] * (x >> 16) + m - cum(s)
    while x < L:
        if pos == L_bytes: refuse TRUNC
        x = (x << 8) | b[pos++]
termination: x must equal L (else CORRUPT); pos must equal L_bytes (else TRAIL)
```

## 7. Producer context rule (predictor side)

`tc_produce` reimplements the static key walk of `ty_model.c` and cross-checks
that its ideal length equals `ty_model_score` exactly (else CORRUPT).

- Events are the CTR1 events in file order. The crumb ordinal starts at 0 and
  increments at every event with `first` set, except event 0. `first` means
  CTR1 event_index = 0 (DATA_FORMAT.md section 1.2; the file must start with one).
- History p[0..4] (PREV1..PREV5) resets to K (the "none" value) at every `first`
  event, then after the event p shifts and p[0] = symbol.
- Key = mixed radix, low digit first, over the fields in the model mask:
  OP (radix 16), DEPTH (radix TY_DEPTH_CLIP + 1 = 8), PREV1..PREV5 (radix K+1
  each), and finally POS = (crumb << 20) | min(idx, 2^20 - 1) as the top digit.
- Row = the model row whose key matches (binary search over sorted keys), else
  the model's default row. Every entry must lie in 1..65535 (else ZERO).

## 8. Ideal length

Ideal(t0..t1) = sum over t of `ub(q_t[x_t])` micro-bits (`ty_ubits_q16`, the same
integer method the TY-2 receipts use). ub(q) for q in 1..65536 is this exact rule and no
other (profile ideal_codelength_method, EVALUATOR.md section 6.2):

1. ip = index of the highest set bit of q; m = q << (62 - ip); frac = 0.
2. For i = 1..32: m = (m * m) >> 62 (128-bit product, truncated); if m >= 2^63,
   set bit (32 - i) of frac and m = m >> 1.
3. log2 = (ip << 32) | frac; x = (16 << 32) - log2; ub(q) = (x * 1000000 + 2^31) >> 32.

Test values: ub(1) = 16000000, ub(32768) = 1000000, ub(65536) = 0 (defined for completeness; a coded
symbol never has q = 65536, since every other entry of its row is at least 1), ub(43481) = 591903,
ub(46819) = 485194. The last two are one micro-bit above the correctly rounded
-log2(q/65536) x 10^6 (591902, 485193); every other q agrees, so each symbol is within
0.501 ub of the exact value. The rule, not correct rounding, is the definition. Bits = ub / 10^6.

## 9. Envelope (frozen with the profile, coder_envelope)

Overhead = coded bits (whole file, section 4) minus ideal bits. N = symbols in
the unit. Measured with `make turing-exp001-envelope` on dev seeds 1 to 7 only
(seeds 8 to 10 untouched), profile digest all zeros (the numbers do not depend
on it), three models: uniform K=9 (`83c04b...`), TY-2 baseline (`59ae93...`),
TY-2 candidate (`64a57b...`). Units: 21 whole files (N = 1,945,549 to 2,172,776)
and 3,894 crumbs (1,298 per model, N = 403 to 20,004), each crumb coded as its
own file with its own 56-byte header.

Per-crumb coding (the S4 per-crumb envelope and sum checks) builds no new TPS1 file and no new
digest. For the crumb with symbols [lo, hi) the evaluator takes rows lo..hi-1 of the one parsed file
TPS1 (already checked, section 2) and symbols lo..hi-1 of the parsed TSY1, runs both coders on that
slice from a fresh coder state (the section 5 and 6 payloads, no header written), decodes the payload
with the same rows and compares it with the symbols, and counts the crumb as payload bytes plus 56,
the size of a section 4 header. A crumb header would bind the file TPS1 digest, which the file-level
BINDING check (S3) already covers. Per-crumb coded bytes are recomputed, not stored, and are reported
in ideal_lengths.json (crumbs list) and the envelope result (`tools/turing_cal_eval.c`, per-crumb loop).

Per file, overhead in bits (min / mean / max over 7 seeds):

| Model | Range | rANS |
|---|---|---|
| uniform | 1425 / 1479 / 1543 | -641 / -570 / -509 |
| baseline | 1341 / 1391 / 1433 | 473 / 477 / 482 |
| candidate | 1457 / 1528 / 1582 | 474 / 477 / 481 |

Per-symbol rate after removing the 448-bit header, (ovh - 448) / N:
range 4.5e-4 to 5.4e-4; rANS 1.1e-5 to 1.7e-5 on the TY-2 models and
-5.0e-4 to -4.9e-4 on uniform.

Per crumb, (ovh - 448) in bits (min / mean / max, all models): range
19.4 / 33.0 / 42.9; rANS 13.7 / 26.2 / 32.2.

Where the overhead comes from:
- Range: the truncation r = range >> 16 loses up to 2^-8 of the interval per
  symbol in the worst case (range at 2^24) and about 5e-4 bits per symbol on
  this data; flush adds up to 40 bits.
- rANS: rounding of x / f moves each step slightly up or down. The effect is
  signed and favours low-index symbols; the uniform model on this data (skewed
  toward low symbols) codes about 5e-4 bits per symbol BELOW ideal. The 4-byte
  final state adds 32 bits. So the envelope must be two-sided.

**The envelope (one two-sided band for both coders, any unit; the profile value):**

    | overhead - 448 | <= 64 + 1.0e-3 x N   bits

- a = 448 bits header (exact, counted) plus 64 bits constant margin
  (observed per-crumb constant part at most 43 bits).
- b = 1.0e-3 bits per symbol, about 1.9x the largest observed magnitude
  (5.4e-4). Empirical for K = 9 and these models, not proven: adversarial
  sequences can reach about 5.6e-3 (range, interval pinned near 2^24) and 2.8e-3
  (rANS, f near M/2 with x near L) bits per symbol.
- Every one of the 21 files and 3,894 crumbs fits. Closest per-crumb approach:
  32 bits inside the bound (N = 410). Closest per-file: 973 bits inside.
- Observed maxima to record next to the bound: range file 1582 bits, rANS file
  482 bits, rANS file minimum -641 bits; crumb max 491 (range) and 480 (rANS)
  bits including the header.

A coder whose overhead leaves this band on any unit of a frozen run fails S4 for
that unit. There is no alternative bound: the single two-sided band above is the
only envelope (a one-sided range bound was considered and not adopted).

Measured on all seven candidates (CAL-0 review Q4; `make turing-exp001-envelope`,
`calibration/scripts/envelope_summary.sh`): each candidate over the 7 dev files of
seeds 1-7 and their 1,298 crumbs, 1,305 units per coder. Largest |overhead - 448|
in bits over all units, and violations:

| Candidate | Range | rANS | Violations |
|---|---:|---:|---:|
| B0_uniform | 1094.6 | 1089.4 | 0 |
| B1_order0 | 953.5 | 32.7 | 0 |
| B2_order1 | 984.7 | 33.5 | 0 |
| B3_heuristic | 1217.8 | 790.2 | 0 |
| M_candidate | 1133.8 | 33.4 | 0 |
| M_mem | 1134.5 | 569.5 | 0 |
| M_mem_seed1 | 547.8 | 71.2 | 0 |

The closest unit of every candidate and coder is about 32 bits inside the bound.
The dev models were fit in-sample on seeds 1-7; the numbers measure the coders on
these streams, not model quality.

Throughput on the Spark (single thread, -O2, millions of symbols per second,
whole dev files): range encode 95 to 104, decode 137 to 143; rANS encode 103 to
120, decode 126 to 131.

## 10. Refusal codes

| Code | Name | Meaning |
|---|---|---|
| -101 | ARG | bad argument |
| -102 | IO | file or allocation failure |
| -103 | FORMAT | bad magic, version, reserved field, size, K or qbits |
| -104 | DIGEST | trailer digest does not match |
| -105 | NORM | row sum or norm_sum is not 65536 |
| -106 | ZERO | a probability entry is 0 |
| -107 | PROFILE | profile digest differs from expected |
| -108 | MODEL | model digest differs from expected |
| -109 | DATASET | dataset digest differs (expected, or TPS1 vs TSY1) |
| -110 | COUNT | count mismatch |
| -111 | HEADER | coded header missing or altered |
| -112 | BINDING | coded file bound to another TPS1 |
| -113 | TRUNC | bitstream too short |
| -114 | TRAIL | unconsumed bytes after the last symbol |
| -115 | CORRUPT | decoder state invalid / termination check failed |
| -116 | SYMBOL | symbol outside 0..K-1, or decoded symbols differ |
| -117 | INDEX | observation_index is not the record position |

Every refusal also returns a one-line reason.

## 11. CLI (`turing-coder`)

Exit 0 = accepted, 2 = refused (reason on stderr), 1 = usage.

- `pstream <model.tym | uniform:K> <profile_hex> <trace.ctr> <out.tps> <out.tsy>`
- `encode <range|rans> <in.tps> <in.tsy> <out.coded>`
- `decode <range|rans> <in.tps> <in.coded> <out.tsy>`
- `verify <range|rans> <in.tps> <in.tsy> <in.coded>`
- encode, decode and verify accept `--profile HEX`, `--model HEX`, `--dataset HEX`
  and refuse unless the TPS1 header carries those digests
- `envelope <profile_hex> <out.csv> <trace.ctr> <model...>` (CSV columns: unit,
  model, index, n, ideal_ub, range_bytes, rans_bytes; bytes include the header)
