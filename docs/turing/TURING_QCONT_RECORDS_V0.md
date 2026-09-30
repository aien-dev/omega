# TURING companion records for continuous-value evidence (v0)

Four record domains and the sidecar manifest formats they bind. Code:
`src/turing/ty_qrecord.{h,c}`. Tests: `make test-turing-qrecord`. These are
companion records beside the qint.v1 scorer (`ty_qcont`) and the PRD1 reader;
no existing `turing.*` record, domain or golden changes.

## Digest recipe

`SHA-256(domain || 0x00 || OMG0 bytes)`, with the OMG0 bytes from
`omega_canonical_encode` and the hash from `src/sha256.c`, the same recipe as
`ty_record.c`. A record digest is never an Omega semantic id (OSC-0B). Positive
T grants nothing.

## Conventions

- One OMG0 object per record; `record` is the first attribute and holds the
  domain name without `.v0` (for example `turing.qgain`). At most 32 attributes.
- Every key is shorter than 64 bytes and every value is at most 512 bytes. A
  longer value is refused (`TY_E_FORMAT`); nothing is truncated.
- The canonical encoder sorts attributes by key, so insertion order does not
  change the digest.
- Renderings: `dec` decimal with no padding; `hex` 64 lowercase hex characters;
  `hex40` 40 lowercase hex characters; `text` printable ASCII, non-empty;
  `list` tokens separated by exactly one space. An absent digest is 64 `0`
  characters; an absent number inside a list is `-1`.
- The library holds no profile constants. Every descriptive value (names,
  versions, rule ids, counts, units) is supplied by the caller and carried
  verbatim.
- Builders refuse a malformed record. Verifiers rebuild the digest from the
  fields and compare (`TY_E_DIGEST` on mismatch). `tyqr_file_verify` does the
  same for a stored record file.
- Writers are write-once: `<dir>/<record>.<digest hex>.omg0`, opened with
  `O_CREAT|O_EXCL`, mode 0444, holding the canonical bytes. A second write of
  the same record returns `TYQR_E_EXISTS` and leaves the file alone.

## turing.qprofile.v0 (constraint, 23 attributes)

`record`, `profile`, `spec` (hex), `packet` (hex), `protocol`, `quantizer`,
`delta_log2` (dec, signed), `edge_rule`, `sd_min_log2` (dec, signed),
`floor_rule`, `families` (list), `kmax`, `n_points`, `split` (dec),
`cells` (hex, digest of the cell-table sidecar), `rider`, `lm_rule`,
`baselines` (list), `thresholds` (hex, digest of the thresholds sidecar),
`thresholds_format`, `gen_version`, `bootstrap_b` (dec), `unit`.

## turing.qworld.v0 (evidence, 14 attributes)

`record`, `qprofile` (hex), `mode` (`dev`, `certification`, `discovery`),
`parent_kind` (`dev`, `sealed`), `qfreeze` (hex), `cell`, `block` (dec),
`label`, `gen_version`, `generator` (hex), `rep_first`, `rep_count` (dec),
`traj_manifest` (hex), `traj_format`.

Refused: `mode` and `parent_kind` that disagree (`dev` if and only if `dev`);
`qfreeze` that is not all zero exactly when `parent_kind` is `dev`; a `cell`
containing a space.

## turing.qfreeze.v0 (evidence, 21 fixed attributes plus lineage, at most 29)

`record`, `mode` (`certification`, `discovery`), `artifact` (hex),
`artifact_kind`, `artifact_bin` (hex), `qprofile` (hex), `dev_manifest` (hex),
`b2_emerging_label` (`POSITIVE`, `NEGATIVE`, `AMBIGUOUS`),
`bayes_plugin_check` (`AGREE`, `STOP`), `diag_fw_rate_ppm` (list),
`lengthening_test`, `stability_test` (`PASS`, `FAIL`), `calibration_extra`,
`run_commit` (hex40), `tree_dirty` (`0`, `1`), `base_commits` (list),
`producer_tool`, `producer_model`, `prompt_digest` (hex),
`transcript_digest` (hex), `n_lineage` (0..8), then `lineage.00` to
`lineage.07` (hex, only indexes below `n_lineage`).

Refused: a ninth predecessor; `artifact_bin` present in certification or absent
in discovery; in certification, producer fields other than `none` or non-zero
prompt or transcript digests; in discovery, a zero prompt or transcript digest.

## turing.qgain.v0 (evidence, 29 attributes)

`record`, `qprofile`, `qworld`, `qfreeze` (hex), `mode`, `cell`,
`gen_version`, `baseline` (`<opcode dec> <artifact hex>`), `candidate` (hex),
`candidate_opcode` (dec), `prd_b`, `prd_m` (hex), `official` (`0`, `1`),
`ladder_item`, `lengths` (six signed integers, micro-bit units: `L(B) L(D|B)
L(M) L(D|M) DL(B,D) DL(M,D)`), `t_ub`, `t_lo_ub`, `t_hi_ub`,
`numeric_bound_ub`, `mc_err_ub` (dec, signed), `counts` (four unsigned
integers), `floor_hits`, `protocol_failures` (dec), `leak_flag` (`0`, `1`,
`INCONCLUSIVE`), `coverage` (three signed integers, `-1` where absent),
`diag_warnings` (list or `-`), `classification` (`POSITIVE`, `NEGATIVE`,
`AMBIGUOUS`, `WEAK`, `STOPPED`), `status` (`PASS`, `FAIL_*`, `STOPPED`), `unit`.

Refused: `baseline` opcode outside 1..7; `candidate_opcode` other than 1..7 in
certification or 255 in discovery; `ladder_item` that is not `-` exactly when
`official` is 1; `t_ub` that is not `lengths[4] - lengths[5]` (checked with
`ty_sub`, so int64 overflow is a refusal); `t_lo_ub` above `t_hi_ub`;
`protocol_failures` other than 0 unless `status` is `STOPPED`.

### Binding

`tyqr_gain_bind` refuses (`TY_E_PROFILE`) a gain unless the gain, its world and
its freeze all name the same profile digest; the gain names that world and that
freeze; the world is sealed by that freeze; `mode`, `cell` and `gen_version`
match the world; the freeze's `mode` matches; and `candidate` equals the
freeze's `artifact`. All three records are also verified against their digests
first.

## Sidecar manifests

ASCII, one record per line, fields separated by one space, every line ends with
LF including the last, no other bytes. The sidecar digest is SHA-256 of the file
bytes (`tyqr_sidecar_digest`) and is the value stored in the record.

| format | line | order |
|---|---|---|
| trajectory manifest, PRD1 manifest | `<rep dec> <digest hex>` | rep strictly ascending |
| development manifest | `<cell> <block dec> <digest hex>` | cell by byte order, then block, strictly ascending |
| cell table | `<cell> <field>=<value> ...` | cell by byte order, strictly ascending |
| thresholds | `<item_id> <field> <value>` | byte order of the whole line, strictly ascending |

The first three have build and parse functions (`tyqr_repman_*`,
`tyqr_devman_*`); decimals have no sign and no leading zero, digests are
lowercase hex. The cell table and thresholds have format checkers only
(`tyqr_celltab_check`, `tyqr_thrtab_check`); their values are supplied by the
caller.
