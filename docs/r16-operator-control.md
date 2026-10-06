# R16 G6: operator control of the production program (interface contract)

Status: CONTRACT, written before the code (lane ESTOP, CAND-3), then implemented. Two
points were corrected from the code: when the socket opens (section 2) and a store
refusal under a stop (section 6). The runtime control it wires is `spec/r16-operator-emergency-stop.md` (merged #300); this document says how an
operator outside the running production program reaches that control, and what the
program does with a request. It adds no authority scheme: every decision is made by the
existing world authority (`rx_world_emergency_stop` / `_resume`, `halt_authorize` in
`src/runtime/rx_world.c`).

Production program: `tests/runtime/rx_r13_living.c`, built without `AIEN_TEST_BUILD` as
`build/rx_r13_living_host` and `build/rx_r13_living_silicon`. It runs six modes, each in
its own world. Every statement below holds for each of those worlds.

## 1. Who the operator is and how its authority reaches the program

* One operator subject, `RX_OPERATOR_SUBJ` (70), not used by any faculty, reaction or
  composition subject (`rx_living.h` 61-64, `rx_omega.h` 21-22, `rx_aien.h` 31,
  `rx_aegis.h` 41-42, `rx_compose.h` 200+).
* **Credential.** At each world start, with the other production subjects and before
  enrollment closes (`rx_living_enroll_callers`, then `rx_world_bind_callers`), the
  program enrolls the operator with `rx_world_enroll_caller`: a fresh 32-byte random
  secret and a generation, issued by the runtime. The program keeps no copy after
  handing it over (below) and wipes its buffer.
* **Capability.** The program mints, through the native AIENOS authority it already
  starts (`aienos_cap_start`, the same `mint()` path every production grant uses), one
  capability for `RX_OPERATOR_SUBJ` on `RX_WORLD_RES_CONTROL` (0x906) with
  `RX_WORLD_RIGHT_HALT` (`RX_RIGHT_EPOCH`: privileged, never delegable). No other
  subject is minted anything on that resource; the program's authority sweep fails the
  run if any live capability on it belongs to another subject or carries another right.
* **Hand-over.** Both are written to the operator credential file
  `<state>/control/operator.cred` (format §3), created mode 0600 in a 0700 directory
  owned by the program's user, written to a temporary file, fsynced and renamed. It is
  the operator's copy; the world holds only the credential digest and the authority
  holds the capability. Anyone who can read the file is, by construction, the operator
  account (the program's own uid, or root).
* **No editable boolean grants authority.** The file carries a credential the world
  checks in constant time and a capability reference the authority validates; editing
  either makes it fail. Nothing in the file, the socket, the environment or the command
  line switches a check off.
* **A reaction subject can never control the world.** The world refuses any subject
  that a registered reaction runs under (`halt_authorize`, spec §2 point 2). The
  operator subject runs no reaction. The program's own reaction credentials never leave
  the program, so they cannot be presented from outside; that refusal is proven
  in-process by `make test-rx-emergency` (E1) and is not re-proven from outside.

## 2. The outside entry point

* An `AF_UNIX` stream socket `<state>/control/operator.sock`, created right after the
  world's credential, capability and halt directory exist (before its objects are
  built) and removed when the world is torn down. While the program sets the world
  up, a request waits (the client allows 60 s) and is answered once setup is done, so
  a stop never lands inside setup; a world restored stopped answers at once (it does
  no setup until resumed). Between worlds there is no socket: a client gets
  "connection refused" and the request does nothing.
* **One program per state directory.** At startup the program takes an exclusive
  `flock` on `<dir>/operator.lock` (opened `O_CLOEXEC|O_NOFOLLOW`, a regular file of its
  uid) and, when a world first uses it, on `<dir>/gen-<mode-key>/operator.lock`; it holds
  both for its whole life. A second program on the same state directory, or one whose
  state directory is the first one's generation directory, is refused (exit 2,
  `... is in use by another program (pid N)`) before it touches a socket or a
  credential. Each world also locks `control/` the same way while its entry point is
  open. The control directory is held open as a descriptor: the stale socket is removed,
  the socket bound (through `/proc/self/fd/<fd>/operator.sock`) and the credential
  written and renamed relative to that descriptor, never by a path looked up again.
  Close removes the socket and the credential only if they are still the files this
  instance created (same device and inode), then releases the lock.
