# Turing Architecture: Prior-Art Kill Test

| Field | Value |
|---|---|
| Date | 2026-09-29 |
| Author | Claude Opus 5.5 research worker |
| Question | Does the proposed Turing architecture collapse into existing work? |
| Method | Every entry below was opened at its primary source (paper page, arXiv abstract, publisher page, PDF text, or official repo/docs) during this session, unless marked UNVERIFIED. |
| Earlier doc | aien-architecture PR #58, `docs/plans/mixed-algebra/research/PRIOR_ART.md` on branch `docs/mixed-algebra`. Note: the path given in the brief (`docs/mixed-algebra/research/...`) does not exist; the file is under `docs/plans/`. |

## 0. Reused from the earlier doc (by reference, not re-derived)

The earlier survey's corrected entries are reused as-is for the ternary kernel row:
- BitNet b1.58 (Ma et al., arXiv:2402.17764): VERIFIED there.
- T-MAC (Wei et al., EuroSys 2025, arXiv:2407.00088): its **[fixed]** citation. Do not reuse Gemini's original "Xie", "USENIX ATC 24" or "2x to 11x" claims.
- bitnet.cpp (github.com/microsoft/BitNet): VERIFIED there. The TL1/TL2 internals remain unverified.
Errors from that doc that must not be repeated anywhere: HERMES figures, RNSnet numbers, invented Res-DNN authors, Pedersoli TCSI 2018 (does not exist).

## 1. Turing component to prior-art map (short form)

| Turing component | Closest prior art (see matrix) |
|---|---|
| One stable semantic identity per operation | Git object model, Unison (hash of syntax tree, names as metadata) |
| Field of alternative realizations | PetaBricks (algorithmic choice), SPIRAL (algorithm x implementation), StarPU (per-task CPU/GPU implementations), HPVM (CPU/GPU/vector/FPGA from one IR) |
| Verified equivalence of alternatives | egg / equality saturation (rewrite-proven equality), SPIRAL (math rule derivation), Arco (algebraically equivalent analog configs) |
| Number system / algebra axis | ApproxHPVM (accuracy-aware approximate choices), DIANA + HTVM (layer mapping across 8-bit digital and ~1.5-bit-weight analog), BitNet/T-MAC (ternary kernels) |
| Hardware axis incl. analog | IREE, TVM, HPVM, IRIS (ORNL), DIANA/HTVM/DORY, Legno/Arco |
| Measured, workload-dependent selection | StarPU history-based models, Ansor/MetaSchedule/Halide learned cost models, SPIRAL feedback search, PetaBricks autotuner |
| Shadow observation plane | Dapper (sampled tracing), LTTng (low-impact kernel+user tracing, flight recorder), eBPF |
| Provenance, receipts, content addressing | Git, SLSA provenance, Atomic (semantic change graph with intent and AI attribution) |
| Fabric: semantic transport between humans, LLMs, compilers | MLIR dialects (compiler side), GraalVM polyglot interop (language side), CIPHER / Cache-to-Cache / LatentMAS (LLM latent side), emergent communication research |
| RSI loop learning from evidence | Learned cost models retrained on measurements (Ansor, MetaSchedule, Halide 2019), StarPU runtime model adaptation |

## 2. Matrix

Columns: **Solves** = what it already solves. **Reuse** = what to reuse conceptually. **Don't reinvent** = what not to rebuild. **Turing beyond** = what Turing proposes that it does not do.

### 2.1 Identity, provenance, content addressing

| System | Solves | Reuse | Don't reinvent | Turing beyond |
|---|---|---|---|---|
| Git object model | Immutable content-addressed blobs/trees/commits/tags; commits form a DAG of history | Hash-of-content identity; immutable objects; DAG of parents | Object store, hashing, packing, DAG history | Objects are contracts, realizations and measurement receipts, not files |
| Unison | Each definition identified by a hash of its syntax tree; names are metadata; dependencies pinned by hash | Identity = normalized body, names separate (Omega program id v2 already does this) | AST-hash identity scheme | Identity binds a *contract*, then many realizations across languages/algebras share it; Unison hashes one implementation |
| SLSA provenance | Standard attestation of how an artifact was built: builder, resolved inputs with digests, parameters (in-toto format) | Receipt field layout; builder identity; input digests | A new provenance schema from scratch | Receipts that carry runtime energy/latency/accuracy evidence and feed selection |
| Atomic (atomicdotdev/atomic) | Exists. Apache-2.0 VCS by Atomic Software, Co. "Semantic Change Graph" tracking intent, provenance (goal to exploration to commitment to verification), AI attribution (model, provider, tokens, cost); patch-theory DAG; cites SLSA. Early stage (about 94 stars when opened). | Intent + decision graph as first-class; agent-session attestation | Agent-intent provenance for *code changes* | Atomic tracks source changes; it does not select or measure runtime realizations |

