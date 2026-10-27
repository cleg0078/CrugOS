# Host-side unit tests for freestanding kernel/loader logic.
CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-builtin
CPPFLAGS := -I../../include -I../../boot/efi

BUILD := build
BIN := $(BUILD)/unit-tests

SOURCES := main.c test_string.c test_printf.c test_elf.c test_bitmap.c test_vm_math.c test_handle.c \
           test_crc32c.c \
           ../../kernel/lib/string.c ../../kernel/lib/printf.c ../../kernel/lib/bitmap.c \
           ../../kernel/lib/crc32c.c \
           ../../kernel/core/process.c \
           ../../boot/efi/elf.c

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SOURCES)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD)
