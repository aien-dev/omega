# BRN-10 publication (after the seal)

Published after the one sealed BRN-10 attempt, as the protocol requires (evaluator code stays private until the seal). Results: docs/brownian/BROWNIAN_DISCOVERY_RESULTS.md.

Check the public commitment (docs/brownian/PROFILE_COMMITMENT.txt, published before the producer ran):

| Commitment line | File here | sha256 |
|---|---|---|
| spec | brownian-profile-v1.5.md | f929f2a69435b346604a407e42937e07821df935922a6a3e375cb6ce49da8b03 |
| evaluator-tree | brownian-evaluator-tree-cefe889.tar | fc6fcb40d6bce718b88f59c7b9664fea2e611ce65eebe72e7d191e02d30da12d |
| slice-candidate | packet.txt | 4383e7db7804088d323ee964d625fc82e9571f9baf0dcabbde8afba1da19e5c7 |
| slice-brwdl | brw_dl_spec.txt | 86bf360cfa208d91338cf22faa0f6022c93dfe2ee1a574e2ee27a800ec6a557f |
| slice-private | brw_thr_v1.txt | fca680aa1a4104d65b1d7fa9766cbc3fe1eeb04956a33fff5e3b446a4679194a |

The evaluator tree is the exact output of `git archive --format=tar cefe8894e34930d6c22d2e4ed870a9ac5c7a1b48 brownian/src brownian/tests brownian/Makefile` in the private evaluator repository (the archive embeds the commit id). Unpack with `tar xf`.

Also here: candidate.brwdl (the producer's program, sha256 09e4534670e38b4c864e851c7db5dca9e60323a35f455a499effce1c032e3c82), candidate.canonical.brwdl.bin (the assembled artifact that was frozen and scored, sha256 52fb1d23e0672a92b0b469af6d6f748d964c0b8137dca44a9419720eeb5eaa78), results.json (printed by the evaluator from its records, sha256 4549c0e2351ca659503e9d3b59c5a69325cf70852714f5220292842786ac2596).

Not published: the producer transcript, the sealed world files and the scoring files (kept in the private evaluator records, listed by sha256 there).