### 2.2 Multi-language, multi-level IR, verified equivalence

| System | Solves | Reuse | Don't reinvent | Turing beyond |
|---|---|---|---|---|
| GraalVM Truffle / polyglot | Languages as AST interpreters, partially evaluated to fast code; standardized interop messages let JS, Python, Ruby, R, LLVM langs share values in one VM | Interop protocol as a message set every language implements | A cross-language value-sharing runtime | Treat languages as interchangeable *realizations* of one op, chosen by measurement; Truffle runs whatever language you wrote |
| MLIR | Extensible multi-level IR with dialects; lowers across abstraction levels and hardware | Dialect idea for versioned machine dialects; progressive lowering | A general IR framework and lowering infra | Dialects as a transport language between humans/LLMs/compilers with round-trip tests; MLIR has no measured-selection or evidence layer |
| IREE | MLIR-based end-to-end compiler + runtime for ML models on CPU and GPUs (Vulkan, CUDA, ROCm, Metal) and other accelerators | Split of scheduling logic from execution; HAL-style device abstraction | An ML model to multi-device compiler | Selection across algebras and analog; runtime evidence loop |
| HPVM | One hierarchical dataflow IR / virtual ISA targeting GPUs, vector units, multicore CPUs, FPGAs (PPoPP 2018) | Single portable graph as the stable thing, hardware as a lowering choice | Portable heterogeneous IR | Many *different algorithms and number systems* per op, not one program lowered many ways |
| egg (equality saturation) | E-graphs compactly hold many equivalent programs; rebuilding + e-class analyses make it fast and extensible (POPL 2021) | Field = e-class of equivalent realizations; extraction = selection | Rewrite engine, e-graph data structure | Extraction driven by *measured* cost with receipts, and equivalence across algebras where exactness is contract-bounded rather than exact |
| SPIRAL | Generates DSP transform code from math rules; feedback-driven search over algorithm and implementation choices; beats hand-tuned libs (Proc. IEEE 2005) | Proof-carrying derivation of alternatives from rules; search with measurement feedback | Rule-based algorithm space for linear transforms | Generalizes beyond linear transforms and beyond one number system per run |
| PetaBricks | Algorithmic choice as a language construct; autotuner picks algorithms and parameters, including accuracy targets for iterative methods (PLDI 2009) | Multiple implementations of one spec as the natural program shape; accuracy-aware tuning | Language-level algorithmic choice + autotuning | Choice spans languages, algebras and hardware; decisions recorded as auditable receipts |

### 2.3 Heterogeneous runtimes and measured selection

