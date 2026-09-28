# R8: AEGIS resident authority

ADR 0016 §41 and its 2026-09-28 scope note. Gate `R8_CONSTITUTIONAL_AEGIS_PASS`.

## Rule

Authority relationships live in the world. A reaction that already holds a
valid capability runs with no policy round trip. A reaction that needs new
authority is not ready. Its principal publishes a request, AEGIS decides, and
only the AIENOS root mints. Nothing is called and nothing waits.

"AEGIS" here is the verifier/policy faculty (ARCHITECTURE §2.5), not the
`aegis-runtime` program. The root is the native AIENOS capability authority
(aienos `native/capability/`, C).

## What runs

Code: `src/runtime/rx_aegis.{c,h}`. Engine additions in `rx_world.{c,h}`:
`cap_slotted` / `cap_slot`, `stamp_proposed`, `rx_world_crumb_origin`.
Test: `tests/runtime/rx_r8_aegis.c`, `make test-r8` (any host).

| Object | Written by | Read by |
|---|---|---|
| `request` (per client) | the client's own reaction | AEGIS, root |
| `approval` (per client) | a human, from outside | AEGIS |
| `decision` (per client) | `aegis.decide` | root, client |
| `slot[0..3]` (per client) | `root.install` | the client's reactions |

- **Capability slots.** A reaction can declare that capability need *i* reads
  its reference from a slot object (fields 0–1) at every check, instead of a
  fixed reference. The slot is also a trigger, so filling it wakes the
  reaction. A slot is not permission: the AIENOS view still validates
  whatever reference it holds.
- **`aegis.decide`** applies the policy table. It denies privileged rights,
  resources no rule covers, and rights beyond the rule. It caps the lease at
  the rule's maximum. It escalates when the rule requires a human, and then
  grants or denies on the human's `approval`. A release becomes `REVOKE`.
- **`root.install`** is the only holder of the AIENOS admin. It mints, or
  revokes, only when all of these hold:
  1. every decision field was last written by that client's `aegis.decide`.
     `stamp_proposed` makes each AEGIS commit the author of every field it
     names, so a value a forger left behind cannot keep the forger's name;
  2. every request field was last written by a reaction of the client's own
     subject;
  3. the decision echoes the request;
  4. the grant stays inside the client's domain and rights ceiling, with no
     privileged right.

  It keeps its own record of what it minted per slot. A repeated run never
  mints twice. It revokes only references it minted itself, never whatever
  sits in a slot.

This closes the migration map's open item "who may request a fresh mint"
for the reaction path. The requester is authenticated by who wrote the
request, and the policy decision by who wrote the decision.

## Results (DGX Spark, host processor)

| | |
|---|---|
| Fast path: 2000 actions under a live grant | 0 AEGIS activations, 0 root activations |
| One fast-path authority check | ~26 ns (AIENOS validate) |
| One in-process policy evaluation | ~3 ns (rule table) |
| One process spawn + reap (`/bin/true`), the floor of any shell-out policy call like `spark-aegis` | ~260–310 µs |
| Slow path, outside intent → work committed | ~44–130 µs |

A synchronous per-action policy path would make 2000 round trips for those
2000 actions. R8 made one decision, at grant time. Compared with a
shell-out, the saving per action is about four orders of magnitude.
Compared with an in-process policy call, the fast path is not cheaper per
check (26 ns vs 3 ns). What it removes is the round trip and the ordering
dependency, not the arithmetic.

## Attacks (all must be refused by the root)

- A policy mistake grants outside the client's domain: refused (`DOMAIN`),
  nothing minted.
- An outside writer with wrongly granted WRITE on the client's request asks
  in the client's name. AEGIS grants by the client's policy; the root
  refuses (`REQUEST_ORIGIN`).
- A non-AEGIS reaction with wrongly granted WRITE on the decision forges a
  grant: nothing minted. It can jam the client's decisions until its grant
  is revoked; after revocation the honest path works.
- An outside writer publishes a decision: refused (`DECISION_ORIGIN`).
- Slot stuffing: another subject's valid reference is written into the
  client's slot. The client stays blocked (subject mismatch). The root's
  next grant does not revoke the stuffed reference.
- Privileged rights, unruled resources, rights beyond the rule, a slot out
  of range, and a human rejection are all denied.
- A lease expires on the AIENOS clock and the reader stops. A renewal
  replaces the grant: the old one is revoked, the new one is minted.
- A release revokes through the root. The client stops; the world goes on.

Every AEGIS and root activation has a waking cause, and the crumb chain
verifies.

## Not claimed / limits

- The AIENOS authority runs as a host library inside this process, not as
  the AIENOS kernel. Keeping the admin handle away from other code is a
  property of how this program is built, not of an address-space boundary.
- The policy is a fixed rule table given at start. Changing policy at run
  time, and revoking grants because the policy changed, is not implemented.
- A principal wrongly given WRITE on AEGIS's decision object can jam that
  client's decisions (denial of service). It cannot cause a mint.
- The client learns of a denial by reading `decision`. There is no retry
  policy.
- No graphics-processor reaction uses slots yet (R12's seat binds its
  references at claim time).
