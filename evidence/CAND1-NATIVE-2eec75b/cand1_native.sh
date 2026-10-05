#!/usr/bin/env bash
# L5-NATIVE CAND-1 native qualification: one command, build + chip run.
#
#   bash cand1_native.sh <sovereign-core sha (full 40 hex)>
#
# 1. Clean detached checkout of that sha (fresh worktree, refused if dirty or HEAD differs),
#    omega at its omega.lock, physics at omega's physics.lock, each in its own fresh worktree.
# 2. Release build of aien-cli (scripts/repro-build.sh when the sha has it, else
#    cargo build --release --locked) and release test binaries, AIEN_DEV_FALLBACK unset.
#    Refuses to continue if the Omega engine was not linked (stub build).
# 3. Chip steps: identity, zero_cuda, strict_real_model (gate, vs reference, vs HF oracle),
#    runtime GB10 e2e tests incl. the native paged_attention_batch case, daemon positive turn,
#    daemon missing checkpoint, identity_after.
# Output: ~/workspace/evidence-out/CAND1-NATIVE-<sha7>/ (batch.log, results.txt, per-step logs,
# receipts). Every step runs even if an earlier one failed. Nothing is retried, killed or timed
# out. Change from the CAND-0 batch: the daemon exit code is captured (CAND-0 got 127 because
# gpu_procs overwrote the caller's `pid` variable; bash functions share non-local variables).
set -u
SHA=${1:?usage: cand1_native.sh <sovereign-core sha>}
[[ $SHA =~ ^[0-9a-f]{40}$ ]] || { echo "need a full 40-hex sha, got '$SHA'"; exit 2; }
S7=${SHA:0:7}
O=$HOME/workspace/overnight-1005
E=$HOME/workspace/evidence-out/CAND1-NATIVE-$S7
SC=$O/wt-L5-NATIVE-cand1-$S7-aien-sovereign-core
TGT=$O/wt-L5-NATIVE-targets/cand1-$S7
HF=$HOME/.cache/huggingface/hub/models--TinyLlama--TinyLlama-1.1B-Chat-v1.0/snapshots/fe8a4ea1ffedaf415f4da2f062534de366a451e6
BIN=$E/bin
unset AIEN_DEV_FALLBACK AIEN_FORCE_CPU_STUB AIEN_OMEGA_GPU_LIB
export RUST_BACKTRACE=1 CARGO_BUILD_JOBS=6 CARGO_TARGET_DIR=$TGT
mkdir -p "$E" "$BIN" "$E/lib"
cp "$0" "$E/cand1_native.sh"
: > "$E/results.txt"

step() {
	local name=$1
	shift
	echo "=== STEP $name start $(date -u +%FT%TZ)"
	"$@" > "$E/$name.log" 2>&1
	local rc=$?
	echo "=== STEP $name rc=$rc end $(date -u +%FT%TZ)"
	echo "$name rc=$rc" >> "$E/results.txt"
	grep -E "test result|ORACLE_GATE (step0|teacher|verdict|receipt|NATIVE)|OMEGA_GATE (teacher|verdict|tokens_match)|STRICT_GATE verdict|GB10_PAGED_BATCH|OP_CALLS|ZERO_CUDA|DAEMON_|BUILD_|CHECKOUT_|MISSING|panicked" "$E/$name.log" | head -40
	return $rc
}

# Fresh detached worktree of <repo> at <sha>; refuses an existing dirty or mismatched one.
checkout() {
	local repo=$1 sha=$2 dir=$3
	git -C "$repo" fetch -q origin || return 1
	git -C "$repo" cat-file -e "$sha^{commit}" || { echo "CHECKOUT_FAIL $sha not in $repo"; return 1; }
	[ -e "$dir" ] || git -C "$repo" worktree add -q --detach "$dir" "$sha" || return 1
	local head dirty
	head=$(git -C "$dir" rev-parse HEAD)
	dirty=$(git -C "$dir" status --porcelain --untracked-files=no)
	echo "CHECKOUT $dir head=$head dirty=[${dirty}]"
	[ "$head" = "$sha" ] && [ -z "$dirty" ] || { echo "CHECKOUT_FAIL $dir not a clean checkout of $sha"; return 1; }
}

