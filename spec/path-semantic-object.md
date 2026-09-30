# SPECIFICATION: Native Path Semantic Object and Relational Intelligence

```text
Document ID:     SPEC-OMEGA-PATH-M16
Classification:  Sovereign Semantic Specification
Target Substrate: Native Omega Bare Metal, Content-Addressed Directed Graph, Resident Reactions
Status:          SPECIFICATION FOR PATH-0 / PATH-1
Domain Tags:     "omega.path.v1" (semantic), "omega.path.realization.v1" (realization)
```

---

## 1. Executive Principle and Scope

A computational result alone is incomplete evidence. The relational path that produced the result is an inseparable component of the proof.

This specification defines the native Omega Path Semantic Object. It establishes relational path intelligence directly within bare-metal Omega architecture without external dependencies. No Rust service, Python daemon, external graph database (such as Neo4j), relational database, Redis cache, or network RPC layer is permitted. 

All temporary C runtime implementations are strictly classified as `BOOTSTRAP`, `REFERENCE`, or `TEST HARNESS` scaffolding, carrying a concrete migration route to native Omega bare metal.

---

## 2. Re-Audit and Architectural Ground Truth

### 2.1 Commit Baseline Inspected
The following specific repository commits and state lines were inspected to establish ground truth:

1. `omega` active worktree (`HEAD`): `6d1ff1dbcb15972dcc5195239ddd4085e6ff6de7` (Merge PR #105 from `feat/est-0-1`).
2. `omega` R16 orchestrator retirement specification: `4350eb9` (`feat/r16-orchestrator-retirement`).
3. `omega` R16 authority review and negative tests: `44d8c06` (`feat/r16-g3-g5-host`, checking G4 authority guards).
4. `omega` R15 performance qualification: `3e9e53b` (merged via PR #67 at `bba3bd3`).
5. `omega` M4 Canonical Encoding: `spec/canonical-encoding.md` (`SPEC-OMEGA-CANON-M4`).
6. `omega` Semantic Object Model: `spec/semantic-object.md` (`SPEC-OMEGA-OBJECT-M4`).
7. `omega` Program Identity v2: `spec/program-identity.md` (`SPEC-OMEGA-PROGRAM-ID-V2`).
8. `omega` Program Realization v0: `spec/program-realization.md`.
9. `omega` Action Graph IR: `spec/action-graph-ir.md`, `src/runtime/rx_graph.{c,h}`.
10. `omega` Plan Reuse: `spec/plan-reuse.md`, `src/runtime/rx_plan.{c,h}`.
11. `omega` J-Space Shared State: `spec/branch-state-reuse.md`, `src/runtime/rx_jspace.{c,h}`.
12. `omega` Cortex Durable Typed Memory: `src/runtime/rx_cortex.{c,h}`.
13. `omega` ARGUS Live Producer ABI: `src/runtime/rx_argus.{c,h}`.
14. `omega` Surface Language v0: `spec/omega-language-v0.md`.
15. `aien-sovereign-core` PR #102: inspected as an experimental PEARL Cortex reference only.

### 2.2 Reused Architecture and Non-Duplication
The Path Semantic Object reuses existing runtime machinery:

- **Canonical Identity Derivation**: Reuses `SPEC-OMEGA-CANON-M4` and `SPEC-OMEGA-PROGRAM-ID-V2` big-endian, length-prefixed, domain-tagged hashing principles.
- **Cognitive Branch Isolation**: Reuses `rx_jspace` state-unit chaining, branch-freezing rules, and divergence points.
- **Memory Classes**: Reuses Cortex (`rx_cortex.h`) memory classes (`CX_ENTITY`, `CX_CLAIM`, `CX_OBSERVATION`, `CX_EXECUTION`, `CX_EVIDENCE`).
- **Dependency Execution**: Reuses Action Graph IR (`rx_graph.h`) port-indexed data edges and order constraints.
- **Reaction Dispatch**: Reuses `rx_world.h` readiness indexing, run-token movement, and worker pool execution.
- **Hardware Observation**: Reuses ARGUS (`rx_argus.h`) ring-buffer event production for authority decisions, hardware cycles, and memory transitions.

---

## 3. Semantic Path vs. Path Realization

Omega enforces an architectural separation between what an operation sequence means and how it physically executed.

```text
+-------------------------------------------------------------------------------+
|                             SEMANTIC PATH                                     |
|  - Meaningful sequence of typed operations and relations                     |
|  - Abstract, platform-agnostic, mathematical definition                      |
|  - Invariant under hardware, placement, compiler choice, or execution time    |
|  - Bound to canonical content identity: SEMANTIC_PATH_ID                      |
+-------------------------------------------------------------------------------+
                                      |
                                      | realized by (1-to-many)
                                      v
+-------------------------------------------------------------------------------+
|                            PATH REALIZATION                                   |
|  - Physical manifestation on concrete hardware (e.g. Grace Blackwell GB10)   |
|  - Specific scheduling (SEQUENTIAL vs. PRELOAD), arena placement, memory      |
|  - Measured ARGUS telemetry: latency (ns), energy (nJ), memory delta (bytes)  |
|  - Bound to physical triple identity: REALIZATION_ID                          |
+-------------------------------------------------------------------------------+
```

### 3.1 The Semantic Path
A Semantic Path is a content-addressed, immutable sequence of typed semantic steps. It specifies the logical trajectory from a starting semantic state or entity to an ending state or claim. It contains zero references to host memory addresses, process IDs, hardware core indices, compiler optimizations, clock cycles, or energy measurements. Two paths that perform the identical sequence of logical operations on identical semantic entities have identical semantic identities across all machines and processes.

### 3.2 The Path Realization
A Path Realization describes the physical execution of a Semantic Path on a concrete hardware substrate. It binds the Semantic Path to a target machine graph, a specific scheduler policy, an execution engine profile, physical memory arenas, and observed execution telemetry. Many distinct Path Realizations can correspond to a single Semantic Path.

---

## 4. Typed Semantic Step Vocabulary

A relational path is not a loose collection of untyped graph edges. Each transition along a path is a typed semantic step belonging to an explicit vocabulary family.

```text
Step Envelope:
+-------------+-------------+-------------+-------------+-----------------------+
| step_index  | step_family |  step_role  | input_count | input_semantic_ids    |
|   (u16)     |   (u16)     |   (u16)     |   (u16)     |   [SemanticId; N]     |
+-------------+-------------+-------------+-------------+-----------------------+
| operator_id | out_count   | output_ids  | param_len   | param_payload_bytes   |
| (SemanticId)|   (u16)     | [SemId; M]  |   (u32)     |   (big-endian bytes)  |
+-------------+-------------+-------------+-------------+-----------------------+
```

### 4.1 Step Families and Roles

1. **Family 0x0001: Entity Relation Path (`STEP_FAM_RELATION`)**
   - Captures relational knowledge graphs and verified claims.
   - Sequence: `ENTITY -> CLAIM -> ENTITY -> EVIDENCE -> ENTITY`.
   - Roles:
     - `0x0101 ROLE_REL_ENTITY`: An ontological entity in the semantic universe.
     - `0x0102 ROLE_REL_CLAIM`: A declared assertion connecting entities.
     - `0x0103 ROLE_REL_EVIDENCE`: A cryptographic receipt or proof reference supporting the claim.
     - `0x0104 ROLE_REL_INFERENCE`: A candidate relational link derived via deduction.

2. **Family 0x0002: Execution Path (`STEP_FAM_EXECUTION`)**
   - Captures deterministic task computation through the action graph.
   - Sequence: `GOAL -> OPERATION -> OPERATION -> VERIFICATION -> RESULT`.
   - Roles:
     - `0x0201 ROLE_EXEC_GOAL`: The target objective object.
     - `0x0202 ROLE_EXEC_OPERATION`: A discrete computational operation.
     - `0x0203 ROLE_EXEC_VERIFICATION`: An invariant or contract check over intermediate state.
     - `0x0204 ROLE_EXEC_RESULT`: The validated terminal value or state.

3. **Family 0x0003: J-Space Trajectory Path (`STEP_FAM_JSPACE`)**
   - Captures hypothesis space exploration across cognitive branches.
   - Sequence: `WORLD -> FORK -> PROPOSAL -> ACTION -> OBSERVATION -> VERIFIER -> CANDIDATE`.
   - Roles:
     - `0x0301 ROLE_JS_WORLD`: Baseline world root snapshot.
     - `0x0302 ROLE_JS_FORK`: Creation of an isolated branch workspace.
     - `0x0303 ROLE_JS_PROPOSAL`: Proposed hypothetical modification.
     - `0x0304 ROLE_JS_ACTION`: Concrete state manipulation step.
     - `0x0305 ROLE_JS_OBSERVATION`: Sensor or state readout event.
     - `0x0306 ROLE_JS_VERIFIER`: Gate verification check on the branch.
     - `0x0307 ROLE_JS_CANDIDATE`: Validated candidate trajectory unit.

4. **Family 0x0004: Capability Realization Path (`STEP_FAM_CAPABILITY`)**
   - Captures the path from semantic intent to authorized physical effect.
   - Sequence: `INTENT -> OPERATION -> CAP_REQUIREMENT -> IMPLEMENTATION -> MACHINE -> AUTH_VERIFY -> EFFECT`.
   - Roles:
     - `0x0401 ROLE_CAP_INTENT`: High-level declared purpose.
     - `0x0402 ROLE_CAP_OPERATION`: Concrete semantic operation requesting resource access.
     - `0x0403 ROLE_CAP_REQUIREMENT`: Explicit declared capability token demand.
     - `0x0404 ROLE_CAP_IMPLEMENTATION`: Code or kernel selected to execute operation.
     - `0x0405 ROLE_CAP_MACHINE`: Target hardware node and execution engine.
     - `0x0406 ROLE_CAP_AUTH_VERIFY`: Native verification of capability tokens against authority root.
     - `0x0407 ROLE_CAP_EFFECT`: Authorized effect dispatch.

5. **Family 0x0005: Scientific Discovery Path (`STEP_FAM_DISCOVERY`)**
   - Captures the empirical discovery cycle.
   - Sequence: `OBSERVATION -> HYPOTHESIS -> EXPERIMENT -> MEASUREMENT -> UPDATE -> EXPLANATION`.
   - Roles:
     - `0x0501 ROLE_DISC_OBSERVATION`: Empirical observation from ARGUS or Cortex.
     - `0x0502 ROLE_DISC_HYPOTHESIS`: Formal proposition explaining observation.
     - `0x0503 ROLE_DISC_EXPERIMENT`: Controlled test action graph.
     - `0x0504 ROLE_DISC_MEASUREMENT`: Observed experimental outcome.
     - `0x0505 ROLE_DISC_UPDATE`: Epistemic state refinement.
     - `0x0506 ROLE_DISC_EXPLANATION`: Structural abstraction explaining the phenomenon.

---

## 5. Canonical Identity Derivation

Identity derivation adheres strictly to the canonical identity laws of `spec/canonical-encoding.md` and `spec/program-identity.md`.

### 5.1 Semantic Path Identity Law
The semantic identity of a path is a SHA-256 cryptographic hash computed exclusively over its domain tag and canonical semantic byte sequence.

$$\text{SEMANTIC\_PATH\_ID} = \text{SHA256}(\text{"omega.path.v1"} \parallel 0x00 \parallel \text{CANONICAL\_SERIALIZATION}(\text{path}))$$

```text
Preimage Layout:
1. Domain Tag:               "omega.path.v1" (13 bytes)
2. Tag Terminator:           0x00 (1 byte)
3. Start Anchor ID:          start_anchor_id (32 bytes SemanticId)
4. End Anchor ID:            end_anchor_id (32 bytes SemanticId)
5. Context Scope ID:         context_id (32 bytes SemanticId)
6. Step Count:               step_count (2 bytes u16 big-endian)
7. Canonical Step Stream:    Concatenation of canonical steps (0 to step_count - 1)
8. Canonical Attributes:     Lexicographically sorted attributes
9. Canonical Constraints:    Kind-sorted constraints
```

### 5.2 Identity Invariants
1. **Intensional Sequence**: Semantic Path identity is order-dependent. A path executing step A then step B produces a different identity than a path executing step B then step A.
2. **Platform Invariance**: A path evaluated on AArch64 Grace Blackwell GB10 and the identical path evaluated on an x86_64 host or QEMU target produce the identical `SEMANTIC_PATH_ID`.
3. **Builder Order Invariance**: Attributes and constraints are sorted canonically before serialization. Internal builder sequence does not alter the identity.
4. **Pointer Independence**: Host virtual memory addresses, arena offsets, thread IDs, and process handles never enter the preimage.

### 5.3 Path Realization Identity Law
A Path Realization possesses its own separate identity, binding the semantic path to physical hardware, implementation choices, and execution receipts.

$$\text{REALIZATION\_ID} = \text{SHA256}(\text{"omega.path.realization.v1"} \parallel 0x00 \parallel \text{REALIZATION\_PREIMAGE})$$

```text
Realization Preimage Layout:
1. Domain Tag:               "omega.path.realization.v1" (24 bytes)
2. Tag Terminator:           0x00 (1 byte)
3. Semantic Path ID:         semantic_path_id (32 bytes)
4. Machine ID:               machine_id (32 bytes content-addressed machine graph)
5. Engine Profile ID:        engine_profile_id (32 bytes)
6. Realizer Code Digest:     code_digest (32 bytes)
7. Schedule Kind:            schedule_kind (2 bytes u16 big-endian)
8. Telemetry Summary Digest: argus_observation_digest (32 bytes)
```

---

## 6. Concrete Canonical Binary Encoding

All multi-byte numeric quantities are encoded in network byte order (big-endian). There is zero internal struct padding.

### 6.1 Encoding Grammar

```text
Offset  Length  Field                  Type / Description
---------------------------------------------------------------------------------
+0x00   4       magic                  0x4F, 0x4D, 0x47, 0x30 ("OMG0")
+0x04   1       version                0x01
+0x05   1       kind                   0x0C (KIND_PATH)
+0x06   2       step_count             u16 big-endian, count of steps (0..512)
+0x08   32      start_anchor_id        SemanticId (32 bytes SHA-256)
+0x28   32      end_anchor_id          SemanticId (32 bytes SHA-256)
+0x48   32      context_id             SemanticId (32 bytes SHA-256)
+0x68   2       attr_count             u16 big-endian, count of attributes (0..32)
+0x6A   ...     attributes             Sorted lexicographically by UTF-8 key
                [ key_len: u8, key_bytes, val_len: u16, val_bytes ]
...     2       constraint_count       u16 big-endian, count of constraints (0..16)
...     ...     constraints            Sorted by (constraint_kind: u16, payload)
...     4       steps_payload_len      u32 big-endian, total length of steps data
...     ...     ordered_steps          Array of step records in sequential order
```

### 6.2 Step Record Binary Format
Each step in `ordered_steps` is encoded as follows:

```text
Offset  Length  Field                  Type / Description
---------------------------------------------------------------------------------
+0x00   2       step_index             u16 big-endian (must match array index)
+0x02   2       step_family            u16 big-endian (0x0001..0x0005)
+0x04   2       step_role              u16 big-endian (specific role tag)
+0x06   2       input_count            u16 big-endian, input reference count (0..8)
+0x08   32*N    input_ids              N consecutive 32-byte SemanticIds
+...    32      operator_id            SemanticId of operation, relation, or opcode
+...    2       output_count           u16 big-endian, output reference count (0..8)
+...    32*M    output_ids             M consecutive 32-byte SemanticIds
+...    4       param_len              u32 big-endian, parameter payload length (0..1024)
+...    P       param_bytes            Kind-specific parameter bytes (big-endian)
```

### 6.3 Architectural Limits
To guarantee bounded memory usage on bare-metal targets, the encoder and decoder enforce strict static limits:

| Limit Name | Maximum Value | Failure Mode on Exceeding |
|---|---|---|
| `PATH_MAX_STEPS` | 512 steps | Fail-closed: `RX_PATH_ERR_STEP_LIMIT` (-80) |
| `PATH_MAX_INPUTS_PER_STEP` | 8 inputs | Fail-closed: `RX_PATH_ERR_INPUT_LIMIT` (-81) |
| `PATH_MAX_OUTPUTS_PER_STEP` | 8 outputs | Fail-closed: `RX_PATH_ERR_OUTPUT_LIMIT` (-82) |
| `PATH_MAX_PARAM_LEN` | 1024 bytes | Fail-closed: `RX_PATH_ERR_PARAM_LIMIT` (-83) |
| `PATH_MAX_ATTR_COUNT` | 32 attributes | Fail-closed: `RX_PATH_ERR_ATTR_LIMIT` (-84) |
| `PATH_MAX_CONSTRAINT_COUNT` | 16 constraints | Fail-closed: `RX_PATH_ERR_CONST_LIMIT` (-85) |
| `PATH_MAX_TOTAL_SERIALIZATION` | 65536 bytes (64 KiB) | Fail-closed: `RX_PATH_ERR_BUFFER_OVERFLOW` (-86) |

---

## 7. Shared-Prefix Branch Tails (Memory Scaling)

Exploration algorithms in J-Space routinely evaluate hundreds or thousands of candidate trajectories that share long common prefixes (for example, steps 0 through 120 are identical across 1000 candidate paths). Duplicating the common prefix across every candidate creates unacceptable memory pressure and cache pollution.

```text
Path Prefix Tree (Copy-On-Write Merkle DAG):

           [Step 0] -> [Step 1] -> ... -> [Step K-1]  (Shared Prefix Path: prefix_id)
                                             |
                   +-------------------------+-------------------------+
                   |                                                   |
                   v                                                   v
           [Branch Alpha]                                      [Branch Beta]
     [Step K_a] -> [Step K+1_a]                         [Step K_b] -> [Step K+1_b]
  (Tail: private steps K..N-1)                       (Tail: private steps K..M-1)
```

### 7.1 Representation Rules
1. **Prefix Sharing by Content Reference**: When a path forks at divergence step $K$, the child path does not copy steps $0 \dots K-1$. It stores:
   - `parent_path_id`: The 32-byte `SEMANTIC_PATH_ID` of the common ancestor.
   - `divergence_step_index`: The 16-bit integer $K$.
   - `tail_steps`: An array containing only the branch-local steps $K \dots N-1$.
2. **Deterministic Full Identity**: The `SEMANTIC_PATH_ID` of the branched path is computed over the fully reconstructed logical sequence ($0 \dots N-1$). The sharing of prefixes in physical storage does not alter the mathematical semantic identity.
3. **Ancestor Immutability**: Aligned with `rx_jspace` branch freezing rules (`spec/branch-state-reuse.md`), once a path serves as a parent prefix for branch forks, its step sequence is frozen and immutable. Further modifications require constructing a new branch tail.

---

## 8. Authority Separation and Fail-Closed Boundary

### 8.1 Priority of R16 Orchestrator Retirement
R16 orchestrator retirement (`spec/r16-orchestrator-retirement.md`) addresses the G4 authority issue: legacy or non-authoritative callers asserting identity without cryptographic binding to the executing capability root.

Under no circumstances does PATH work weaken, bypass, or postpone R16 authority enforcement.

### 8.2 The Non-Authoritative Principle
**A PATH IS EVIDENCE, NEVER AUTHORITY.**

```text
+-------------------------------------------------------------------------------+
|                                 PATH SPACE                                    |
|  - Relational trajectories                                                    |
|  - Candidate operation chains                                                 |
|  - Empirical performance predictions                                          |
|  - High Pareto scores                                                         |
+-------------------------------------------------------------------------------+
                                      |
                                      | Proposes Action
                                      v
=================================================================================
                 FAIL-CLOSED AUTHORITATIVE BOUNDARY (AIENOS Root)
   Checks: 1. Is caller bound to authentic capability token?
           2. Is token valid for active generation (64-bit generation check)?
           3. Does token explicitly permit effect on target physical resource?
=================================================================================
             |                                                  |
             | If Valid                                         | If Invalid / Unchecked
             v                                                  v
+-----------------------------+                    +----------------------------+
|   EFFECT DISPATCH GRANTED   |                    | REFUSED: RX_ERR_CAP_DENIED |
|   (Authoritative Receipt)   |                    | (State Untouched, Crumb)   |
+-----------------------------+                    +----------------------------+
```

1. **Zero Authority Granting**: A path that documents the sequence `ACTOR -> CAPABILITY -> OPERATION` grants zero rights. The recording of a capability reference in a path step is descriptive evidence that an access was modeled or previously observed; it provides no authority to execute.
2. **Candidates Authorize Nothing**: A path step sequence `CANDIDATE -> PROMOTION` authorizes no generation promotion or state modification.
3. **Fail-Closed Gate**: Any proposal derived from path analysis submitted to the production reaction runtime (`rx_world.h`) must pass through the native AIENOS capability verification root (`aienos_cap_validate`). If valid credentials are not presented, the request fails closed immediately with error code `RX_ERR_CAP_DENIED`.
4. **Sequencing Guard**: No path components are linked into the production authoritative reaction loop until R16 qualification (`R16_ORCHESTRATOR_RETIRED = PASS`) is merged.

---

## 9. Evidence, Observations, and Telemetry Integration

### 9.1 ARGUS as the Sole Observation Plane
Omega does not invent secondary telemetry protocols for relational paths. The observation plane is ARGUS (`src/runtime/rx_argus.h`).

All physical metrics observed during path execution are produced as ARGUS events:
- Wall-clock and execution latency (nanoseconds).
- Hardware energy consumed (nanojoules).
- Memory allocations, relocations, and cache footprints.
- Inter-core and inter-socket synchronization overhead.
- Execution errors, register faults, and retry counts.
- Temperature and thermal throttle indicators.
- Hardware capability validation and denial events.

### 9.2 Observation Non-Contamination Rule
Observations belong strictly to the Path Realization and Evidence layers. 

Observations NEVER participate in the computation of `SEMANTIC_PATH_ID`. Telemetry values vary between runs, hardware models, and temperature conditions; folding telemetry into semantic identity would destroy content-addressed equivalence and prevent deterministic replay.

### 9.3 Cortex Typed Storage
Completed paths, realizations, and receipts are recorded into Cortex durable memory (`rx_cortex.h`):
- `CX_EXECUTION`: Records the execution receipt of the path.
- `CX_EVIDENCE`: Stores the cryptographic verification proofs.
- `CX_OBSERVATION`: Stores ARGUS telemetry summaries linked by `SEMANTIC_PATH_ID`.
- `CX_RELATIONSHIP`: Stores verified relational abstractions.

Protected Cortex memory classes (`CX_PROT_AUTHORITY`, `CX_PROT_COMMIT_RECEIPT`, `CX_PROT_VERIFY_EVIDENCE`, `CX_PROT_GENERATION_ID`) ensure that path records can never be summarized or evicted under memory pressure.

---

## 10. Invariant Catalog

Every implementation of the Path Semantic Object must maintain eight fundamental architectural invariants:

- **I1: PATH != AUTHORITY**: Recording a path grants no authority. Authority stems exclusively from the AIENOS capability root.
- **I2: PATH != FACT**: A path recording a sequence of claims or hypotheses represents evidence of reasoning, not established fact. Inferred relationships remain candidates until promoted.
- **I3: PATH != HIDDEN CHAIN OF THOUGHT**: Paths record externally verifiable, typed operations, observations, decisions, and evidence references. Unrecorded scratchpad deliberations or invisible internal states are prohibited.
- **I4: SEMANTIC PATH != PATH REALIZATION**: Logical sequence identity is completely decoupled from execution medium, placement, scheduling, or runtime duration.
- **I5: PREDICTION != VERIFICATION**: A predicted path outcome or high model score is not verification. Verification requires empirical evaluation and proof receipts.
- **I6: HIGH SCORE != PERMISSION**: A candidate path achieving a high score on quality, latency, or energy acquires zero permission to bypass authority checks.
- **I7: OBSERVATION != INFERENCE**: Raw telemetry recorded by ARGUS represents an empirical observation. An inferred causal relationship is a separate candidate claim requiring independent validation.
- **I8: CANDIDATE != PROMOTED KNOWLEDGE**: Candidate paths explored in J-Space or Cortex memory remain candidates until passing explicit, frozen, statistical promotion gates.

---

## 11. Multi-Vector Scoring and TURING Epistemic Gain

Path evaluation rejects scalar score collapse. A single scalar score obscures trade-offs between accuracy, verification confidence, resource cost, and authority risk.

### 11.1 The Multi-Vector Score
A path score is an explicit vector of thirteen measurable quantities:

$$\mathbf{S}(\text{path}) = \begin{bmatrix}
q_{\text{task}} \\
e_{\text{strength}} \\
c_{\text{verify}} \\
s_{\text{simplicity}} \\
u_{\text{budget}} \\
t_{\text{latency}} \\
e_{\text{energy}} \\
m_{\text{memory}} \\
x_{\text{movement}} \\
y_{\text{sync}} \\
r_{\text{risk}} \\
a_{\text{auth}} \\
g_{\text{turing}}
\end{bmatrix} = \begin{array}{l}
\text{Task Quality (ppm)} \\
\text{Evidence Strength (ppm)} \\
\text{Verification Confidence (ppm)} \\
\text{Structural Simplicity / MDL (bytes)} \\
\text{Uncertainty Bound (ppm)} \\
\text{Execution Latency (ns)} \\
\text{Energy Consumption (nJ)} \\
\text{Peak Memory Frame Footprint (bytes)} \\
\text{Data Movement Cost (bytes transferred)} \\
\text{Synchronization Wait Cycles (cycles)} \\
\text{Authority Risk Factor (ppm)} \\
\text{Authority Validity Status (boolean / enum)} \\
\text{Turing Epistemic Information Gain (bits)}
\end{array}$$

### 11.2 Scoring Principles
1. **Deterministic Interpretable Rules First**: Initial path ranking employs explicit, interpretable Pareto frontiers over the score vector. No black-box learned neural ranking model may evaluate paths on the authoritative path.
2. **Context-Specific Pareto Selection**: An operation operating under extreme power constraints selects paths on the energy-latency Pareto frontier; an operation requiring provable safety selects on the verification-authority frontier.
3. **Score Separation from Identity**: The score vector is an evaluation metadata record attached to a realization in Cortex; it never alters `SEMANTIC_PATH_ID`.

### 11.3 TURING Epistemic Integration
TURING evaluates whether a discovered path represents genuine epistemic gain using the Minimum Description Length (MDL) principle:

$$\text{Gain}_{\text{TURING}} = L(\mathcal{D}) - [L(\mathcal{D} \mid \text{path}) + L(\text{path})]$$

Where:
- $L(\mathcal{D})$ is the description length of the observed empirical evidence under the current model.
- $L(\mathcal{D} \mid \text{path})$ is the residual description length using the path abstraction.
- $L(\text{path})$ is the canonical binary description length of the path itself.

A path is eligible for consideration as general knowledge only when $\text{Gain}_{\text{TURING}} > 0$ across frozen, held-out validation sets.

### 11.4 Recursive Self-Improvement (RSI) Discipline
RSI never mutates production runtime structures based on a single successful execution. The promotion pipeline requires:

$$\text{Candidate Path} \longrightarrow \text{Isolated Branch Run} \longrightarrow \text{ARGUS Measurement} \longrightarrow \text{Formal Verification} \longrightarrow \text{Held-Out Evaluation} \longrightarrow \text{Statistical Gate Approval}$$

Failed paths are preserved in Cortex memory to prevent recursive re-exploration of known defective trajectories.

---

## 12. Resident-Reaction Fit (No Central Orchestrator)

Omega operates through resident reactions, not a central control loop. The Path Semantic Object is engineered specifically to execute within this reaction paradigm (`src/runtime/rx_world.h`).

```text
Reaction Flow:
+-------------------+     +---------------------+     +-----------------------+
| Path Cell (Step K)| --> | Reaction Evaluator  | --> | Path Cell (Step K+1)  |
| Published Token   |     | Matches Readiness   |     | Emits New State Token |
+-------------------+     +---------------------+     +-----------------------+
        ^                            ^                            |
        |                            |                            v
+-------------------+     +---------------------+     +-----------------------+
| Dependency Index  |     | Checks Capabilities |     | Triggers Next Step    |
| (World Cell State)|     | & Resource Budget   |     | Reaction Worker       |
+-------------------+     +---------------------+     +-----------------------+
```

### 12.1 Execution Mechanics
1. **No Central Thread**: No central path sequencer loop advances paths. Paths advance through cell state changes.
2. **State Cells**: A path under active traversal is represented as an Omega World Cell holding the current step token, head pointer, and branch context.
3. **Reaction Triggers**: Reactions declare input dependencies on specific path step completions. When step $K$ finishes and publishes its outcome, the reaction engine updates its readiness index.
4. **Worker Pool Execution**: Any available resident worker thread executes the newly eligible reaction for step $K+1$.
5. **Decoupled Concurrency**: Independent branches of a path graph execute concurrently across available worker threads without central coordination.

---

## 13. Surface Language Construct Investigation

An investigation was conducted into exposing paths as first-class syntax constructs within the Omega Surface Language (`spec/omega-language-v0.md`).

### 13.1 Evaluated Syntax Proposal
The following syntax was evaluated for potential inclusion:

```text
// Speculative syntax sketch:
path verify_trajectory(start: State, goal: Target) -> Result {
    step fork_branch = fork(start);
    step propose_op  = action(fork_branch, OP_APPLY);
    step verify_post = verify(propose_op, contract);
    yield verify_post;
}
```

### 13.2 Linguistic and Architectural Evaluation
1. **Grammar Mismatch**: Surface Language V0 is strictly a line-oriented expression language designed for the Visor interactive console (`let`, `fn`, binary operations mod $2^W$). Introducing multi-line block structures and stateful step keywords directly violates Language V0 simplicity.
2. **Existing Lowering Substrate**: The Action Graph IR (`rx_graph.h`) already models typed nodes, dependency ordering, and verification gates. Introducing duplicate language keywords adds syntax noise without expanding semantic capability.
3. **Investigation Verdict**: 
   - **Do not adopt specialized path syntax in Language V0.**
   - Instead, paths are constructed in the Visor console by referencing existing Action Graph builder functions.
   - Any eventual dedicated language syntax must be deferred to Language V1 and formalized in a dedicated ADR after PATH-4 qualification.

---

## 14. Promotion Ladder and Lifecycle Status

Paths and their associated knowledge progress through a strict qualification ladder:

```text
TARGET  ===>  EXPERIMENTAL  ===>  MEASURED  ===>  QUALIFIED  ===>  PRODUCTION
```

### 14.1 Status Definitions

| Status Level | Criteria and Verification Requirement |
|---|---|
| `TARGET` | Design phase. Defined in specification (`spec/path-semantic-object.md`). Zero runtime code. |
| `EXPERIMENTAL` | Initial implementation. Isolated unit tests in sandbox. No production exposure. |
| `MEASURED` | Quantitative profiling. Evaluated across benchmark workloads with ARGUS telemetry receipts. |
| `QUALIFIED` | Differential testing passed. Proved to match oracle baselines; verified against statistical gates. |
| `PRODUCTION` | Active in production runtime. Approved by Drake and ratified in architecture doctrine. |

### 14.2 Per-Object Lifecycle States
Individual path objects in memory carry an operational status:
- `PATH_STATUS_PENDING`: Constructed, awaiting dependency readiness.
- `PATH_STATUS_ACTIVE`: Currently executing across resident reaction workers.
- `PATH_STATUS_COMPLETED`: Traversal reached terminal anchor; verification passed.
- `PATH_STATUS_REFUSED`: Refused at the capability boundary due to missing authority.
- `PATH_STATUS_FAILED`: Invariant violated or intermediate operation failed.

---

## 15. Milestone Gates: PATH-1 through PATH-7

Path capability bring-up follows seven rigorous, pre-registered milestone gates:

### PATH-1: Native Identity and Construction
- **Objective**: Implement canonical identity derivation and in-memory construction.
- **Verification Suite**: `tests/path/test_path_identity.c`.
- **Pass Criteria**:
  - Determinism: 1,000 randomized paths produce identical `SEMANTIC_PATH_ID` across repeated runs.
  - Order Sensitivity: Swapping any two non-identical steps changes `SEMANTIC_PATH_ID`.
  - Cross-Realization Invariance: Changing machine profile or schedule leaves `SEMANTIC_PATH_ID` unchanged while altering `REALIZATION_ID`.
  - Canonical Serialization: Binary round-trip `decode(encode(path)) == path`.
  - Tamper Refusal: Bit-flip in serialized stream causes hash mismatch and refusal.
  - Bounded Memory: Attempting to add 513 steps fails closed with `RX_PATH_ERR_STEP_LIMIT`.

### PATH-2: Execution Observation (No Ranking Control)
- **Objective**: Wire ARGUS telemetry producer into path step execution.
- **Pass Criteria**:
  - ARGUS ring buffer captures latency, memory delta, and authority verification events for every step.
  - Zero observation bytes participate in `SEMANTIC_PATH_ID`.
  - Observation receipts written to `evidence/PATH/` with content-addressed hashes.

### PATH-3: J-Space Trajectory Experiment
- **Objective**: Compare endpoint-only search versus trajectory-aware search in J-Space.
- **Pass Criteria**:
  - Sealed benchmark comparing exploration efficiency across 500 tasks.
  - Shared-prefix branch tails demonstrate $\ge 60\%$ memory reduction over full copy.
  - Zero state leakage between sibling branches.

### PATH-4: Cortex Path-Context Experiment
- **Objective**: Evaluate bounded path-context retrieval in Cortex durable memory.
- **Pass Criteria**:
  - Query retrieval over historical paths by subject and semantic category.
  - Strict preservation of protected memory classes (`CX_PROT_ALL`).
  - Retrieval latency scales with path length, not total Cortex store size.

### PATH-5: Capability-Path Experiment
- **Objective**: Implement pre-flight trajectory capability validation.
- **Pass Criteria**:
  - Invalid paths (requesting missing capabilities or unavailable hardware) are rejected before execution.
  - Hostile test: Injected path claiming fabricated authority fails closed at the AIENOS boundary.

### PATH-6: TURING Epistemic Integration
- **Objective**: Integrate Minimum Description Length scoring for path abstractions.
- **Pass Criteria**:
  - Automated calculation of Turing gain across held-out benchmark datasets.
  - Path compression accepted only when description length reduces monotonically.

### PATH-7: Adaptive Exploration
- **Objective**: Enable empirical-evidence-driven search depth and width adjustment.
- **Pass Criteria**:
  - Exploration bounds dynamically adjust based on measured uncertainty vectors.
  - Qualification on Grace Blackwell GB10 hardware.

---

## 16. Architectural Decision Record (ADR) Candidates

Before production integration, the following formal ADRs must be drafted and submitted to `docs/adr/`:

1. **ADR-0021: Native Relational Path Semantic Object Model**
   - Formalizes `KIND_PATH` (0x0C) in the core semantic ontology.
   - Ratifies the separation between Semantic Path Identity and Path Realization Identity.
2. **ADR-0022: Shared-Prefix Branch Tail Representation for J-Space**
   - Standardizes the Merkle DAG prefix-sharing specification across memory arenas.
   - Establishes immutable parent freezing rules for divergent cognitive trajectories.
3. **ADR-0023: Fail-Closed Authority Boundary for Relational Path Proposals**
   - Mandates that all path-derived execution plans pass through the native AIENOS capability verification root.
   - Enforces the absolute invariant: Path is evidence, never authority.
4. **ADR-0024: Pareto Multi-Vector Path Scoring and TURING Integration**
   - Replaces scalar optimization with explicit 13-dimensional score vectors.
   - Ratifies the Minimum Description Length criterion for path knowledge promotion.

---

## 17. Bootstrap / Reference Classification and Native Replacement Route

To ensure long-term architectural sovereignty, all software artifacts created during the PATH initiative are classified under an explicit substrate lifecycle.

```text
Migration Route:
[C Scaffolding (Bootstrap)] ===> [Verified Reference Oracle] ===> [Native Bare-Metal Omega]
```

### 17.1 Substrate Classification Table

| Component | Initial Implementation | Classification | Target Substrate | Migration Strategy |
|---|---|---|---|---|
| `rx_path.h` / `rx_path.c` | Host C99 | `BOOTSTRAP` | Bare-Metal Omega | Lower struct definitions to native Omega memory frame declarations. |
| Canonical Path Encoder | Host C99 / SHA-256 | `REFERENCE` | Native ASM / Mojo | Verify byte-for-byte equivalence against host reference oracle. |
| Prefix Branch Allocator | Host C99 Arena | `BOOTSTRAP` | Resident Reaction Memory | Transition from POSIX memory arenas to native Omega frame buffers. |
| Test Harnesses | C / Shell (`make test-path`) | `TEST HARNESS` | Self-Hosting Verifier | Run within Visor qualification suite without host Linux dependencies. |

### 17.2 Native Omega Replacement Route
1. **Zero External Library Dependence**: C bootstrap implementations must use only standard integer types and strict memory bounds. No POSIX threads, dynamic libc heap (`malloc`/`free`), or Linux system calls are permitted on the core path; memory is provided via caller-allocated buffers.
2. **Lowering to Bare-Metal Reactions**: Once PATH-1 and PATH-2 pass qualification, path serialization and step validation will be lowered to native AArch64 instruction sequences generated by `aarch64_encoder.c`.
3. **Elimination of Host Tooling**: The final stage removes Linux host harness wrappers, compiling path reactions directly into the standalone Omega kernel image running on Grace Blackwell GB10 bare metal.