* Two walls before the credential: the directory is 0700 and owned by the program's
  user (validated at startup, §5), and every accepted connection is checked with
  `SO_PEERCRED`: the peer uid must equal the program's effective uid, else the reply
  is `REFUSED reason=peer` and nothing is read further. Root can pass the directory but
  not the peer check.
* One request per connection, read with a 2 s receive and send timeout (if either
  cannot be set the connection is closed with no reply); at most 511 bytes up to
  the first newline. One reply line, then the connection is closed. Requests are served
  one at a time by one listener thread (`src/runtime/rx_operator.c`); it never holds the
  world lock across a request.
* Client: `build/rx_operator` (`tools/rx_operator.c`). It reads the credential file and
  sends one request: `rx_operator --control <state>/control <command> [reason]`, or
  `--cred <file>` to present another file (used by the tests for stale and forged
  credentials), or `--raw <line>` (malformed-request tests).

## 3. Formats

Credential file (text):

```
aien-operator-credential v1
world <mode-key> <program pid> <world start CLOCK_REALTIME ns>
socket <absolute socket path>
subject 70
cred <generation> <64 hex secret>
cap <cap_id> <cap generation>
```

Request (one line):

```
aien-operator v1 <command> subject=<u32> gen=<u64> secret=<64 hex> cap=<id>:<gen> deadline=<u64> [reason=<int>]
```

`deadline` is required: the `CLOCK_MONOTONIC` time in nanoseconds after which the client
has given up (the client sends now + 55 s and waits 60 s). A deadline more than 120 s
ahead is `BAD_REQUEST`. A request that waited for the world's setup past its deadline is
answered `EXPIRED` and not run, unless it is a `stop`: a late stop is fail-safe and still
runs; a late resume would restart a world nobody is watching.

`<command>` is `status`, `stop`, `resume`, `revoke-cap`, `revoke` or `shutdown`. Any other
token, a missing or repeated field, a secret that is not 64 hex digits, a number out of
range or extra bytes is `BAD_REQUEST`, decided before any world call.

Reply (one line): `aien-operator v1 <RESULT> key=value ...`. RESULT is one of `OK`,
`STOPPED_NOT_DURABLE` (the stop is in force in memory but its mark could not be written:
a restart would NOT come up stopped; carries `durable=<-errno> errno=<n> (<text>)`),
`ALREADY` (stop of a stopped world), `NOT_STOPPED` (resume or shutdown of a running world),
`EXPIRED` (the client deadline passed while the request waited; nothing done),
`REFUSED reason=peer|identity|authority`, `BAD_REQUEST`, `ERROR rc=<n>`.
A refusal says nothing about the world: no state, no counters.

Client exit status: 0 for `OK`, 3 for `STOPPED_NOT_DURABLE`, 1 for any other reply, 2 for
a usage error, no credential file, no world serving the socket, or a reply timeout that
could not be set.

## 4. Commands

Every command except `BAD_REQUEST` cases goes to the world authority first. `status`,
`revoke-cap`, `revoke` and `shutdown` are checked by `rx_world_operator_authorize` (the
same `halt_authorize` the stop and resume use, under the same locks, added to
`rx_world.c` so no second authority check exists). `stop` and `resume` are checked inside
`rx_world_emergency_stop` / `_resume` themselves.

