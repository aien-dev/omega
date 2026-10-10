# AT-1 Agent 4 evaluator. Portable: GNU make 3.81 or later, POSIX sh, rustc
# (std only, no Cargo), a C compiler only for the isolation-gate controls.
RUSTC ?= rustc
CC ?= cc
BIN = build/at1-eval
SRC = src/main.rs src/big.rs src/rat.rs src/dd.rs src/sha256.rs src/text.rs src/case.rs \
      src/model.rs src/result.rs src/judge.rs src/verify.rs src/synth.rs src/mutants.rs \
      src/corpus.rs src/hidden.rs src/scan.rs src/qualify.rs

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p build
	$(RUSTC) --edition 2021 -O src/main.rs -o $(BIN)

build/at1-eval-test: $(SRC)
	mkdir -p build
	$(RUSTC) --edition 2021 -O --test src/main.rs -o build/at1-eval-test

# Unit tests, run-time self-test, corpus byte check, mutant receipt, gate controls.
test: $(BIN) build/at1-eval-test
	./build/at1-eval-test
	./$(BIN) selftest
	./$(BIN) corpus-check cases
	./$(BIN) mutants results/MUTANT_RECEIPT.tsv
	CC="$(CC)" sh gates/isolation.sh --controls ./$(BIN) results/ISOLATION_RECEIPT.txt

corpus: $(BIN)
	./$(BIN) corpus-gen cases

clean:
	rm -rf build

.PHONY: all test corpus clean