| System | Solves | Reuse | Don't reinvent | Turing beyond |
|---|---|---|---|---|
| StarPU | Tasks with per-device implementations over CPU+GPU; scheduler uses auto-tuned per-task performance models from a pre-calibration run, past executions, or live adaptation | History-based cost model per (task, device, size); data-transfer-aware scheduling | Heterogeneous task scheduler with measured models | Adds algebra axis and accuracy/energy contracts; evidence kept as signed receipts |
| IRIS (ORNL, Kim/Lee/Johnston/Vetter, HPEC 2021) | One runtime managing CUDA, HIP, OpenCL, OpenMP, Level Zero, Hexagon at once; dependencies, proactive data movement, configurable scheduling | Multi-backend runtime with pluggable policy | Multi-programming-system runtime | Measured, contract-checked selection; repo page did not show history-based models (not confirmed either way) |
| TVM | End-to-end DL compiler with learned cost model for code search; CPU, GPU, FPGA-based accelerator (arXiv 1802.04799) | Learned cost model trained on measured runs | Operator-level autotuning infra | Selection among algebras, not only schedules of one algebra |
| Ansor | Samples programs from a hierarchical space, evolutionary search, learned cost model, task scheduler across subgraphs (OSDI 2020) | Search + learned model + measurement loop | Schedule search for tensor programs | Same loop, but over semantically distinct realizations with receipts |
| MetaSchedule | Probabilistic-program abstraction for tensor schedule spaces, learning-driven search (NeurIPS 2022) | Composable stochastic choice points | Search-space DSL | Choice points across language/algebra/hardware |
| Halide autoscheduler 2019 | Beam search over large schedule space with ML cost model trained on random programs (TOG 38(4)) | Training a cost model from synthetic programs | Schedule autoscheduler | Same |
| ApproxHPVM | Turns app-level accuracy targets into per-op accuracy needs; maps error-tolerant ops to approximate hardware; 1-9x speed, 1.1-11.3x energy (OOPSLA 2019) | Accuracy contract on the op, not the kernel; hardware-agnostic accuracy tuning | Accuracy-aware approximation IR | Explicit number systems (ternary, RNS, Z3 phase) as realizations under the same contract |

### 2.4 Analog and mixed digital/analog

| System | Solves | Reuse | Don't reinvent | Turing beyond |
|---|---|---|---|---|
| Arco | From differential equations, synthesizes analog device configs algebraically equivalent to the spec (PLDI 2016) | Algebraic-equivalence check for analog realizations | Analog configuration synthesis | Analog is one member of a Field competing with digital by measurement |
| Legno | First compiler to a physical programmable analog device (HCDCv2); models noise, quantization, manufacturing variation; 0.50-5.92 ms, 0.28-5.67 uJ on 12 benchmarks (ASPLOS 2020) | Noise-aware compilation; per-device calibration | Analog compile pipeline | Contract-bounded error lets analog and digital be swapped |
| DIANA SoC | RISC-V + 8-bit digital DNN accelerator + analog in-memory core (7/1.5/6-bit input/weight/output); 600 TOP/s/W AIMC peak, 14 TOP/s/W digital, ~7 and ~5.6 TOP/s/W system (JSSC 2023, Houshmand et al.) | Real precedent for running ternary-ish weights on analog next to 8-bit digital in one system | Mixed-precision digital/analog layer mapping | General op-level Field, not a fixed per-layer mapping |
| DORY | Constraint-programming tiling for MCU L1 memory, generates C (IEEE TC 2021) | Memory-constrained tiling as a solver problem | MCU deployment tiling | n/a (tooling) |
| HTVM (TVM + DORY) | Deploys MLPerf Tiny on DIANA across CPU, digital and analog accelerators, 120x over plain TVM (DAC 2023) | Proof that one compiler can dispatch across digital and analog cores | Heterogeneous tinyML dispatch | Measured, evolving choice rather than compile-time rules |

### 2.5 Shadow plane (low-overhead observation)

| System | Solves | Reuse | Don't reinvent | Turing beyond |
|---|---|---|---|---|
| Dapper | Production tracing with sampling and instrumentation in a few common libraries; low overhead, app transparency (Google tech report 2010) | Sampling; trace trees; instrument the shared layer, not every app | Span/trace model; sampling policy | Traces become evidence that changes selection, not only debugging data |
| LTTng | Low-impact kernel + user tracing, reentrant, NMI-safe tracepoints, flight-recorder mode (OLS 2006) | Lock-free per-CPU buffers; flight recorder for post-hoc explanation | Ring-buffer tracer | Semantic events keyed by operation identity |
| eBPF | Verified, JIT-compiled sandboxed programs at kernel hook points with maps; used for tracing/observability | Verifier-gated observers; hook points | Safe in-kernel probes (on Linux) | AIENOS is not Linux, so the idea transfers, the implementation does not |

### 2.6 Fabric (semantic transport)

