# R16 inventory fixture map (test data for tests/r16_inventory/run.sh)

<!-- r16-inventory:rows -->
| id | repo | path | line | symbol | old role | new role | class | authoritative | reason | evidence |
|---|---|---|---|---|---|---|---|---|---|---|
| FX-01 | omega | `src/legacy_oracle_demo.c` | 4 | `demo_main` | hand sequencer | legacy oracle | A | false | fixture: class A under a legacy_oracle name passes | `while (1) {` |
| FX-02 | omega | `src/world.c` | 8 | `worker_main` | worker | worker | E | true | fixture | `for (;;) {` |
| FX-03 | omega | `src/world.c` | 22 | `wait_ready` | readiness poll | hardware wait | E | true | fixture | `for (int i = 0; i < 100; i++) {` |
| FX-04 | omega | `tools/tool.c` | 4 | `main` | gate runner | maintenance | B | false | fixture | `if (argc > 1 && strcmp(argv[1], "--run-x-gates") == 0) return 0;` |
| FX-05 | aien-sovereign-core | `src/spine.rs` | 4 | `run_until_complete` | spine | LLM serving | E | false | fixture | `pub fn run_until_complete(max_steps: usize) {` |
| FX-06 | aien-sovereign-core | `src/spine.rs` | 5 | `run_until_complete` | spine loop | LLM serving | E | false | fixture | `for _ in 0..max_steps {` |
| FX-07 | aien-sovereign-core | `src/spine.rs` | 11 | `serve` | accept loop | protocol | F | false | fixture | `loop {` |
| FX-08 | aegis-runtime | `src/agent.rs` | 1 | `execute_task` | agent loop | retired by non-use | A | false | fixture: Rust-repo A is allowed (non-use) | `fn execute_task(max_turns: usize) {` |
| FX-09 | aegis-runtime | `src/agent.rs` | 2 | `execute_task` | agent loop | retired by non-use | A | false | fixture | `for turn in 1..=max_turns {` |
| FX-10 | aienos | `src/halt.rs` | 2 | `halt` | halt | halt | E | true | fixture | `loop {` |
| FX-11 | physics | `nvrm.c` | 4 | `wait_marker` | marker wait | hardware wait | E | true | fixture | `while (*m == 0) {` |
<!-- /r16-inventory:rows -->

<!-- r16-inventory:rows -->
| id | repo | path | line | symbol | old role | new role | class | authoritative | reason | evidence |
|---|---|---|---|---|---|---|---|---|---|---|
| FX-M1 | physics | `nvrm.c` | manual | `-` | manual row | manual row | C | true | fixture: manual row present | `int wait_marker(volatile int *m) {` |
<!-- /r16-inventory:rows -->
