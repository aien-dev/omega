# TYM0 model description encoding (Turing-profile-v1.0)

This document is sufficient to write a TYM0 decoder and scorer without reading omega source. The reference
implementation is `src/turing/ty_model.c` (`ty_model_encode`, `ty_model_decode`, `ty_model_score`); where this
document and the code disagree, the run is void and the disagreement is reported (FAILURE_REPORTING.md).

L(M), the model cost under Turing-profile-v1.0, is the exact bit length of the TYM0 code defined here.

## 1. Bit order

The code is a bitstream written most significant bit first. A field of width w holding value v contributes its w
bits from bit w-1 down to bit 0. Bit i of the stream is bit (7 - i mod 8) of byte floor(i / 8). After the last
field the stream is padded with 0 bits to a whole byte. The file (`.tym`) is exactly these bytes, nothing before or
after.

## 2. Layout

| field | width (bits) | value / rule |
|---|---|---|
| magic | 32 | 0x54594D30 (ASCII `TYM0`) |
| version | 8 | 0 (only admissible value) |
| K | 8 | alphabet size; 9 under this profile (decoder accepts 2..16, the scorer requires 9) |
| mask | 8 | feature mask, bits defined in section 3; bits outside 0..7 refused |
| qbits | 8 | 16 (only admissible value) |
| keybits | 8 | must equal ceil(log2(S)) where S = key space of the mask (section 3); 0 when S = 1 |
| nrows | 32 | number of stored context rows |
| default row | 16 x (K-1) | entries q[0..K-2], each 1..65535 |
| row i, i = 0..nrows-1 | keybits + 16 x (K-1) | key (keybits bits), then entries q[0..K-2] |
| padding | 0..7 | zero bits |

The header (magic through nrows) is exactly **104 bits**. For every row (and the default row) the K-th entry is
not stored: q[K-1] = 65536 - (q[0] + ... + q[K-2]).

**L(M) = 104 + 16(K-1) + nrows x (keybits + 16(K-1)) bits.** With K = 9: L(M) = 232 + nrows x (keybits + 128).
Padding is not part of L(M).

## 3. Context key

Features are combined into one mixed-radix key, **in this fixed order, lowest digit first** (the order of the
code, not of the mask bit values):

| order | feature | mask bit | radix | digit value for event t |
|---|---|---|---|---|
| 1 | op | 1 | 16 | op feature: op_index 0..14 for EXPAND, 15 for SUBMIT |
| 2 | depth | 2 | 8 | min(depth, 7) |
| 3 | prev1 | 4 | K+1 | outcome of the previous event in the same crumb, or K at crumb start |
| 4 | prev2 | 8 | K+1 | outcome two events back in the crumb, or K |
| 5 | prev3 | 16 | K+1 | three back, or K |
| 6 | prev4 | 64 | K+1 | four back, or K |
| 7 | prev5 | 128 | K+1 | five back, or K |
| 8 | pos | 32 | 2^36 | (crumb ordinal << 20) OR min(event_index, 2^20 - 1) |

key = d1 + r1 x (d2 + r2 x (d3 + ...)) over the features present in the mask, in the order above. S = product of
the radices of present features; S > 2^62 is refused. keybits = 0 if S <= 1, else the bit length of S - 1.

- "Previous outcome" digits are kept per file and all reset to K when an event opens a crumb (the CTR1 crumb-start record),
  before the key of that event is computed. After the event is scored the history shifts: prev5 <- prev4 <- ... <-
  prev1 <- x_t.
- Crumb ordinal = number of crumb starts before this event in the file (the first crumb is 0), saturating at 65535.
- The outcome symbol x_t, op feature and depth come from each 247-byte CTR1 record exactly as in
  TURING_YIELD_PROFILE_V0.md sections 2-3 (reader `src/turing/ty_ctr1.c`, byte offsets in `ty_ctr1.h`).

## 4. Prediction

For event t with key k: if a stored row has key k, P(x) = row.q[x] / 65536; otherwise P(x) = default.q[x] / 65536.
Ideal code length of the event = -log2(q[x_t] / 65536) bits, computed in micro-bits exactly as `ty_ubits_q16`
(profile `ideal_codelength_method`).