| command | world call | reply on success |
|---|---|---|
| `status` | `rx_world_operator_authorize`, `rx_world_halt_status` | `OK state=running\|stopped restored= seq= durable= refused= cancelled= crumbs= reactions=` plus program counters `served= production_commits= promotion= inforce= active_generation= seat_claims= seat_halted=` |
| `stop [reason]` | `rx_world_emergency_stop` | `OK state=stopped seq= crumb= durable=1`; mark not written: `STOPPED_NOT_DURABLE ... errno=` (client exit 3); a second stop: `ALREADY` with no new crumb |
| `resume` | `rx_world_emergency_resume` | `OK state=running seq=`; a running world: `NOT_STOPPED` |
| `revoke-cap` | authorize, then `aienos_cap_revoke` of the presented control capability by the authority's office | `OK revoked=capability`; afterwards every request with that capability is `REFUSED reason=authority` |
| `revoke` | authorize, then `aienos_cap_revoke` (capability) and `rx_world_revoke_caller` (credential); the credential file is removed | `OK revoked=capability,credential`; afterwards `REFUSED reason=identity` |
| `shutdown` | authorize; only while stopped | `OK state=stopped shutdown=1`; the program ends the current mode, tears the world down with the stop still in force, writes no R13 receipt and exits 3. The durable mark stays. |

A revoked operator cannot be re-enrolled in the same world (enrollment is closed); the
world keeps running without operator control until the program starts the next world,
which issues a new credential and capability. Revoking while stopped leaves the world
stopped for the rest of that world's life; the durable mark makes the next start of that
mode come up stopped, and the next credential can resume it.

**Stale credentials.** A credential is valid for one world of one program start: every
world start enrolls a new random secret and mints a new capability. A credential from an
earlier world or an earlier program start fails the constant-time caller check
(`REFUSED reason=identity`).

## 5. State directory, durability and restart

* `--state-dir <dir>` names the program's durable state (default: a fresh 0700 scratch
  directory under `$TMPDIR`, as before, so `make test-r13-host` keeps running unchanged).
  Layout: `<dir>/control/` (credential file and socket) and one generation store per mode,
  `<dir>/gen-<mode-key>/` (`positive`, `no-aien`, `no-promotion`, `revoked-experiment`,
  `stale-gpu`, `failed-verification`).
* **Validated at startup, refuse if wrong.** `<dir>` and each subdirectory must be a real
  directory (opened `O_DIRECTORY|O_NOFOLLOW` and checked with `fstat` on that descriptor,
  so a symlink is refused and nothing can be swapped in between), owned by the effective uid, with
  no group or other permission bits (`mode & 077 == 0`). A missing subdirectory is
  created 0700. A path too long for a socket address is refused. On any refusal the
  program prints the reason and exits 2 before any world starts.
* **The durable halt directory is the generation store's directory**:
  `rx_world_set_halt_dir(w, <dir>/gen-<mode-key>)`, called right after the store is opened
  and bound and **before any object is created or any work is published**. A mark there
  (`OPERATOR_HALT`, sealed) makes the world start stopped (spec §4: a torn or unreadable
  mark also stops it). The program then serves operator requests on the socket and does
  nothing else until a resume (or `shutdown`); only then does it build the world's
  objects and run. So a restart while stopped restores the stop before it accepts any
  work, including its own setup.
* Resume keeps the sealed resumed record `OPERATOR_HALT.resumed.<seq>.<t_ns>` and only
  then removes the mark (spec §4, unchanged).
* Directory protection beyond these checks (another process of the same uid deleting the
  mark while no program runs) is the operating system's job (spec §7.3).

## 6. What a stop covers, and what it does not

* It stops the world's publications and promotions, exactly as spec §3 lists: no commit,
  no outside publication (the production request stream included), no seat result, no
  object create or retire, no generation proposal or promotion. In-flight activations
  are refused at commit and run again on resume.
* The program keeps its own clocks honest: time spent stopped does not count toward the
  episode's deadlines (`run_clock`), a production request refused by the stop is
  published again after resume, and the end-of-mode checks wait while the world is
  stopped. A durable generation proposal or promotion that the store refuses
  because of the stop (`RX_GEN_ERR_HALTED`) is not an outcome: the activation waits
  and posts it again after the resume (`halted_wait`, `src/runtime/rx_living.c`). A world
  whose episode is done is not torn down while stopped: the program waits for the resume
  (or `shutdown`) before it audits and ends the mode. A seat result caught by a stop is
  claimed again after the resume; the world counts those claims (`stats.resident_halted`)
  and the episode check subtracts them before it compares claims with trials.
  A stop therefore delays the episode and leaves its outcome unchanged.
