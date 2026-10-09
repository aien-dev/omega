# BRN-10 Brownian discovery results (Wave 1)

**BRN-10 verdict: PASS** (profile v1.5, attempt 2): the sealed protocol completed with no applicable stop condition, and all 12 primary cells received an outcome. A PASS says the protocol ran cleanly and every cell was classified; it is not a discovery claim.
**Attempt 1 (profile v1.4): FAIL**, kept permanently. Its producer session was stopped by an over-broad transcript scan before any freeze or seal; profile v1.5 corrected the scan and allowed exactly one new producer session.

Prohibited sentence (Section 21): this result does not say that AIEN discovered Brownian motion or any hidden law. A positive T grants nothing beyond its own sentence.

## What ran
- A producer (Claude Code 2.1.295, model claude-opus-5-5) worked in an isolated sandbox with only the public packet, the BRW-DL language spec, development worlds and the interpreter, and wrote one BRW-DL program. Web tools were denied; every tool call was scanned and audited.
- The evaluator froze the program (discovery mode), sealed the 12 primary cells once, scored 13 comparisons, verified all 13, and replayed them twice from stored files and once with regenerated worlds: all identical.
- Pre-seal audit by an independent Opus 5.5 session: GO.

## Outcomes
T in bits; positive means the candidate's total description (program plus data) is shorter than the baseline's.

| Cell | Baseline | T (bits) | interval | outcome |
|---|---|---|---|---|
| B0-irregular | IID_NORMAL | 384992.6 | [338832.6, 436125.8] | found structure |
| B0-regular | IID_NORMAL | 352714.2 | [316335.3, 392467.1] | found structure |
| B0-short | IID_NORMAL | 16458.6 | [10730.7, 23784.4] | found structure |
| B1-0 | DIFFUSION | -3172.6 | [-3200.5, -3141.9] | no additional structure |
| B1-1 | DIFFUSION | -3163.9 | [-3201.1, -3125.6] | no additional structure |
| B1-3 | DIFFUSION | 894 | [636.4, 1156] | found structure |
| B1-4n | DIFFUSION | 2411 | [2123.7, 2705.2] | found structure |
| B2-clear | DIFFUSION_DRIFT | 2280.4 | [2184.4, 2373.8] | found structure |
| B2-emerging | DIFFUSION_DRIFT | 12877.6 | [12282.8, 13366.6] | found structure |
| B2-fast | DIFFUSION_DRIFT | 18931.2 | [18673.1, 19188.5] | found structure |
| B2-noise | DIFFUSION_DRIFT (descriptive only) | -455.3 | [-645.9, -258.8] | no additional structure |
| B2-noise | DIFFUSION (A4.2 pair; gives the outcome) | -3279.6 | [-3364.2, -3194.9] | no additional structure |
| B2-weak | DIFFUSION_DRIFT | -2085.2 | [-2307.3, -1883.8] | no additional structure |

Found structure on 8 cells (B0-regular, B0-irregular, B0-short, B1-3, B1-4n, B2-clear, B2-fast, B2-emerging); no additional structure on 4 (B1-0, B1-1, B2-noise, B2-weak). No cell carries a fail flag, a floor hit or a leak flag.

## Claim ceiling and limitations
- EXP-002D and H4 remain INCOMPLETE; this PASS does not lift them. EXP-003 (Wave 2) is not started.
- Status note 2026-10-09: EXP-002D later reached PASS, scoped to scorer 2564f57, see docs/turing/TURING_SCIENTIFIC_QUALIFICATION_STATE.md. H4 is still INCOMPLETE. The line above is kept as written at the time.
- The pre-seal audit was done by the same model family as the producer.
- Stability and one-byte lengthening confirmations (stops 6 and 8) are carried from the v1.3 protocol checks, not run on this candidate.
- The producer sandbox shared the host network namespace; network control was by tool restrictions, no network client in the sandbox, and the transcript scan.
- Discovery run: certification-only items were not applied.

## Verify it yourself
The committed fingerprints are in docs/brownian/PROFILE_COMMITMENT.txt (v1.5, published before the run; history in PROFILE_COMMITMENT_HISTORY.txt). The files they cover, the candidate and results.json are in evidence/BROWNIAN/BRN-10/ (see its README).
