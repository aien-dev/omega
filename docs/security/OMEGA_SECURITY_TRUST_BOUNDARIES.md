# Omega security trust boundaries

SECURITY-0. Baseline `c0edef0337e7d27150eb7274ff13bbb10cba3021`.

The decision is OMEGA-0001. This file names who may change authority, who may only look, and which boundaries exist only inside one process.

## 1. Trusted boundary

These components are the security nucleus. A fault here changes what every other component is allowed to do.

| Boundary | Where it lives | What it may do | What it is refused |
|---|---|---|---|
| Capability root | Linux oracle: `rx_caproot.c`, sealed memfd, separate process, `SOCK_CLOEXEC` socket. Native view: `aienos_cap.h` linked to `aienos` `d39dd5bc3deb1a24e26a5059477a1342c110c71b`, table in `calloc` memory. | Mint, attenuate, revoke, reclaim, move the clock and epoch, hold the promotion right. | The world is given the read-only view or the Linux read-only mapping. `RxCapRoot` has no office token and no control socket. |
| Reference check | `rx_caproot_validate`, `aienos_cap_validate`, `rx_world_validate_cap` | Accept a reference only when id, 64-bit generation, subject, resource, rights, epoch, lease, and ancestor chain all match. | Amplification, delegation of privileged rights (mint, revoke, reclaim, epoch, clock, promote), generation wrap. |
| Promotion | `rx_gen_promote` | Flip one lineage after a different subject presents exactly `RX_GEN_RIGHT_PROMOTE` (`0x200`) on resource `0x905`, proofs pass, and observed object generations still match. | `request->subject == candidate proposer`. The candidate record cannot authorize itself. |
| World publication | `run_one`, `stage_mutations`, `commit_writes` in `rx_world.c` | Publish fields inside the declared write set after a second capability check and a generation check. Append a crumb. | Stale object generation, write outside the set, capability revoked during the run. |
| AEGIS install | `rx_aegis.c` `root.install` | Mint into a client slot when the decision was last written by that client's `aegis.decide`, the request was last written by the client, the decision echoes the request, and the grant is inside the client domain and rights ceiling. | Privileged rights. A forged decision. A request written by someone else. A grant outside the domain, whatever the policy table said. |
| Effect name | `EffectPayload` version 2, 178 bytes, big-endian, 64-bit generation. `visor_effect_request_build`. | Describe an effect and refuse a digest that does not match the stored id. | Set `authorized`. The Visor links neither the policy evaluator nor the capability root. |
| Evidence | `RxCrumb`, generation blobs (`evidence`, `model`, `realization`, `config`, `provenance`, `objects`), receipt digests. | Record what was checked and what was published. | Authorize the next run. A crumb is not a capability. |

`RxAegisFaculty` holds `AienosCapAdmin *`. That handle is the native admin for the faculty that installs slots. Cognition and ordinary reactions are not given it. On this host reference the separation is which pointer the program passes, not a separate address space. The R8 note in `spec/r8-aegis-resident.md` already says that.

## 2. Untrusted boundary

These may propose, compute, or parse. They commit only by publishing through the world, and only with a reference the root still accepts.

| Component | On main | Authority it does not receive |
|---|---|---|
| Reaction body (`RxFn`) | Runs with the world lock dropped. Its proposals are filtered on return. | Mint, the office token, objects outside its write set. |
| Resident Omega candidate code | Unverified bytes run in a forked child (`sandbox_run`). Passed bytes are mapped executable in the parent and run there. | The child does not inherit the Linux control socket (`SOCK_CLOEXEC`). The native table is process memory, so the child's writes do not change the parent's table. The parent run still shares the process with the world. |
| Visor | Viewport over one `OmegaGraph`. Session tables hold program and realization copies for display. | Mint, publish, promote, capability checks. `authorized` is fixed false. |
| Model / synthesis | `omega.synthesize` publishes a candidate object. Selection is a later reaction. R13 can point production at a serve record written by the promotion path. | Self-promotion. `is_verified` is required before a library insert and is still not a right. |
| ARGUS | Compile-time. `RX_ARGUS` defaults to 0 in `rx_argus.h`. The header says it copies decisions and never re-validates. | Grant, revoke, mint. |
| Parser and canonical decoder | Refuse a legacy effect (`OMEGA_CANON_ERR_EFFECT_V1`) and a version/kind mismatch. | Turn a decoded object into a right. |
| Accelerator worker | Two stacks exist. `rx_resident_gpu` claims through the resident world. `OmegaAcceleratorWorld` has its own epoch, slot generation, and `OmegaHandle`. | An `OmegaHandle` is not an `RxCapRef`. A GPU virtual address stays in the M19 registry and is not an object id. |
| Python and third-party packages | No Python runtime under `src/`. `tools/qualify_visor.py` is a host qualification script. | No envelope, no ambient home, no inherited credentials, because the runtime is not there. |
| `omega_program_exec` and the tool binary | In-process native execution of a realized program on the tool path. | This path is not the resident world and does not consult `RxCapRoot`. |

## 3. Authority flow

```text
human or external publication
    -> request object, written by the client subject
    -> aegis.decide (policy table only)
    -> root.install (origin, domain, ceiling, then native mint)
    -> slot object holds {cap id, generation}
    -> reaction becomes ready because the slot changed
    -> validate_caps (subject, resource, rights, generation, chain)
    -> body runs, or the graphics seat is claimed
    -> validate_caps again
    -> atomic field publish
    -> crumb
    -> dependents wake
```

A reaction that already holds a live reference does not wake AEGIS. The fast path is a capability check, not a skipped check.

An effect that leaves the machine is not in that diagram as an execution step. Omega stops at a typed effect object. AEGIS and the Effect Broker are outside this repository. See OMEGA-0001.

## 4. State flow

```text
candidate bytes
    -> identity hash
    -> V0 structural check, and for resident matvec a forked differential
    -> verdict object
    -> measure object
    -> selection object
    -> optional serve record (R13), which production reads
    -> promotion of a generation only with the promotion right
    -> active lineage id
```

GPU completion on the resident seat is a publication attempt. It is not, by itself, the new generation. `OmegaAcceleratorWorld` drain updates that registry's dispatch accounting. It does not advance `RxGenStore`.

## 5. What shares an address space today

| Pair | Shared process on the host reference? |
|---|---|
| Reaction body and `RxWorld` | Yes. The lock is dropped around `fn`. |
| Verified matvec and `RxWorld` | Yes, after the verdict. |
| Unverified matvec differential and the Linux mint socket | No. Fork plus `SOCK_CLOEXEC`. |
| Unverified matvec and the native capability bytes | The child gets a copy. Parent mutations and child mutations diverge. |
| `RxAegisFaculty` admin and the world | Yes, when the faculty is created in the process. The world struct stores `RxCapRoot *` or a view, not the admin. |
| Linux mint process and the world | No. Read-only sealed memfd and a socket. |
| Visor and the capability root | No. The effect-request lane does not link them. |
| M19 accelerator world and `RxWorld` | Separate registries in one program if both are linked. They do not share object ids. |

## 6. Boundary the envelope is allowed to sit on

The envelope view is computed in the trusted publication path, from the reaction descriptor, the capability view, the verdict, and the resource need. The reaction body can read the view. It cannot swap the view for a wider one after `validate_caps` has passed, because publication checks the descriptor and the root again, not a copy the body returns.

A safety class is an input to that computation. It is not one of the rows in the trusted table.