| System | Solves | Reuse | Don't reinvent | Turing beyond |
|---|---|---|---|---|
| Emergent communication survey (Lazaridou and Baroni, arXiv 2006.02419, 2020) | Surveys agents learning their own protocols; both the science and the practical side | Warning: emergent codes drift and can be opaque to humans | Learned-protocol research | Versioned, frozen dialects with round-trip tests to human-readable form |
| CIPHER, "Let Models Speak Ciphers" (Pham et al., ICLR 2024) | LLMs debate via expected output embeddings instead of sampled tokens; +0.5-5.0% over NL debate | Skip token sampling when both ends are models | Embedding-level debate | Auditability: every compressed message decodable and round-trip checked |
| Cache-to-Cache (Fu et al., ICLR 2026, arXiv 2510.03215) | Projects and fuses one LLM's KV-cache into another with learned gates; latest version reports +3.1-5.4% over text and about 2.5x lower latency (v1 said 3.0-5.0% and 2.0x) | KV-level transfer between different models | Latent inter-LLM transfer | Same as above; C2C messages are not human-checkable |
| LatentMAS (Zou et al., arXiv 2511.20639, ICML 2026 per the abstract page) | Training-free multi-agent collaboration via last-layer hidden states and shared latent memory; 70.8-83.7% fewer output tokens, 4-4.3x faster | Shared latent working memory | Latent multi-agent collaboration | Links LLM-side transport to compiler-side IR and evidence receipts |

## 3. Discrepancies and open verification items

- DIANA: the imec record opened is the JSSC 2023 journal paper (first author Houshmand). The ISSCC 2022 paper (Ueyoshi et al.) appears only in search snippets: UNVERIFIED.
- MLIR: arXiv 2002.11054 (2020) opened. The CGO 2021 venue was not opened: UNVERIFIED.
- StarPU: the CCPE PDF opened is the preprint (header says 2009); the published issue year and volume were not opened: UNVERIFIED.
- Cache-to-Cache: numbers changed between arXiv versions; quote the latest (v revised 2026-03-02) or name the version.
- IRIS is ambiguous. Relevant here: ORNL IRIS runtime (HPEC 2021). Not relevant: Iris separation logic (iris-project.org, Rocq proofs of concurrent programs), except as a possible tool for proving contracts later. No IRIS reference was found in local Turing notes to disambiguate further.
- MLIR round-trip (print/parse) testing as a convention: not checked in a primary source: UNVERIFIED.
- Truffle: SPLASH 2013 page opened (Onward! 2013, DOI 10.1145/2509578.2509581, Wurthinger et al.); abstract not shown there, content taken from GraalVM docs.

## 4. Sources opened in this session

1. Git internals, objects: https://git-scm.com/book/en/v2/Git-Internals-Git-Objects
2. Unison, the big idea: https://www.unison-lang.org/docs/the-big-idea/
3. SLSA provenance v1.0: https://slsa.dev/spec/v1.0/provenance
4. Atomic repo: https://github.com/atomicdotdev/atomic
5. Truffle, Onward! 2013: https://2013.splashcon.org/details/onward-2013-papers/6/One-VM-to-Rule-Them-All ; GraalVM polyglot docs: https://www.graalvm.org/latest/reference-manual/polyglot-programming/
6. MLIR: https://arxiv.org/abs/2002.11054
7. IREE: https://iree.dev/
8. HPVM, PPoPP 2018 pp. 68-80, DOI 10.1145/3178487.3178493: https://research.cs.wisc.edu/arch/uwarch-wiki2/index.php/PubsUWArch/B2hd-DBLPconfppoppKotsifakouSSKAA18
9. ApproxHPVM, OOPSLA 2019, DOI 10.1145/3360612: https://2019.splashcon.org/details/splash-2019-oopsla/67/ApproxHPVM-A-Portable-Compiler-IR-for-Accuracy-Aware-Optimizations
10. egg, POPL 2021: https://arxiv.org/abs/2004.03082
11. SPIRAL, Proc. IEEE 93(2) 2005: https://spiral.net/doc/papers/IEEE_2005.pdf
12. PetaBricks, PLDI 2009: https://dspace.mit.edu/handle/1721.1/62300
13. StarPU, CCPE preprint: https://www.par.univie.ac.at/project/peppher/publications/Published/ccpe10.pdf
14. IRIS (ORNL), HPEC 2021: https://impact.ornl.gov/en/publications/iris-a-portable-runtime-system-exploiting-multiple-heterogeneous-/ ; repo https://github.com/ORNL/iris ; Iris logic https://iris-project.org/
15. TVM: https://arxiv.org/abs/1802.04799
16. Ansor, OSDI 2020: https://arxiv.org/abs/2006.06762
17. MetaSchedule, NeurIPS 2022: https://arxiv.org/abs/2205.13603
18. Halide autoscheduler, TOG 38(4) 2019: https://halide-lang.org/papers/autoscheduler2019.html
19. Arco, PLDI 2016: https://pldi16.sigplan.org/details/pldi-2016-papers/7/Configuration-Synthesis-for-Programmable-Analog-Devices-with-Arco
20. Legno, ASPLOS 2020: https://dspace.mit.edu/handle/1721.1/130464
21. DIANA, JSSC 2023: https://imec-publications.be/handle/20.500.12860/59550
22. DORY, IEEE TC 2021: https://arxiv.org/abs/2008.07127
23. HTVM, DAC 2023: https://arxiv.org/abs/2406.07453
24. Dapper, 2010: https://research.google/pubs/dapper-a-large-scale-distributed-systems-tracing-infrastructure/
25. LTTng, OLS 2006: https://www.kernel.org/doc/ols/2006/ols2006v1-pages-209-224.pdf
26. eBPF: https://ebpf.io/what-is-ebpf/
27. Emergent communication survey: https://arxiv.org/abs/2006.02419
28. CIPHER: https://arxiv.org/abs/2310.06272
29. Cache-to-Cache: https://arxiv.org/abs/2510.03215
30. LatentMAS: https://arxiv.org/abs/2511.20639
31. BitNet b1.58, T-MAC, bitnet.cpp: by reference to PR #58 (verified there, not reopened here).

