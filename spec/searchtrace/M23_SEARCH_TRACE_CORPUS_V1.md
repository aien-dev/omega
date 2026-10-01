# M23 search-trace corpus v1 (G1)

Status: corpus half of M23 SEARCH_GUIDE_TRAINING. The training half waits on M22.
Code: `src/searchtrace/`, hooks in `src/omega_synthesis.c` and `src/omega_realize_synth.c`.
Gate: `tests/searchtrace/run_m23_corpus_gate.sh`. Make: `make searchtrace test-searchtrace` (`mk/searchtrace.mk`).

## What it records

For every candidate the program search (`omega_synthesize`) touches: parent and child
candidate, the transform applied, candidate features, the verifier result, the
realization cost and the prune reason. Replaying the frozen task set regenerates the
corpus byte for byte.

## Hooks (off by default)

`src/searchtrace/st_hook.h` defines one function-pointer type `StHookFn` shared by both sites,
following the rx_world recorder pattern (omega#114): at most one recorder per site, installed
with `omega_synth_set_trace_hook` / `omega_realize_set_trace_hook`, removed with the matching
clear call and the same `ctx`. The hook is per thread (`_Thread_local`). With no hook installed
each emission point is one null test; search order, pruning, results and stats are unchanged
(`searchtrace hookcheck`, and the M9/M14 gates run on omegatool built with the hooks compiled in).
The recorder only observes: const pointers, valid during the call, no veto.

- Synthesis site: one `ST_EV_SYNTH_CANDIDATE` event per candidate, including candidates
  refused by composition (type), cost cap (cost), observational equivalence (equiv), and
  the single global `max_candidates` cutoff (budget, last event of the task).
- Realization site: `omega_synthesize_realization` is the unchanged body wrapped once;
  the hook receives the finished `RealizationSynthesisResult` and return code.

The corpus recorder realizes every evaluated candidate (prune `none`) for the fixed DGX Spark
machine graph (`omega_machine_build_dgx_spark`) through `omega_synthesize_realization`, and takes
the cost from the realize hook. `rcycles` is the machine model's estimate, not a measured time.

## Task-set format (`OMEGA-SEARCHTRACE-TASKSET v1`)

ASCII, LF only, no CR/NUL, no trailing spaces, ends with a newline.

    OMEGA-SEARCHTRACE-TASKSET v1
    # comment lines (start with #) are allowed and covered by the digest
    task <name> depth=<1..3> max_cost=<1..64> max_candidates=<1..100000> dedup=<0|1> pairs=<x>:<y>[,<x>:<y>]{0,15}
    end

Names `[A-Za-z0-9_.-]{1,60}`, unique. Numbers are canonical unsigned decimal (no sign, no leading
zero), u64. Fields in exactly this order. Up to 64 tasks. Nothing after `end`. The task-set
digest is SHA-256 of the file bytes. The frozen set is `tests/searchtrace/frozen_taskset_v1.txt`;
it is never edited, a changed set is a new file and version.

## Corpus format (`OMEGA-SEARCHTRACE v1`)

Same byte rules. Lines, in order:

    OMEGA-SEARCHTRACE v1
    taskset <sha256 of task-set file>
    machine <machine graph id> bank=<primitive count>
    task <t> name=<n> task_id=<hex64> depth=<d> max_cost=<c> max_candidates=<m> dedup=<0|1> examples=<k>
    step <t> <seq> d=<1..3> parent=<seq|-> prim=<j> pname=<name|-> transform=<op>:<imm> child=<hex64|-> nsteps=<n|-> insn=<n|-> body=<op:imm[/op:imm]*|-> sig=<hex64|-> prune=<none|type|cost|equiv|budget> verdict=<none|reject|solved|verify_fail> erc=<int> vrc=<int> rrc=<int|-> rbytes=<n|-> rcycles=<n|-> rchecked=<n|->
    ...
    result <t> solved=<0|1> solution=<hex64|-> generated=<n> pruned_type=<n> pruned_equiv=<n> failed=<n> solutions=<n> steps=<n>
    ... (task / step* / result per task)
    end tasks=<n> steps=<total> digest=<sha256 of every byte before the end line>

Field meaning:

| field | meaning |
|---|---|
| `seq` | step number within the task, from 0, no gaps |
| `parent` | `seq` of the kept candidate this one extends (depth 2 and 3); `-` at depth 1 |
| `prim`, `pname`, `transform` | bank primitive applied: index, name, opcode (2 hex) and immediate (hex) |
| `child` | program id (v2) of the candidate; `-` when composition was refused or the budget cut the search |
| `nsteps`, `insn`, `body` | features: body length, instruction-count cost, canonical body steps (innermost first) |
| `sig` | observational-equivalence signature (SHA-256 of outputs on the 6 probe inputs) when computed |
| `prune` | why the candidate left the search; `none` = it was evaluated on the task |
| `verdict`, `erc`, `vrc` | task evaluation outcome and return codes of `omega_task_evaluate_candidate` and `omega_program_verify` |
| `rrc`, `rbytes`, `rcycles`, `rchecked` | realization for the Spark machine graph: return code, code bytes, model cycle estimate, differential inputs checked |

Hex is lowercase. Integers are canonical decimal (`-` sign only before nonzero).

## Refusal rules (`st_corpus_verify`)

Refused: wrong magic or version; any field missing, extra, reordered or out of grammar; uppercase
hex; CR or NUL; empty line; trailing space; task or step index out of order or with gaps; a parent
that is not an earlier step; prune/verdict/child combinations that the search cannot produce; any
step after a budget cutoff; result step count different from the steps seen (partial task); a
missing `end` line or missing final newline (partial corpus); end counts or digest mismatch;
bytes after `end`; with a task set given, a different task-set digest, task count or name.

## Determinism

Everything recorded is a function of the task set, the primitive bank and the code: no
timestamps, addresses, timing or host randomness. The corpus differs only if code changes
(new corpus digest) or, for `rrc`/`rchecked`, on a host where native differential execution is
unavailable (non-AArch64 returns -3). The receipt records the host architecture.

## Receipt

`run_m23_corpus_gate.sh` refuses a dirty tree, then writes
`evidence/M23/corpus/corpus-<end digest>.txt` and
`evidence/M23/receipts/m23-corpus-<sha256 of receipt>.json` with the exact commit. Existing
evidence is never replaced; an existing file must hold identical bytes.
