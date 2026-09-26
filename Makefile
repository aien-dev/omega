CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic -D_GNU_SOURCE -O2 -Isrc
OUT_DIR ?= build

SRCS = src/sha256.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_exec.c src/omega_self_host.c src/omega_verify.c src/omega_program.c src/omega_synthesis.c src/omega_library.c src/omega_discovery.c src/omega_machine.c src/omega_realize_synth.c src/omega_matvec.c src/omega_accelerator.c tools/omegatool.c
OBJS = $(patsubst %.c,$(OUT_DIR)/%.o,$(notdir $(SRCS)))
TARGET = $(OUT_DIR)/omegatool

.PHONY: all clean test test-m5 test-m6 test-m7 test-m8 test-m9 test-m10 test-m11 test-m12 test-m13 test-m14 test-m15

all: $(TARGET)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/%.o: src/%.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/omegatool.o: tools/omegatool.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

test: $(TARGET)
	./$(TARGET) --run-gates
	./$(TARGET) --demonstrate-arithmetic
	./$(TARGET) --demonstrate-physics

test-m5: $(TARGET)
	./$(TARGET) --run-m5-gates
	./$(TARGET) --demonstrate-realization

test-m6: $(TARGET)
	./$(TARGET) --run-m6-gates
	./$(TARGET) --demonstrate-self-host

test-m7: $(TARGET)
	./$(TARGET) --run-m7-gates
	./$(TARGET) --demonstrate-verify

test-m8: $(TARGET)
	./$(TARGET) --run-m8-gates
	./$(TARGET) --demonstrate-program

test-m9: $(TARGET)
	./$(TARGET) --run-m9-gates
	./$(TARGET) --demonstrate-synthesis

test-m10: $(TARGET)
	./$(TARGET) --run-m10-gates
	./$(TARGET) --demonstrate-library

test-m11: $(TARGET)
	./$(TARGET) --run-m11-gates
	./$(TARGET) --demonstrate-discovery

test-m12: $(TARGET)
	./$(TARGET) --run-m12-gates
	./$(TARGET) --demonstrate-living-matvec

test-m13: $(TARGET)
	./$(TARGET) --run-m13-gates
	./$(TARGET) --demonstrate-machine

test-m14: $(TARGET)
	./$(TARGET) --run-m14-gates
	./$(TARGET) --demonstrate-realization-synthesis

test-m15: $(TARGET)
	./$(TARGET) --run-m15-gates
	./$(TARGET) --demonstrate-accelerator

clean:
	rm -rf $(OUT_DIR)