## 5. Verdict (blunt)

**Fully covered by prior art. Build on it, do not claim it.**
- Stable identity by content hash, names as metadata: Git + Unison. Omega program id v2 is already this.
- Content-addressed receipts and provenance: Git + SLSA. Agent-intent provenance for code: Atomic.
- A set of alternative implementations for one operation: PetaBricks, SPIRAL, StarPU codelets, e-graphs.
- Measured, workload-dependent selection with learned or history-based cost models: StarPU, Ansor, MetaSchedule, Halide, TVM, SPIRAL.
- Accuracy-contract-driven choice of approximate realizations: ApproxHPVM, PetaBricks.
- Heterogeneous CPU/GPU/FPGA dispatch from one representation: HPVM, IREE, IRIS, TVM.
- Analog compilation and mixed digital/analog dispatch: Arco, Legno, DIANA + HTVM.
- Ternary LLM kernels on CPU: BitNet b1.58, bitnet.cpp, T-MAC.
- Low-overhead tracing with post-hoc replay: Dapper, LTTng, eBPF.
- Model-to-model communication below the token level: CIPHER, Cache-to-Cache, LatentMAS.

**Integration only (real work, not a research claim).**
- Putting language, algorithm, number system and hardware on *one* Field keyed by one contract id. Each axis exists; nobody found here spans all four at once, but the joining is engineering.
- Making Shadow traces the input to selection. Tracing tools exist; autotuners measure; wiring them is integration.
- Using MLIR-style versioned dialects as the Fabric wire format.

**Plausibly novel (narrow, and only if demonstrated).**
1. A closed, auditable evidence loop across language x algebra x hardware: every selection decision cites content-addressed receipts (measured energy, latency, accuracy) against a contract, and can be replayed. Autotuners keep cost models, not signed, replayable decision records; provenance systems record builds or code changes, not runtime realization choices.
2. Number system as a first-class, contract-verified selection axis (int8, BF16, FP4, balanced-ternary bitplanes, RNS, Z3 phase, analog) in one Field. ApproxHPVM and DIANA touch precision, but not distinct algebras with per-realization equivalence contracts.
3. A human/LLM/compiler Fabric whose compressed dialects must pass round-trip tests to a human-readable form. The latent-communication papers optimize accuracy and speed and give up readability; none found enforces decodable, versioned messages.

None of these is a single new mechanism. The kill test says: Turing survives only as the integration plus the closed evidence loop, and only once a measured result shows the loop picks better realizations than a plain autotuner (for example Ansor or StarPU-style history models) on the same workload. Until then, describe it as "a system that combines known parts", not as new theory.
