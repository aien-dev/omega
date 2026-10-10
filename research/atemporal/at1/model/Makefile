# AT-1 candidate engine (Agent 3). POSIX make: works with GNU Make 3.81 (macOS) and later.
# Only cc, libc and libm. Override the compiler with: make CC=clang
CC = cc
CFLAGS = -std=c11 -O2 -ffp-contract=off -Wall -Wextra -Werror -pedantic
SANFLAGS = -std=c11 -O1 -g -ffp-contract=off -Wall -Wextra -Werror -pedantic -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
LDLIBS = -lm
SHA = ../../../../src
LIB = at1_bn.c at1_case.c at1_exact.c at1_numeric.c at1_result.c at1_io.c $(SHA)/sha256.c
HDR = at1_model.h $(SHA)/sha256.h

all: build/at1-model build/test_model

build/at1-model: $(LIB) at1_main.c $(HDR)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(SHA) -o build/at1-model $(LIB) at1_main.c $(LDLIBS)

build/test_model: $(LIB) tests/test_model.c $(HDR)
	mkdir -p build
	$(CC) $(CFLAGS) -I$(SHA) -I. -o build/test_model $(LIB) tests/test_model.c $(LDLIBS)

test: all
	./build/test_model tests/cases build/at1-model

# Sanitizer run, only where the compiler supports the flags (probed first; skipped otherwise).
asan:
	mkdir -p build
	@if echo 'int main(void){return 0;}' > build/probe.c && $(CC) $(SANFLAGS) -o build/probe build/probe.c 2>/dev/null && ./build/probe; then \
	  $(CC) $(SANFLAGS) -I$(SHA) -o build/at1-model-asan $(LIB) at1_main.c $(LDLIBS) && \
	  $(CC) $(SANFLAGS) -I$(SHA) -I. -o build/test_model-asan $(LIB) tests/test_model.c $(LDLIBS) && \
	  ASAN_OPTIONS=detect_leaks=1 ./build/test_model-asan tests/cases build/at1-model-asan; \
	else echo "asan: -fsanitize=address,undefined not supported by $(CC) here; skipped"; fi

clean:
	rm -rf build

.PHONY: all test asan clean