## 5. Decoder refusals (all fail closed)

A conforming decoder refuses, and the scorer never scores, a code with any of:

1. magic != 0x54594D30, or fewer than 104 header bits;
2. version != 0;
3. qbits != 16;
4. mask with bits outside 0..7, K outside 2..16, S > 2^62, or keybits != ceil(log2(S));
5. file length in bytes != ceil(L(M) / 8) where L(M) is computed from the header;
6. any stored entry equal to 0, or stored entries of a row summing to >= 65536 (implied last entry < 1);
7. a row key >= S, or keys not strictly increasing;
8. any nonzero padding bit.

Turing-profile-v1.0 admits only K = 9: a code declaring another K is not a valid candidate under this profile, even though the generic decoder accepts it.

## 6. Why the fixed 104-bit header closes the metadata-hiding channel

A memorizer would like to store data somewhere it is not charged for. In TYM0 there is no such place:

- Every header field has a fixed width and is part of L(M). magic, version and qbits have exactly one admissible
  value; keybits is determined by the mask; K is fixed at 9 by the profile. The only free choices are the mask (8
  bits, one of 256 values) and nrows (32 bits), and both are paid for in L(M).
- nrows fixes the total length exactly: the decoder computes L(M) from the header and refuses a file whose byte
  count differs from ceil(L(M) / 8). Trailing bytes, a length field that lies, or an extra section cannot exist.
- Padding is at most 7 bits and must be zero, so it carries no information.
- Keys must be strictly increasing and inside the key space, so row order carries no information and a row cannot
  appear twice.
- There is no comment, name, version string, timestamp or extension field. The model's identity is its digest
  SHA-256('turing.ymodel.v0' || 0x00 || bytes), computed, not stored.
- The scorer runs only on the decoded table. Anything that did not survive decoding cannot influence a
  probability, and everything that survives decoding was counted in L(M).
- Coded data files follow the same rule in the other direction: every byte of every coded file counts
  (profile `coder_header_accounting`), so a coder cannot move data into an uncounted header either.

Consequence: the information content a model can carry is bounded by L(M), bit for bit. The position memorizers of
EXP-001 (mask 32) pay 164 bits per stored event, more than the 3.17 bits a uniform code spends on that event.

## 7. Test vectors

B0 (uniform, K = 9, mask 0, nrows 0), 29 bytes, L(M) = 232 bits:

```
54 59 4d 30 00 09 00 10 00 00 00 00 00 1c 72 1c 72 1c 72 1c 72 1c 72 1c 72 1c 72 1c 71
```

magic `TYM0`, version 0, K 9, mask 0, qbits 16, keybits 0, nrows 0, default row 7282 x 7, 7281 (symbol 7);
symbol 8 implied = 65536 - 7 x 7282 - 7281 = 7281. Model digest
`83c04bdfc10614febd634092355ef03997ed255fde5bdec9a80fe2d8964998ce`.

B3 (fixed heuristic, mask 1, keybits 4, nrows 16), 293 bytes, L(M) = 2344 bits, first 32 bytes:

```
54 59 4d 30 00 09 01 10 04 00 00 00 10 1c 72 1c 72 1c 72 1c 72 1c 72 1c 72 1c 72 1c 71 02 49 22
```

After the default row: key 0 in 4 bits (0000), then 9362 = 0x2492 seven times, then 1; the bytes `02 49 22 49 ...`
are that bit sequence shifted by 4. Model digest `e729818b8289263e6819b21fc55f944f9d5113e7cfc4419d0badcc27f589f2ae`.

Every EXP-001 candidate's file SHA-256, model digest, rows, keybits and L(M) are in
`calibration/experiments/EXP-001/candidate_manifest.json`.

## 8. Fit-time procedures are not part of the code

How rows were produced (KT add-1/2 smoothing, largest-remainder quantization `ty_quantize_kt`, MDL pruning, hand
setting, sharpening) is free and not scored. Only the table in the code is scored and paid for. A reimplementation
needs sections 1-5 only.
