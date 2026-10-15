# log.tsv columns 10 and 11 are redacted

Columns `check_r1` and `check_r2` keep only the reader stage of each
answer: `OK`, `-` (no round 2), `REFUSED omegese` or `REFUSED compiler`.
The full refusal lines quote identifiers from held-out generations, so they
stay in the run directory outside the repository (see `runs.txt`). Every
score, outcome and count in this directory was computed from the full log
before redaction and is unchanged. The futility check's `a1_rows_sha256`
refers to the full run log, not to this copy.