* Evidence: at the end of every mode the program reads its crumb log (the canonical
  evidence, spec §4) and prints `R13 operator <mode-key>: stops S resumes R restored X
  open O; under stop: commits 0 externals 0 cancelled C`. Any COMMIT or EXTERNAL crumb
  between an OPERATOR_STOP and its OPERATOR_RESUME (or after an unresumed stop) fails the
  mode. The R13 receipt carries the totals under `"operator"`.
* **Outside effects already underway (spec §7.4).** A stop does not reach a durable
  executor already performing an effect outside the world, and it does not interrupt a
  GPU kernel already running on the resident seat. The production program has one
  outside effect path, the resident seat: a seat result that lands under a stop is
  refused and the output window re-projected from the world (spec §3, R12
  `t_emergency_stop`); the kernel's work is discarded and runs again on resume. A stop
  does not stop the process, its threads, ARGUS observation or the authority; it is not a
  kill switch and not a rollback.

### 6.1 Closed limit: the halt right is the epoch right

**Closed at aienos.lock `b84c0a67590a934f3f3e001b12ec85ebc086a9eb` (aienos#272, closing
aienos#266).** The native `auth_use` now honors a privileged right only on an entry whose
resource is `AIENOS_CAP_RES_AUTHORITY`, else `AIENOS_CAP_ERR_RESOURCE`. omega's Linux
oracle (`src/runtime/rx_caproot.c` `auth_use`) carries the same rule so R7 parity holds.
Proof: `make test-r16-operator-authority-misuse` mints the operator capability as R13 does and
requires every authority operation to refuse it (and one holding every privileged right on
the control resource), the halt check to accept it, and the office to still operate; against
bbad5e4 the halt capability bumps the epoch and the test fails. `make test-r7` compares the
two authorities on the same case. The text below is the record at bbad5e4.

The spec gives the stop the privileged epoch right (`spec/r16-operator-emergency-stop.md`
§2 lines 44-45: "The right is `RX_RIGHT_EPOCH` ... The right that may void every
capability may also pause the world"). ADR 0016 (aien-architecture main 014159e) is
silent on a halt or pause right: neither word occurs in it. Its capability section
(lines 367-382) lists the resource among what a capability binds, and line 1475 asks
that a wrong-resource attack fail.

The locked native authority does not check the resource when a capability is used:
`auth_use` (aienos `native/capability/aienos_capability.c` lines 205-218 at aienos.lock
bbad5e4) checks delivery, generation, state, epoch, expiry, the delegation chain and
the rights, not the entry's resource. So the operator's control capability
(`RX_WORLD_RES_CONTROL` with `RX_RIGHT_EPOCH`) would also pass `aienos_cap_bump_epoch`
(same file, line 555, through `auth_use(..., AIENOS_CAP_RIGHT_EPOCH, ...)` at line 369)
if anything handed it there. omega does not change the locked authority; the request
is aien-dev/aienos#266 (a resource check in `auth_use`, or a dedicated halt right).

What keeps it closed today is omega's own code, shown here and tested:

* The capability is minted once per world (`tests/runtime/rx_r13_living.c` 449,
  `mint(r, RX_OPERATOR_SUBJ, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT)`) and passed only
  to `rx_operator_open` (line 327), which writes it to the operator's credential file
  and keeps no other use of it.
* A presented capability (`q->cap`, parsed at `src/runtime/rx_operator.c` 262) reaches
  exactly four calls: `rx_world_emergency_stop` (291), `rx_world_emergency_resume`
  (312), `rx_world_operator_authorize` (328, for status, shutdown, revoke-cap and
  revoke) and, as the *target* of a revocation authorized by the authority's office,
  `aienos_cap_revoke` (360). The first three validate it with
  `rx_world_validate_cap(..., RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT, ...)` inside
  `halt_authorize` (`src/runtime/rx_world.c` 914).
* Every other command, with a full valid credential, is `BAD_REQUEST` before any
  authority call (`rx_operator.c` 327). The host test sends `explode`, `bump-epoch`,
  `bump_epoch`, `mint`, `grant`, `epoch`, `STOP`, `Stop`, `stop2`, `resume2` and `halt`
  and requires `BAD_REQUEST` for each; mutant O22 removes that refusal and the test fails.
* `aienos_cap_bump_epoch` has no caller in omega's production sources (`git grep`: the
  declaration in `src/runtime/aienos_cap.h` 56 and the R7 oracle test
  `tests/runtime/rx_r7_native.c` 217 only).

A future omega change that hands the control capability to any other authority entry
point reopened this while it was a known limit (until aienos#266 closed; see the note above).

## 7. Proof (G6 by execution)

`make test-r16-operator-host` (`tests/runtime/rx_operator_host.sh`) starts the production
host binary and drives it only through `build/rx_operator` and signals, checking replies,
the files in the state directory and the program's crumb-log audit lines: authorized stop
and resume; forged secret, wrong capability, wrong peer (root via `sudo -n`, reported
SKIPPED and therefore NOT_RUN if sudo is unavailable), malformed request; stale
credential (earlier world, earlier program start); `revoke-cap` and `revoke`; repeated
stop and resume; repeated stop and resume cycles under production load (in-flight work
refused and re-run); promotion blocked while stopped and completed after resume; restart
while stopped after `shutdown` and after `SIGKILL`; recovery to a passing R13 episode;
startup refusal of a group-readable, foreign-owned or symlinked state directory; a
second program on the same state directory or on the first one's generation directory
refused while the first keeps its socket and credential; a stop whose mark cannot be
written (generation directory made 0500) answering `STOPPED_NOT_DURABLE errno=13` with
client exit 3; eleven unknown commands with a full credential refused; an expired
resume answering `EXPIRED` and doing nothing, a missing or too-distant deadline refused.
On host three checks are made deterministic by holding one thread of the production
program under gdb in non-stop mode (the socket keeps serving): the seat acceptor held
with a computed seat result while a stop is accepted, then released, must end that claim
halted (`seat_halted` in status, so the episode's claim subtraction is exercised on
host); the positive world held after its episode's work, stopped, then released, must
stay up and stopped and print that it waits for a resume; the next world's open held
after the positive world closed must find a credential file planted by someone else
still in place. Without gdb those checks are SKIPPED (gate NOT_RUN). Silicon runs
without gdb; its in-flight count is reported, not checked. A host run whose only gap is a
missing `sudo -n` or gdb is NOT_RUN with the reason in the receipt, never FAIL.
`tests/runtime/rx_operator_mutants.sh` rebuilds the production program from mutated
copies and requires the test to fail against each (wiring missing, handler disconnected,
success reply without a state transition, unauthorized resume accepted, wiring present
only in a comment, wiring present only as an unused call, and the others it lists).
`make test-r16-operator-silicon` runs the same test on `build/rx_r13_living_silicon`
(resident GB10 seat) without the SIGKILL phase (a chip program is never killed). The
receipt labels host and silicon results separately. The silicon test requires the
program's own gate `R13_LIVING_SYSTEM=PASS` (bound to the candidate commit on a clean
tree): the spec allows no `SILICON_PASS_UNBOUND`. Each run writes
`operator_<seat>_receipt.json` to `RX_OP_OUT` (default `build/r16-operator`); the mutant
script gives every mutant its own directory, so a mutant never overwrites the real receipt.
`tools/r16_qualify.sh` copies both receipts into its raw directory (covered by the raw
digest) and refuses PASS unless each names its seat, the candidate commit on a clean tree,
no failure or skip, the seat's gate, and the SHA-256 of the production binary built in that
run; the silicon run counts toward `silicon_observed`. The G6 item
`operator_emergency_controls_passing` is PASS only when both ran and passed in the same
qualification run on the candidate.
