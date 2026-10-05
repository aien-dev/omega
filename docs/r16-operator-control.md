# R16 G6: operator control of the production program (interface contract)

Status: CONTRACT, written before the code (lane ESTOP, CAND-3). The runtime control it
wires is `spec/r16-operator-emergency-stop.md` (merged #300); this document says how an
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

* An `AF_UNIX` stream socket `<state>/control/operator.sock`, created when the world is
  ready for operator requests and removed when the world is torn down. Between worlds
  there is no socket: a client gets "connection refused" and the request does nothing.
* Two walls before the credential: the directory is 0700 and owned by the program's
  user (validated at startup, §5), and every accepted connection is checked with
  `SO_PEERCRED`: the peer uid must equal the program's effective uid, else the reply
  is `REFUSED reason=peer` and nothing is read further. Root can pass the directory but
  not the peer check.
* One request per connection, read with a 2 s receive timeout; at most 511 bytes up to
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
aien-operator v1 <command> subject=<u32> gen=<u64> secret=<64 hex> cap=<id>:<gen> [reason=<int>]
```

`<command>` is `status`, `stop`, `resume`, `revoke-cap`, `revoke` or `shutdown`. Any other
token, a missing or repeated field, a secret that is not 64 hex digits, a number out of
range or extra bytes is `BAD_REQUEST`, decided before any world call.

Reply (one line): `aien-operator v1 <RESULT> key=value ...`. RESULT is one of `OK`,
`ALREADY` (stop of a stopped world), `NOT_STOPPED` (resume or shutdown of a running world),
`REFUSED reason=peer|identity|authority`, `BAD_REQUEST`, `ERROR rc=<n>`.
A refusal says nothing about the world: no state, no counters.

## 4. Commands

Every command except `BAD_REQUEST` cases goes to the world authority first. `status`,
`revoke-cap`, `revoke` and `shutdown` are checked by `rx_world_operator_authorize` (the
same `halt_authorize` the stop and resume use, under the same locks, added to
`rx_world.c` so no second authority check exists). `stop` and `resume` are checked inside
`rx_world_emergency_stop` / `_resume` themselves.

| command | world call | reply on success |
|---|---|---|
| `status` | `rx_world_operator_authorize`, `rx_world_halt_status` | `OK state=running\|stopped restored= seq= durable= refused= crumbs= objects=` plus program counters `served= production_commits= inforce= active_generation=` |
| `stop [reason]` | `rx_world_emergency_stop` | `OK state=stopped seq= crumb= durable=`; a second stop: `ALREADY` with no new crumb |
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
  directory (checked with `lstat`, a symlink is refused), owned by the effective uid, with
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
  stopped. A stop therefore delays the episode; it does not fail it.
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
startup refusal of a group-readable, foreign-owned or symlinked state directory.
`tests/runtime/rx_operator_mutants.sh` rebuilds the production program from mutated
copies and requires the test to fail against each (wiring missing, handler disconnected,
success reply without a state transition, unauthorized resume accepted, wiring present
only in a comment, wiring present only as an unused call, and the others it lists).
`make test-r16-operator-silicon` runs the same test on `build/rx_r13_living_silicon`
(resident GB10 seat) without the SIGKILL phase (a chip program is never killed). The
receipt labels host and silicon results separately. The G6 item
`operator_emergency_controls_passing` is PASS only when both ran and passed in the same
qualification run on the candidate.
