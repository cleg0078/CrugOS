# CrugOS top-level build system.
#
#   make            build kernel, UEFI loader and ESP tree
#   make run        boot the ESP image under QEMU + OVMF
#   make test       boot and verify the serial markers / exit code
#   make test-host  build and run host unit tests
#   make test-compat  boot a compatibility build (no AVX2 required)
#   make disk       build the GPT/ext4 test disk
#   make check-disk run e2fsck and file checks on the test disk
#   make test-raid  boot RAID1 (two members) and verify member disks
#   make test-raid-degraded  boot RAID1 with one member missing
#   make size       enforce the kernel size budget
#   make check-efi  validate the loader as a PE32+ EFI application
#   make repro      verify the build is bit-for-bit reproducible
#   make preflight  check the local toolchain
#   make version    print the release version from VERSION
#
# Variables: ARCH=x86_64 MODE=release|debug|size V=1 REQUIRE_AVX2=1|0

SHELL := /bin/bash
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.SUFFIXES:

ARCH ?= x86_64
MODE ?= release
BUILD_ROOT ?= build
V ?= 0
REQUIRE_AVX2 ?= 1
QEMU_CPU ?= qemu64,+avx,+avx2
EXPECT_AVX2 ?= yes

CC := crugc
LD := ld
PYTHON := python3

# VERSION is the single source of truth for the release number.
CRUGOS_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)
VERSION_FLAGS := -DCRUG_VERSION_STR='"$(CRUGOS_VERSION)"'
AVX2_FLAGS := -DCRUG_REQUIRE_AVX2=$(REQUIRE_AVX2)

ifeq ($(V),1)
Q :=
else
Q := @
endif

ifeq ($(ARCH),x86_64)
LOADER_LD_ARCH := i386pep
LOADER_EFI_NAME := BOOTX64.EFI
else
$(error unsupported ARCH '$(ARCH)': only x86_64 is implemented)
endif

BUILD := $(BUILD_ROOT)/$(ARCH)
OBJ := $(BUILD)/obj
KERNEL := $(BUILD)/kernel.elf
KERNEL_MAP := $(BUILD)/kernel.map
LOADER := $(BUILD)/$(LOADER_EFI_NAME)
ESP := $(BUILD)/esp
SERIAL_LOG := $(BUILD)/serial.log
DEBUG_LOG := $(BUILD)/debug.log
QEMU_EXIT := $(BUILD)/exit-code.txt
DISK_IMG ?= $(BUILD)/disk.img
DISK_SIZE_MB ?= 16
RAID_DISK_A ?= $(BUILD)/raid-a.img
RAID_DISK_B ?= $(BUILD)/raid-b.img
EXTRA_DISKS ?=

ifeq ($(MODE),debug)
MODE_FLAGS := -Og -g3
else ifeq ($(MODE),size)
MODE_FLAGS := -Os -DNDEBUG
else ifeq ($(MODE),release)
MODE_FLAGS := -O2 -DNDEBUG
else
$(error unsupported MODE '$(MODE)': use release, debug or size)
endif

WARN_FLAGS := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
COMMON_FLAGS := -std=gnu11 -fcrug-kernel -ffreestanding -fno-pic -fno-pie \
                -fno-stack-protector -fno-ident -fno-asynchronous-unwind-tables \
                -fno-unwind-tables -fno-builtin -fno-omit-frame-pointer -mno-red-zone \
                -Iinclude -MMD -MP $(VERSION_FLAGS) $(AVX2_FLAGS) $(WARN_FLAGS) $(MODE_FLAGS)
KERNEL_FLAGS := $(COMMON_FLAGS) -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
                -mno-avx -mno-avx2 -mno-avx512f -fno-builtin
LOADER_FLAGS := $(COMMON_FLAGS) -fshort-wchar

KERNEL_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
                  -Wl,-T,kernel/arch/$(ARCH)/linker.ld -Wl,-Map,$(KERNEL_MAP)

KERNEL_SOURCES := $(wildcard kernel/arch/$(ARCH)/*.c) $(wildcard kernel/arch/$(ARCH)/*.S) \
                  $(wildcard kernel/core/*.c) $(wildcard kernel/lib/*.c) \
                  $(wildcard kernel/mm/*.c) $(wildcard kernel/block/*.c) \
                  $(wildcard kernel/fs/*.c) $(wildcard drivers/*/*.c)
KERNEL_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(basename $(KERNEL_SOURCES)))
KERNEL_DEPS := $(KERNEL_OBJECTS:.o=.d)

