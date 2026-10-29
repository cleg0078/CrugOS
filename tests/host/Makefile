# Host-side unit tests for freestanding kernel/loader logic.
CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-builtin
CPPFLAGS := -I../../include -I../../boot/efi -DCRUG_HOST_TEST=1

BUILD := build
BIN := $(BUILD)/unit-tests

SOURCES := main.c test_string.c test_printf.c test_elf.c test_bitmap.c test_vm_math.c test_handle.c \
           test_crc32c.c test_blake2b.c test_aes.c test_argon2.c test_crypt.c \
           test_lineedit.c test_input.c test_net.c test_dns.c test_usb.c test_audio.c \
           test_gfx.c test_intuition.c test_commodity.c test_app.c test_textgrid.c \
           test_shell.c test_harden.c \
           test_heap.c test_trap.c test_argv.c test_ufile.c \
           test_uaccess.c test_exec.c test_vm_space.c test_pkg.c test_gpu.c \
           ../../drivers/usb/usb_hid.c \
           ../../kernel/audio/audio.c ../../kernel/audio/mixer.c \
           ../../kernel/gfx/gfx.c ../../kernel/gfx/intuition.c \
           ../../kernel/gfx/commodities.c ../../kernel/gpu/gpu_util.c \
           ../../kernel/input/input.c ../../kernel/net/net_util.c \
           ../../kernel/net/dns.c \
           ../../kernel/lib/string.c ../../kernel/lib/printf.c ../../kernel/lib/bitmap.c \
           ../../kernel/lib/crc32c.c ../../kernel/lib/lineedit.c \
           ../../kernel/lib/textgrid.c ../../kernel/shell/shell.c \
           ../../kernel/lib/harden.c ../../kernel/lib/trapfmt.c \
           ../../kernel/mm/heap.c ../../kernel/mm/uaccess.c ../../kernel/mm/vm_space.c \
           ../../kernel/mm/cow.c \
           ../../kernel/crypto/blake2b.c \
           ../../kernel/crypto/aes.c ../../kernel/crypto/xts.c ../../kernel/crypto/aesni.S \
           ../../kernel/crypto/argon2id.c ../../kernel/block/crypt_meta.c \
           ../../kernel/exec/argv.c ../../kernel/fs/ufile.c \
           ../../kernel/core/process.c ../../kernel/app/pkg.c \
           ../../kernel/lib/caps.c ../../kernel/lib/perms.c \
           ../../kernel/crypto/sha512.c ../../kernel/crypto/ed25519.c \
           ../../kernel/app/manifest.c ../../boot/efi/elf.c

.PHONY: all run clean fuzz

all: $(BIN)

$(BIN): $(SOURCES)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

run: $(BIN)
	./$(BIN)

# The mount-path fuzzer is a separate binary: it installs its own stubs for the
# physical memory manager and the kernel panic hook and links only the storage
# stack, so it must not share the unit-test link line.
FUZZ_BIN := $(BUILD)/fuzz-mount
FUZZ_SOURCES := fuzz_mount.c ../../kernel/fs/ext4.c ../../kernel/fs/gpt.c ../../kernel/fs/journal.c ../../kernel/mm/heap.c ../../kernel/lib/string.c ../../kernel/lib/printf.c ../../kernel/lib/bitmap.c ../../kernel/lib/crc32c.c ../../kernel/lib/harden.c ../../kernel/block/block.c ../../kernel/block/crypt_meta.c ../../kernel/crypto/blake2b.c ../../kernel/crypto/argon2id.c ../../kernel/crypto/aes.c ../../kernel/crypto/xts.c ../../kernel/crypto/aesni.S

fuzz: $(FUZZ_BIN)

$(FUZZ_BIN): $(FUZZ_SOURCES)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(FUZZ_SOURCES)

clean:
	rm -rf $(BUILD)
