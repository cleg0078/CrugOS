# Host-side unit tests for freestanding kernel/loader logic.
CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-builtin
CPPFLAGS := -I../../include -I../../boot/efi -DCRUG_HOST_TEST=1

BUILD := build
BIN := $(BUILD)/unit-tests

SOURCES := main.c test_string.c test_printf.c test_elf.c test_bitmap.c test_vm_math.c test_handle.c \
           test_crc32c.c test_blake2b.c test_aes.c test_argon2.c test_crypt.c \
           test_lineedit.c test_input.c test_net.c test_dns.c test_usb.c test_audio.c \
           test_gfx.c test_intuition.c test_textgrid.c test_shell.c test_harden.c \
           ../../drivers/usb/usb_hid.c \
           ../../kernel/audio/audio.c ../../kernel/audio/mixer.c \
           ../../kernel/gfx/gfx.c ../../kernel/gfx/intuition.c \
           ../../kernel/input/input.c ../../kernel/net/net_util.c \
           ../../kernel/net/dns.c \
           ../../kernel/lib/string.c ../../kernel/lib/printf.c ../../kernel/lib/bitmap.c \
           ../../kernel/lib/crc32c.c ../../kernel/lib/lineedit.c \
           ../../kernel/lib/textgrid.c ../../kernel/shell/shell.c \
           ../../kernel/lib/harden.c \
           ../../kernel/crypto/blake2b.c \
           ../../kernel/crypto/aes.c ../../kernel/crypto/xts.c ../../kernel/crypto/aesni.S \
           ../../kernel/crypto/argon2id.c ../../kernel/block/crypt_meta.c \
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