LOADER_SOURCES := $(wildcard boot/efi/*.c)
LOADER_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(basename $(LOADER_SOURCES)))
LOADER_DEPS := $(LOADER_OBJECTS:.o=.d)

.PHONY: all kernel loader esp disk raid-disks run test test-compat test-raid test-raid-degraded \
        check-disk size check-efi repro test-host preflight version clean distclean help

all: esp

kernel: $(KERNEL)

loader: $(LOADER)

esp: $(KERNEL) $(LOADER)
	@mkdir -p $(ESP)/EFI/BOOT $(ESP)/crugos
	$(Q)cp $(LOADER) $(ESP)/EFI/BOOT/$(LOADER_EFI_NAME)
	$(Q)cp $(KERNEL) $(ESP)/crugos/kernel.elf
	$(Q)printf '%s\n' "$(CRUGOS_VERSION)" > $(ESP)/crugos/version
	@echo "  ESP   $(ESP) ($(CRUGOS_VERSION))"

$(KERNEL_OBJECTS): VERSION
$(LOADER_OBJECTS): VERSION

$(KERNEL): $(KERNEL_OBJECTS) kernel/arch/$(ARCH)/linker.ld VERSION
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(KERNEL_FLAGS) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJECTS)
	@echo "  LD    $@"

$(LOADER): $(LOADER_OBJECTS) VERSION
	@mkdir -p $(dir $@)
	$(Q)$(LD) -m $(LOADER_LD_ARCH) --subsystem 10 -e efi_main \
		--image-base 0x10000000 --disable-reloc-section --no-insert-timestamp \
		-o $@ $(LOADER_OBJECTS)
	@echo "  PE    $@"

$(OBJ)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(KERNEL_FLAGS) -c $< -o $@
	@echo "  CC    $<"

$(OBJ)/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(KERNEL_FLAGS) -c $< -o $@
	@echo "  AS    $<"

$(OBJ)/drivers/%.o: drivers/%.c
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(KERNEL_FLAGS) -c $< -o $@
	@echo "  CC    $<"

$(OBJ)/boot/%.o: boot/%.c
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(LOADER_FLAGS) -c $< -o $@
	@echo "  CC    $<"

disk:
	$(Q)scripts/mkdisk.sh $(DISK_IMG) $(DISK_SIZE_MB)
	@echo "  DISK  $(DISK_IMG)"

raid-disks: scripts/mkraid.sh scripts/raid-meta.py
	$(Q)scripts/mkraid.sh $(RAID_DISK_A) $(RAID_DISK_B)
	@echo "  RAID  $(RAID_DISK_A) $(RAID_DISK_B)"

run: esp
	@test -f $(DISK_IMG) || $(MAKE) --no-print-directory disk
	@mkdir -p $(BUILD)/tmp
	$(Q)TMPDIR=$(abspath $(BUILD)/tmp) ESP_DIR=$(abspath $(ESP)) \
		SERIAL_LOG=$(abspath $(SERIAL_LOG)) DEBUG_LOG=$(abspath $(DEBUG_LOG)) \
		EXIT_CODE_FILE=$(abspath $(QEMU_EXIT)) QEMU_CPU="$(QEMU_CPU)" \
		DISK_IMG=$(abspath $(DISK_IMG)) EXTRA_DISKS="$(EXTRA_DISKS)" \
		scripts/run-qemu.sh

test:
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory run
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)"
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-compat:
	$(Q)$(MAKE) test BUILD_ROOT=build/compat REQUIRE_AVX2=0 QEMU_CPU=qemu64 EXPECT_AVX2=no

test-raid: esp raid-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID_DISK_A) EXTRA_DISKS=$(RAID_DISK_B)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" healthy
	$(Q)$(PYTHON) scripts/check-raid.py $(RAID_DISK_A) $(RAID_DISK_B)

test-raid-degraded: esp raid-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID_DISK_A)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" degraded
	$(Q)$(PYTHON) scripts/check-raid.py $(RAID_DISK_A)

check-disk:
	@test -f $(DISK_IMG) || $(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

size: $(KERNEL)
	$(Q)scripts/size-report.sh $(KERNEL)

check-efi: $(LOADER)
	$(Q)$(PYTHON) scripts/check-efi.py $(LOADER)

repro:
	$(Q)scripts/repro-check.sh

test-host:
	$(Q)$(MAKE) -C tests/host run

preflight:
	$(Q)scripts/preflight.sh

version:
	@echo "CrugOS $(CRUGOS_VERSION)"

clean:
	$(Q)rm -rf $(BUILD)
	@echo "  RM    $(BUILD)"

distclean: clean
	$(Q)$(MAKE) -C tests/host clean
	$(Q)rm -rf build
	@echo "  RM    build"

help:
	@sed -n '2,19p' Makefile | sed 's/^# \{0,1\}//'

-include $(KERNEL_DEPS) $(LOADER_DEPS)