prepare() {
	checkout "$O/aien-sovereign-core" "$SHA" "$SC" || return 1
	local osha psha
	osha=$(tr -d '[:space:]' < "$SC/omega.lock")
	checkout "$O/omega" "$osha" "$O/wt-L5-NATIVE-omega-${osha:0:7}" || return 1
	psha=$(tr -d '[:space:]' < "$O/wt-L5-NATIVE-omega-${osha:0:7}/physics.lock")
	checkout "$O/physics" "$psha" "$O/wt-L5-NATIVE-physics-${psha:0:7}" || return 1
	export AIEN_OMEGA_DIR=$O/wt-L5-NATIVE-omega-${osha:0:7} AIEN_PHYSICS_DIR=$O/wt-L5-NATIVE-physics-${psha:0:7}
	cd "$SC" || return 1
	if [ -x scripts/repro-build.sh ]; then
		echo "BUILD_CLI scripts/repro-build.sh -p aien-cli"
		nice -n 10 scripts/repro-build.sh -p aien-cli || return 1
	else
		echo "BUILD_CLI cargo build --release --locked -p aien-cli"
		nice -n 10 cargo build --release --locked -p aien-cli || return 1
	fi
	# The engine must really be linked: build.rs sets cfg has_omega_gpu only then.
	local out
	out=$(grep -l has_omega_gpu "$TGT"/release/build/aien-omega-gpu-*/output 2>/dev/null | head -1)
	[ -n "$out" ] || { echo "BUILD_FAIL Omega engine not linked (stub build)"; return 1; }
	cp "$(dirname "$out")/out/omega-build/libomega_gpu.a" "$E/lib/" || return 1
	cp "$TGT/release/aien-cli" "$BIN/aien-cli" || return 1
	local j
	for spec in "aien-inference-runtime strict_real_model" "aien-runtime runtime_end_to_end_tests"; do
		set -- $spec
		j=$(nice -n 10 cargo test --release --locked --no-run --message-format=json -p "$1" --test "$2") || return 1
		exe=$(printf '%s\n' "$j" | jq -r "select(.reason==\"compiler-artifact\" and .executable!=null and .target.name==\"$2\") | .executable" | tail -1)
		[ -n "$exe" ] || { echo "BUILD_FAIL no executable for $2"; return 1; }
		cp "$exe" "$BIN/$2" || return 1
	done
	for t in omega_vs_hf_oracle strict_real_model_gate omega_vs_reference_real_model; do
		"$BIN/strict_real_model" --list --ignored | grep -q "^$t: test" || echo "MISSING test $t in strict_real_model (sha lacks it)"
	done
	"$BIN/runtime_end_to_end_tests" --list | grep -q "^gb10_decode_batch_runs_paged_attention_batch_natively: test" ||
		echo "MISSING test gb10_decode_batch_runs_paged_attention_batch_natively (sha lacks it)"
	sha256sum "$BIN"/* "$E"/lib/libomega_gpu.a
	echo "BUILD_OK sovereign-core=$SHA omega=$osha physics=$psha"
}

gpu_procs() {
	local p gp
	echo "## nvidia-smi compute apps"
	nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv 2>&1
	echo "## processes holding /dev/nvidia*"
	for p in /proc/[0-9]*; do
		if ls -l "$p/fd" 2>/dev/null | grep -q '/dev/nvidia'; then
			gp=${p#/proc/}
			echo "$gp $(ps -o etime=,rss=,args= -p "$gp" 2>/dev/null | cut -c1-200)"
		fi
	done
	echo "## load"
	uptime
}

identity() {
	date -u +%FT%TZ
	uname -a
	nvidia-smi
	nvidia-smi --query-gpu=name,uuid,driver_version,pci.bus_id,compute_cap --format=csv
	gpu_procs
	echo "## git"
	echo "sovereign-core: $(git -C "$SC" rev-parse HEAD) dirty=[$(git -C "$SC" status --porcelain --untracked-files=no)]"
	echo "omega: $(git -C "$AIEN_OMEGA_DIR" rev-parse HEAD) physics: $(git -C "$AIEN_PHYSICS_DIR" rev-parse HEAD)"
	echo "## sha256"
	sha256sum "$BIN"/* "$HF/model.safetensors" "$HF/tokenizer.json" \
		"$SC/crates/aien-inference-abi/fixtures/tinyllama_oracle.safetensors" \
		"$SC/crates/aien-inference-abi/fixtures/tinyllama_oracle_manifest.json" "$E"/lib/libomega_gpu.a
}

zero_cuda() {
	bash scripts/zero-cuda-gate.sh --self-test || return 1
	local rc=0 b
	bash scripts/zero-cuda-gate.sh "$BIN"/* || rc=1
	for b in "$BIN"/*; do
		echo "## $b"
		readelf -d "$b" | grep NEEDED
		echo "undefined CUDA-like dynamic symbols:"
		nm -D --undefined-only "$b" | awk '{print $NF}' | grep -E '^(cu[A-Z]|cuda[A-Z]|cublas|nvrtc[A-Z]|__cuda)' && rc=1 || echo "  none"
		echo "CUDA library names in strings:"
		strings "$b" | grep -E 'lib(cuda|cudart|cublas|cublasLt|nvrtc|nvJitLink|cudnn)\.so' && rc=1 || echo "  none"
	done
	echo "## static archive libomega_gpu.a"
	nm "$E/lib/libomega_gpu.a" 2>/dev/null | awk '{print $NF}' | grep -E '^(cu[A-Z]|cuda[A-Z]|cublas|nvrtc[A-Z]|__cuda)' && rc=1 || echo "  none"
	return $rc
}

daemon_positive() {
	local sock=$E/daemon.sock log=$E/daemon-positive.stderr
	rm -f "$sock" "$E/daemon.pid" "$E/daemon.rc"
	# The daemon runs as the direct child of a small subshell that records its pid and its
	# exit code in files, so no shell variable or job-table detail can lose the exit code.
	(
		AIEN_GPU_BACKEND=omega AIEN_REQUIRE_CHECKPOINT=1 \
			AIEN_MODEL_PATH="$HF/model.safetensors" AIEN_TOKENIZER_PATH="$HF/tokenizer.json" \
			AIEN_RUNTIME_SOCK="$sock" "$BIN/aien-cli" --daemon > "$E/daemon-positive.stdout" 2> "$log" &
		echo $! > "$E/daemon.pid"
		wait $!
		echo $? > "$E/daemon.rc"
	) &
	local keeper=$!
	until [ -s "$E/daemon.pid" ]; do sleep 0.2; done
	local dpid
	dpid=$(cat "$E/daemon.pid")
	echo "DAEMON_PID $dpid"
	until [ -S "$sock" ] || [ -s "$E/daemon.rc" ]; do sleep 1; done
	if ! [ -S "$sock" ]; then
		wait "$keeper"
		echo "DAEMON_EXITED_BEFORE_SOCKET rc=$(cat "$E/daemon.rc")"
		cat "$E/daemon-positive.stdout" "$log"
		return 1
	fi
	cat "$E/daemon-positive.stdout"
	printf '{"protocol_version":1,"request_id":1,"operation_id":%s,"operator_session":1,"command":{"StreamTurn":{"messages":[{"role":"system","content":"You are a sovereign AI assistant."},{"role":"user","content":"Explain the role of an operating system in one sentence."}],"max_tokens":16,"temperature":0.0}}}\n' "$(date +%s%N)" |
		socat -t 86400 - "UNIX-CONNECT:$sock" > "$E/daemon-turn.jsonl"
	echo "DAEMON_TURN_SOCAT rc=$?"
	echo "## /proc/$dpid/maps libraries (CUDA libraries must be absent)"
	awk '{print $6}' "/proc/$dpid/maps" 2>/dev/null | grep -E '\.so|/dev/nvidia' | sort -u
	awk '{print $6}' "/proc/$dpid/maps" 2>/dev/null | grep -E 'lib(cuda|cudart|cublas|nvrtc|nvJitLink)' && echo "DAEMON_CUDA_MAPPED yes" || echo "DAEMON_CUDA_MAPPED no"
	gpu_procs
	printf '{"protocol_version":1,"request_id":2,"operation_id":%s,"operator_session":1,"command":"Shutdown"}\n' "$(date +%s%N)" |
		socat -t 86400 - "UNIX-CONNECT:$sock"
	wait "$keeper"
	local drc
	drc=$(cat "$E/daemon.rc" 2>/dev/null || echo missing)
	echo "DAEMON_EXIT rc=$drc"
	echo "## daemon stderr"
	cat "$log"
	echo "## turn"
	cat "$E/daemon-turn.jsonl"
	local ok=0
	grep -q '"TurnFinished"' "$E/daemon-turn.jsonl" && echo "DAEMON_TURN_FINISHED yes" || { echo "DAEMON_TURN_FINISHED no"; ok=1; }
	grep -q 'OmegaGb10Backend (native Omega engine, no CUDA, NVIDIA GB10 sm_121)' "$E/daemon-positive.stdout" && echo "DAEMON_BACKEND omega-native" || { echo "DAEMON_BACKEND not-omega"; ok=1; }
	grep -qE 'STRICT_REAL_MODEL_VIOLATION|panicked' "$log" && { echo "DAEMON_STRICT_VIOLATION yes"; ok=1; } || echo "DAEMON_STRICT_VIOLATION no"
	[ "$drc" = 0 ] || ok=1
	return $ok
}

daemon_missing_checkpoint() {
	AIEN_MODEL_PATH=/nonexistent/aien-l5/model.safetensors AIEN_RUNTIME_SOCK=$E/daemon-neg.sock \
		"$BIN/aien-cli" --daemon
	local rc=$?
	echo "DAEMON_MISSING_CHECKPOINT rc=$rc"
	[ "$rc" -ne 0 ] && ! [ -S "$E/daemon-neg.sock" ]
}

{
	echo "L5-NATIVE CAND-1 batch sha=$SHA $(date -u +%FT%TZ)"
	if step prepare prepare; then
		osha=$(tr -d '[:space:]' < "$SC/omega.lock")
		export AIEN_OMEGA_DIR=$O/wt-L5-NATIVE-omega-${osha:0:7}
		export AIEN_PHYSICS_DIR=$O/wt-L5-NATIVE-physics-$(tr -d '[:space:]' < "$AIEN_OMEGA_DIR/physics.lock" | cut -c1-7)
		cd "$SC" || exit 3
		step identity identity
		step zero_cuda zero_cuda
		step strict_gate env AIEN_E2E_CHECKPOINT="$HF" AIEN_STRICT_RECEIPT="$E/receipt-strict-gate.json" \
			"$BIN/strict_real_model" --ignored --nocapture --exact strict_real_model_gate
		step omega_vs_ref env AIEN_E2E_CHECKPOINT="$HF" AIEN_STRICT_RECEIPT="$E/receipt-omega-vs-ref.json" \
			"$BIN/strict_real_model" --ignored --nocapture --exact omega_vs_reference_real_model
		step omega_vs_oracle env AIEN_E2E_CHECKPOINT="$HF" AIEN_STRICT_RECEIPT="$E/receipt-omega-vs-oracle.json" \
			"$BIN/strict_real_model" --ignored --nocapture --exact omega_vs_hf_oracle
		step runtime_e2e_gb10 env AIEN_REQUIRE_RELEASE=1 "$BIN/runtime_end_to_end_tests" --nocapture --test-threads=1 \
			test_end_to_end_gb10_hardware_execution_if_available release_golden_path_records_whether_gb10_ran
		step paged_batch_gb10 "$BIN/runtime_end_to_end_tests" --nocapture --test-threads=1 \
			decode_batch_reaches_paged_attention_batch_on_reference gb10_decode_batch_runs_paged_attention_batch_natively
		# A skipped GB10 body exits 0; these two steps make a skip a FAIL.
		step gb10_bodies_ran sh -c '! grep -q "Skipping GB10" "$1" "$2"' _ "$E/runtime_e2e_gb10.log" "$E/paged_batch_gb10.log"
		step paged_batch_verdict grep -q "GB10_PAGED_BATCH verdict: PASS" "$E/paged_batch_gb10.log"
		step daemon_positive daemon_positive
		step daemon_missing_checkpoint daemon_missing_checkpoint
		step identity_after gpu_procs
	else
		echo "PREPARE FAILED: no chip step ran (NOT_RUN)"
	fi
	echo "## results"
	cat "$E/results.txt"
	echo "L5-NATIVE CAND-1 batch done $(date -u +%FT%TZ)"
} 2>&1 | tee "$E/batch.log"
