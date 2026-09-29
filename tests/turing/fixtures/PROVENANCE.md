# CTR1 fixture provenance

Two byte ranges cut unchanged (`dd bs=247`) from a **fit** seed, never from the held-out seeds 8-10:

| file | source | first record | records | crumbs (ordinal in source) | SHA-256 |
|---|---|---|---|---|---|
| ctr1_seed1_crumbs05-07.ctr | exp-20260927-rep10/seed-1/control/trace.ctr | 60910 | 1418 | 5, 6, 7 | 777193633a4ff8092659024f18194bd111da5129c6e8bc3f04a292c138a732cc |
| ctr1_seed1_crumbs10-12.ctr | same | 102329 | 1330 | 10, 11, 12 | c01b91468d5b6cfb4953166d6f161cee2dceeb174675d0f7d90418b2b8967ccc |

Source file SHA-256 `b81a08a9ad3285abc8161206179763885a47fa34d08e28a1b044c470ea0fdc53` (line 1 of
evidence/TURING_YIELD/trace_manifest_rep10_control.sha256). Each range starts at event_index 0 and ends at a crumb
boundary, so it holds whole crumbs. The BLAKE3 chain digest in each record continues from the record before the range,
so the chain does not verify from zero on these slices (the C reader never checks it). Used by `make test-turing-yield`,
which cannot see the 21 GB corpus.
