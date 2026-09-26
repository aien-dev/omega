CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic -D_GNU_SOURCE -O2 -Isrc
OUT_DIR ?= build

SRCS = src/sha256.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c tools/omegatool.c
OBJS = $(patsubst %.c,$(OUT_DIR)/%.o,$(notdir $(SRCS)))
TARGET = $(OUT_DIR)/omegatool

.PHONY: all clean test

all: $(TARGET)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/sha256.o: src/sha256.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/omega_canonical.o: src/omega_canonical.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/omega_validate.o: src/omega_validate.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/omega_core.o: src/omega_core.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/omega_codec.o: src/omega_codec.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/omegatool.o: tools/omegatool.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

test: $(TARGET)
	./$(TARGET) --run-gates
	./$(TARGET) --demonstrate-arithmetic
	./$(TARGET) --demonstrate-physics

clean:
	rm -rf $(OUT_DIR)
