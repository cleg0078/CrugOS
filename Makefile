# CrugOS top-level build system.
#
#   make            show this help (same as 'make help')
#   make all        build kernel, UEFI loader and ESP tree
#   make run        boot the ESP image under QEMU + OVMF
#   make test       boot and verify the serial markers / exit code
#   make test-host  build and run host unit tests
#   make test-compat  boot a compatibility build (no AVX2 required)
#   make disk       build the GPT/ext4 test disk
#   make check-disk run e2fsck and file checks on the test disk
#   make test-journal-csum  crash-stop after a journal commit, then replay it with e2fsck
#   make test-journal-replay  reboot after a crash and replay the journal in the kernel
#   make test-raid  boot RAID1 (two members) and verify member disks
#   make test-raid10  boot RAID10 (four members) and verify member disks
#   make test-nvme   boot from an NVMe namespace and verify the disk
#   make test-ahci   boot from a SATA (AHCI) disk and verify the disk
#   make test-net    boot with a virtio-net NIC and verify DHCP/ARP/ICMP
#   make test-tcp    boot TCP client/server + HTTP over virtio-net
#   make test-net-dns  boot the DNS resolver against a host DNS server over TCP
#   make test-usb-hotplug  hotplug a USB keyboard into xHCI and type at it
#   make test-usb-mouse    hotplug a USB mouse into xHCI and move the pointer
#   make test-audio  stream PCM to a USB Audio Class device through the mixer
#   make test-gfx    move a hotplugged USB mouse and verify the framebuffer cursor
#   make crypt-disk  build the AES-256-XTS crypt test disk
#   make test-crypt  boot from an encrypted root and verify the disk
#   make test-crypt-recovery  unlock the encrypted root with the recovery keyslot
#   make test-crypt-wrongkey  reject a wrong password without mounting
#   make test-crypt-tamper  reject a tampered keyslot MAC
#   make test-crypt-ni  boot the encrypted root using AES-NI
#   make test-crypt-prompt  type the passphrase on the serial console and boot
#   make test-crypt-prompt-wrong  reject three wrong console passphrases
#   make test-crypt-prompt-timeout  fail closed when the prompt times out
#   make test-crypt-keyboard  type the passphrase on the PS/2 keyboard
#   make test-crypt-keyboard-wrong  reject three wrong keyboard passphrases
#   make crypt-run  boot the encrypted disk and type the passphrase in a QEMU window
#   make test-raid-degraded  boot RAID1 with one member missing
#   make test-raid10-degraded  boot RAID10 with one member per pair
#   make test-raid-repair  boot RAID1 with one blank member and rebuild it
#   make test-raid10-repair  boot RAID10 with two blank members and rebuild them
#   make size       enforce the kernel size budget
#   make check-efi  validate the loader as a PE32+ EFI application
#   make repro      verify the build is bit-for-bit reproducible
#   make preflight  check the local toolchain
#   make version    print the release version from VERSION
#   make options    print every build, CPU, QEMU and storage option
#
# Every knob has a default below; override it on the command line, e.g.
#   make test MEMORY_MB=1024 QEMU_CPU=qemu64
# Bare 'make' shows this help; 'make all' builds everything.

SHELL := /bin/bash
.DEFAULT_GOAL := help
.DELETE_ON_ERROR:
.SUFFIXES:

# Toolchain and host tools. Command-line overrides still win.
CC := crugc
LD := ld
HOST_CC ?= cc
PYTHON ?= python3
QEMU ?= qemu-system-x86_64

# Build selection.
ARCH ?= x86_64
MODE ?= release
BUILD_ROOT ?= build
V ?= 0

# CPU policy and expected guest result.
REQUIRE_AVX2 ?= 1
QEMU_CPU ?= qemu64,+avx,+avx2
EXPECT_AVX2 ?= yes
EXPECT_RAID ?= no
EXPECT_NVME ?= no
EXPECT_REBUILD ?= no
EXPECT_AHCI ?= no
EXPECT_CRYPT ?= no
EXPECT_AES ?= any
EXPECT_NET ?= no
EXPECT_DNS ?= no
EXPECT_USB ?= no
EXPECT_MOUSE ?= no
EXPECT_AUDIO ?= no
EXPECT_GFX ?= no

# QEMU / firmware options.
MEMORY_MB ?= 512
SMP ?= 1
TIMEOUT_SEC ?= 30
QEMU_ACCEL ?=
QEMU_DISPLAY ?= none
QEMU_EXTRA ?=
MACHINE_EXTRA ?=
SERIAL_TCP ?=
TCP_SERVE_SEC ?= 15
TCP_RUN_TIMEOUT_SEC ?= 180
DNS_NAME ?= crugos.test
DNS_ADDRESS ?= 10.0.2.99
DNS_TTL ?= 60
DNS_RUN_TIMEOUT_SEC ?= 180
USB_WAIT ?= 20
USB_KEYS ?= abc
USB_RUN_TIMEOUT_SEC ?= 240
MOUSE_MOVES ?= 2
MOUSE_WAIT ?= 20
MOUSE_RUN_TIMEOUT_SEC ?= 240
AUDIO_RUN_TIMEOUT_SEC ?= 240
AUDIO_QEMU_EXTRA ?= -device qemu-xhci,id=xhci -device usb-audio,audiodev=audio0 -audiodev none,id=audio0
GFX_WAIT ?= 40
GFX_MOVES ?= 3
GFX_RUN_TIMEOUT_SEC ?= 240
GFX_QEMU_EXTRA ?= -device qemu-xhci,id=xhci
NET_MODE ?=
NET_HOSTFWD ?=
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

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
CRASH_IMG ?= $(BUILD)/crash.img
CRASH_REPLAY_IMG ?= $(BUILD)/crash-replay.img
RAID_DISK_A ?= $(BUILD)/raid-a.img
RAID_DISK_B ?= $(BUILD)/raid-b.img
RAID10_DISK_A ?= $(BUILD)/raid10-a.img
RAID10_DISK_B ?= $(BUILD)/raid10-b.img
RAID10_DISK_C ?= $(BUILD)/raid10-c.img
RAID10_DISK_D ?= $(BUILD)/raid10-d.img
NVME_IMG ?= $(BUILD)/nvme.img
AHCI_IMG ?= $(BUILD)/ahci.img
CRYPT_IMG ?= $(BUILD)/crypt.img
CRYPT_TAMPER_IMG ?= $(BUILD)/crypt-tamper.img
CRYPT_PASS ?= crugos-test
CRYPT_RECOVERY_PASS ?= crugos-recovery
CRYPT_PROMPT_WRONG_PASS ?= wrong-password
CRYPT_M ?= 4096
CRYPT_PROMPT_TIMEOUT_SEC ?= 2
CRYPT_RUN_DISPLAY ?= gtk
CRYPT_RUN_TIMEOUT_SEC ?= 600
CMDLINE_EXTRA ?=
MCRYPT := $(BUILD)/mkcrypt
NVME_SERIAL ?= CRUGNVME0
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
                  $(wildcard kernel/fs/*.c) $(wildcard kernel/input/*.c) \
                  $(wildcard kernel/net/*.c) $(wildcard kernel/audio/*.c) \
                  $(wildcard kernel/gfx/*.c) \
                  $(wildcard drivers/*/*.c) \
                  $(wildcard kernel/crypto/*.c) $(wildcard kernel/crypto/*.S)
KERNEL_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(basename $(KERNEL_SOURCES)))
KERNEL_DEPS := $(KERNEL_OBJECTS:.o=.d)

LOADER_SOURCES := $(wildcard boot/efi/*.c)
LOADER_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(basename $(LOADER_SOURCES)))
LOADER_DEPS := $(LOADER_OBJECTS:.o=.d)

HOST_CRYPT_SOURCES := tools/mkcrypt.c kernel/block/crypt_meta.c \
                      $(wildcard kernel/crypto/*.c) $(wildcard kernel/crypto/*.S)

.PHONY: all kernel loader esp disk raid-disks raid10-disks nvme-disk ahci-disk \
        mkcrypt crypt-disk raid-repair-disks raid10-repair-disks run test test-compat \
        test-raid test-raid-degraded test-raid10 test-raid10-degraded test-nvme test-ahci \
        test-raid-repair test-raid10-repair test-journal-csum test-journal-replay \
        test-crypt test-crypt-recovery test-crypt-wrongkey test-crypt-tamper test-crypt-ni \
        test-crypt-prompt test-crypt-prompt-wrong test-crypt-prompt-timeout \
        test-crypt-keyboard test-crypt-keyboard-wrong crypt-run test-net test-tcp \
        test-net-dns test-usb-hotplug test-usb-mouse test-audio test-gfx \
        crash-disk \
        check-disk size check-efi repro test-host preflight version options clean distclean help

all: esp

kernel: $(KERNEL)

loader: $(LOADER)

esp: $(KERNEL) $(LOADER)
	@mkdir -p $(ESP)/EFI/BOOT $(ESP)/crugos
	$(Q)cp $(LOADER) $(ESP)/EFI/BOOT/$(LOADER_EFI_NAME)
	$(Q)cp $(KERNEL) $(ESP)/crugos/kernel.elf
	$(Q)printf '%s\n' "$(CRUGOS_VERSION)" > $(ESP)/crugos/version
	$(Q)cmdline=""; \
	if [ -n "$(CRYPT_PASS)" ]; then cmdline="crypt.pass=$(CRYPT_PASS)"; fi; \
	if [ -n "$(CMDLINE_EXTRA)" ]; then cmdline="$$cmdline $(CMDLINE_EXTRA)"; fi; \
	if [ -n "$$cmdline" ]; then printf '%s\n' "$$cmdline" > $(ESP)/crugos/cmdline.txt; \
	else rm -f $(ESP)/crugos/cmdline.txt; fi
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

$(MCRYPT): $(HOST_CRYPT_SOURCES) include/crug/crypt.h
	@mkdir -p $(dir $@)
	$(Q)$(HOST_CC) -Iinclude -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
		-fno-builtin -o $@ $(HOST_CRYPT_SOURCES)
	@echo "  HOSTCC $@"

mkcrypt: $(MCRYPT)

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

crash-disk: scripts/mkdisk.sh scripts/journal-csum.py
	$(Q)rm -f $(CRASH_IMG)
	$(Q)scripts/mkdisk.sh $(CRASH_IMG) $(DISK_SIZE_MB) crash
	@echo "  DISK  $(CRASH_IMG) (crash mode)"

raid-disks: scripts/mkraid.sh scripts/raid-meta.py
	$(Q)scripts/mkraid.sh $(RAID_DISK_A) $(RAID_DISK_B)
	@echo "  RAID  $(RAID_DISK_A) $(RAID_DISK_B)"

raid10-disks: scripts/mkraid10.sh scripts/raid-meta.py
	$(Q)scripts/mkraid10.sh $(RAID10_DISK_A) $(RAID10_DISK_B) $(RAID10_DISK_C) $(RAID10_DISK_D)
	@echo "  RAID10 $(RAID10_DISK_A) $(RAID10_DISK_B) $(RAID10_DISK_C) $(RAID10_DISK_D)"

nvme-disk: scripts/mkdisk.sh
	$(Q)rm -f $(NVME_IMG)
	$(Q)scripts/mkdisk.sh $(NVME_IMG) $(DISK_SIZE_MB)
	@echo "  NVME  $(NVME_IMG)"

ahci-disk: scripts/mkdisk.sh
	$(Q)rm -f $(AHCI_IMG)
	$(Q)scripts/mkdisk.sh $(AHCI_IMG) $(DISK_SIZE_MB)
	@echo "  AHCI  $(AHCI_IMG)"

crypt-disk: $(MCRYPT) scripts/mkdisk.sh include/crug/crypt.h
	$(Q)rm -f $(CRYPT_IMG)
	$(Q)scripts/mkdisk.sh $(CRYPT_IMG) $(DISK_SIZE_MB) crypt $(abspath $(MCRYPT)) \
		"$(CRYPT_PASS)" "$(CRYPT_RECOVERY_PASS)" $(CRYPT_M)
	@echo "  DISK  $(CRYPT_IMG) (crypt mode)"

raid-repair-disks: raid-disks scripts/raid-blank.sh
	$(Q)scripts/raid-blank.sh $(RAID_DISK_B)
	@echo "  BLANK $(RAID_DISK_B) (cold spare)"

raid10-repair-disks: raid10-disks scripts/raid-blank.sh
	$(Q)scripts/raid-blank.sh $(RAID10_DISK_B)
	$(Q)scripts/raid-blank.sh $(RAID10_DISK_D)
	@echo "  BLANK $(RAID10_DISK_B) $(RAID10_DISK_D) (cold spares)"

run: esp
	@if [ -n "$(DISK_IMG)" ] && [ ! -f "$(DISK_IMG)" ]; then \
		$(MAKE) --no-print-directory disk; \
	fi
	@mkdir -p $(BUILD)/tmp
	$(Q)TMPDIR=$(abspath $(BUILD)/tmp) ESP_DIR=$(abspath $(ESP)) \
		SERIAL_LOG=$(abspath $(SERIAL_LOG)) DEBUG_LOG=$(abspath $(DEBUG_LOG)) \
		EXIT_CODE_FILE=$(abspath $(QEMU_EXIT)) \
		QEMU="$(QEMU)" OVMF_CODE="$(OVMF_CODE)" OVMF_VARS="$(OVMF_VARS)" \
		MEMORY_MB="$(MEMORY_MB)" SMP="$(SMP)" TIMEOUT_SEC="$(TIMEOUT_SEC)" \
		QEMU_CPU="$(QEMU_CPU)" QEMU_ACCEL="$(QEMU_ACCEL)" \
		QEMU_DISPLAY="$(QEMU_DISPLAY)" QEMU_EXTRA="$(QEMU_EXTRA)" \
		SERIAL_TCP="$(SERIAL_TCP)" NET_MODE="$(NET_MODE)" \
		MACHINE_EXTRA="$(MACHINE_EXTRA)" \
		NET_HOSTFWD="$(NET_HOSTFWD)" \
		DISK_IMG=$(if $(DISK_IMG),$(abspath $(DISK_IMG)),) \
		NVME_DISK=$(if $(NVME_DISK),$(abspath $(NVME_DISK)),) \
		NVME_SERIAL="$(NVME_SERIAL)" EXTRA_DISKS="$(EXTRA_DISKS)" \
		AHCI_DISK=$(if $(AHCI_DISK),$(abspath $(AHCI_DISK)),) \
		scripts/run-qemu.sh

# Interactive keyboard test: build the encrypted test disk and boot it with
# no crypt.pass= token on the ESP command line, so the kernel must ask the
# console. Keeps a QEMU window open for real typing and a long timeout.
crypt-run: crypt-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRYPT_IMG) CRYPT_PASS= \
		QEMU_DISPLAY=$(CRYPT_RUN_DISPLAY) TIMEOUT_SEC=$(CRYPT_RUN_TIMEOUT_SEC)
	@echo "  NOTE  type the passphrase in the QEMU window (Ctrl+Alt+G releases input)"
	@echo "  NOTE  serial log $(SERIAL_LOG), exit status $(QEMU_EXIT)"

test-net: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory run NET_MODE=user
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no "$(EXPECT_NVME)" no no no any yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# TCP gate: the guest is the HTTP client and server at once.  The driver plays
# the remote HTTP host over slirp and fetches the guest listener through
# NET_HOSTFWD, so both directions are exercised end to end.
test-tcp: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-net-tcp.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --serve-seconds $(TCP_SERVE_SEC) \
		--run-timeout $(TCP_RUN_TIMEOUT_SEC)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# DNS gate: the driver is a hermetic DNS server on the slirp host loopback and
# the guest resolves DNS_NAME over TCP (slirp does not route UDP to the host).
test-net-dns: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-net-dns.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --name $(DNS_NAME) \
		--address $(DNS_ADDRESS) --ttl $(DNS_TTL) \
		--run-timeout $(DNS_RUN_TIMEOUT_SEC)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# USB gate: the guest boots with an xHCI controller and no keyboard at all, so
# the driver has to hotplug a usb-kbd over QMP and type at it.  usb.wait= and
# usb.keys= make the guest wait for the reports itself; i8042=off keeps the PS/2
# controller out of the way so the keys can only arrive over USB.
test-usb-hotplug: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-usb-hotplug.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --keys $(USB_KEYS) \
		--wait-seconds $(USB_WAIT) --run-timeout $(USB_RUN_TIMEOUT_SEC)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Mouse gate: the guest boots with an xHCI controller and no pointer at all, so
# the driver has to hotplug a usb-mouse over QMP and move it with relative
# events.  usb.mouse= and usb.moves= make the guest wait for the reports, and
# i8042=off keeps the PS/2 mouse out of the way.
test-usb-mouse: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-usb-mouse.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --moves $(MOUSE_MOVES) \
		--mouse-wait $(MOUSE_WAIT) --run-timeout $(MOUSE_RUN_TIMEOUT_SEC)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Audio gate: an xHCI controller with a USB Audio Class device plugged straight
# into it at machine start (audio is not hotpluggable in the driver).  The
# boot self-test streams half a second of a 440 Hz tone through the mixer and
# counts the isochronous completions the controller reports.
test-audio: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(DISK_IMG) CRYPT_PASS= \
		NET_MODE=none SERIAL_TCP= TIMEOUT_SEC=$(AUDIO_RUN_TIMEOUT_SEC) \
		QEMU_EXTRA="$(AUDIO_QEMU_EXTRA)" MACHINE_EXTRA="usb=off,i8042=off"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Graphics gate: the guest boots with an xHCI controller and no pointer, the
# driver hotplugs a usb-mouse over QMP, then moves it with relative events.
# Each move is verified twice: the guest's own cursor marker and the amber
# sprite centroid in a QEMU screendump of the GOP framebuffer.
test-gfx: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-gfx-cursor.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --moves $(GFX_MOVES) \
		--gfx-wait $(GFX_WAIT) --run-timeout $(GFX_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(GFX_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no no no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test:
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory run
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" "$(EXPECT_RAID)" "$(EXPECT_NVME)" "$(EXPECT_REBUILD)" "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-compat:
	$(Q)$(MAKE) test BUILD_ROOT=build/compat REQUIRE_AVX2=0 QEMU_CPU=qemu64 EXPECT_AVX2=no

test-raid: esp raid-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID_DISK_A) EXTRA_DISKS=$(RAID_DISK_B)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" healthy "$(EXPECT_NVME)" "$(EXPECT_REBUILD)" "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-raid.py $(RAID_DISK_A) $(RAID_DISK_B)

test-raid-degraded: esp raid-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID_DISK_A)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" degraded "$(EXPECT_NVME)" "$(EXPECT_REBUILD)" "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-raid.py $(RAID_DISK_A)

test-raid10: esp raid10-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID10_DISK_A) \
		EXTRA_DISKS="$(RAID10_DISK_B) $(RAID10_DISK_C) $(RAID10_DISK_D)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" healthy "$(EXPECT_NVME)" "$(EXPECT_REBUILD)" "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-raid10.py $(RAID10_DISK_A) $(RAID10_DISK_B) $(RAID10_DISK_C) $(RAID10_DISK_D)

test-raid10-degraded: esp raid10-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID10_DISK_A) \
		EXTRA_DISKS="$(RAID10_DISK_C)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" degraded "$(EXPECT_NVME)" "$(EXPECT_REBUILD)" "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-raid10.py $(RAID10_DISK_A) $(RAID10_DISK_C)

test-raid-repair: esp raid-repair-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID_DISK_A) EXTRA_DISKS=$(RAID_DISK_B)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" healthy no 1 "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-raid.py $(RAID_DISK_A) $(RAID_DISK_B)

test-raid10-repair: esp raid10-repair-disks
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(RAID10_DISK_A) \
		EXTRA_DISKS="$(RAID10_DISK_B) $(RAID10_DISK_C) $(RAID10_DISK_D)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" healthy no 2 "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-raid10.py $(RAID10_DISK_A) $(RAID10_DISK_B) $(RAID10_DISK_C) $(RAID10_DISK_D)

test-nvme: esp nvme-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG= NVME_DISK=$(NVME_IMG)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no yes "$(EXPECT_REBUILD)" "$(EXPECT_AHCI)"
	$(Q)$(PYTHON) scripts/check-disk.py $(NVME_IMG)

test-ahci: esp ahci-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG= AHCI_DISK=$(AHCI_IMG)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(AHCI_IMG)

test-crypt: esp crypt-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRYPT_IMG) CRYPT_PASS="$(CRYPT_PASS)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no yes "$(EXPECT_AES)"
	$(Q)$(PYTHON) scripts/check-disk.py $(CRYPT_IMG) \
		--crypt-tool $(abspath $(MCRYPT)) --crypt-password "$(CRYPT_PASS)"

test-crypt-recovery: esp crypt-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRYPT_IMG) \
		CRYPT_PASS="$(CRYPT_RECOVERY_PASS)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no yes "$(EXPECT_AES)"
	$(Q)$(PYTHON) scripts/check-disk.py $(CRYPT_IMG) \
		--crypt-tool $(abspath $(MCRYPT)) --crypt-password "$(CRYPT_RECOVERY_PASS)"

test-crypt-wrongkey: esp crypt-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRYPT_IMG) \
		CRYPT_PASS="wrong-password"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no wrong any

test-crypt-tamper: esp crypt-disk scripts/tamper.py
	$(Q)cp $(CRYPT_IMG) $(CRYPT_TAMPER_IMG)
	# Keyslot 0 MAC: partition at LBA 2048, keyslots at 0x1000, mac at +0x68.
	$(Q)$(PYTHON) scripts/tamper.py $(CRYPT_TAMPER_IMG) 1052776
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRYPT_TAMPER_IMG) \
		CRYPT_PASS="$(CRYPT_PASS)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no wrong any

test-crypt-ni:
	$(Q)$(MAKE) test-crypt QEMU_CPU=qemu64,+aes,+avx,+avx2 EXPECT_AES=aes-ni

test-crypt-prompt: esp crypt-disk
	$(Q)$(PYTHON) scripts/run-crypt-prompt.py correct \
		--password "$(CRYPT_PASS)" --disk $(CRYPT_IMG) \
		--serial-log $(SERIAL_LOG) --exit-code $(QEMU_EXIT)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no yes "$(EXPECT_AES)"
	$(Q)$(PYTHON) scripts/check-disk.py $(CRYPT_IMG) \
		--crypt-tool $(abspath $(MCRYPT)) --crypt-password "$(CRYPT_PASS)"

test-crypt-prompt-wrong: esp crypt-disk
	$(Q)$(PYTHON) scripts/run-crypt-prompt.py wrong \
		--wrong-password "$(CRYPT_PROMPT_WRONG_PASS)" --disk $(CRYPT_IMG) \
		--serial-log $(SERIAL_LOG) --exit-code $(QEMU_EXIT)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no wrong any

test-crypt-prompt-timeout: esp crypt-disk
	$(Q)$(PYTHON) scripts/run-crypt-prompt.py timeout --disk $(CRYPT_IMG) \
		--serial-log $(SERIAL_LOG) --exit-code $(QEMU_EXIT) \
		--cmdline-extra "crypt.prompt_timeout=$(CRYPT_PROMPT_TIMEOUT_SEC)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no locked any

test-crypt-keyboard: esp crypt-disk scripts/run-crypt-keyboard.py
	$(Q)$(PYTHON) scripts/run-crypt-keyboard.py correct \
		--password "$(CRYPT_PASS)" --disk $(CRYPT_IMG) \
		--serial-log $(SERIAL_LOG) --exit-code $(QEMU_EXIT)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no yes "$(EXPECT_AES)"
	$(Q)$(PYTHON) scripts/check-disk.py $(CRYPT_IMG) \
		--crypt-tool $(abspath $(MCRYPT)) --crypt-password "$(CRYPT_PASS)"

test-crypt-keyboard-wrong: esp crypt-disk scripts/run-crypt-keyboard.py
	$(Q)$(PYTHON) scripts/run-crypt-keyboard.py wrong \
		--wrong-password "$(CRYPT_PROMPT_WRONG_PASS)" --disk $(CRYPT_IMG) \
		--serial-log $(SERIAL_LOG) --exit-code $(QEMU_EXIT)
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		"$(EXPECT_AVX2)" no no no no wrong any

test-journal-csum: esp crash-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRASH_IMG)
	$(Q)$(PYTHON) scripts/check-journal.py $(SERIAL_LOG) $(QEMU_EXIT) $(CRASH_IMG)

test-journal-replay: esp crash-disk
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRASH_IMG)
	$(Q)cp $(CRASH_IMG) $(CRASH_REPLAY_IMG)
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(CRASH_REPLAY_IMG)
	$(Q)$(PYTHON) scripts/check-journal-replay.py $(SERIAL_LOG) $(QEMU_EXIT) $(CRASH_REPLAY_IMG)

check-disk:
	@if [ -n "$(DISK_IMG)" ] && [ ! -f "$(DISK_IMG)" ]; then \
		$(MAKE) --no-print-directory disk; \
	fi
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
	$(Q)CC="$(CC)" LD="$(LD)" PYTHON="$(PYTHON)" QEMU="$(QEMU)" \
		OVMF_CODE="$(OVMF_CODE)" OVMF_VARS="$(OVMF_VARS)" \
		scripts/preflight.sh

version:
	@echo "CrugOS $(CRUGOS_VERSION)"

options:
	@echo "CrugOS $(CRUGOS_VERSION) options"
	@printf '  %-16s %s\n' ARCH "$(ARCH)"
	@printf '  %-16s %s\n' MODE "$(MODE)"
	@printf '  %-16s %s\n' BUILD_ROOT "$(BUILD_ROOT)"
	@printf '  %-16s %s\n' V "$(V) (1 = verbose commands)"
	@printf '  %-16s %s\n' CC "$(CC)"
	@printf '  %-16s %s\n' LD "$(LD)"
	@printf '  %-16s %s\n' PYTHON "$(PYTHON)"
	@printf '  %-16s %s\n' QEMU "$(QEMU)"
	@printf '  %-16s %s\n' REQUIRE_AVX2 "$(REQUIRE_AVX2)"
	@printf '  %-16s %s\n' QEMU_CPU "$(QEMU_CPU)"
	@printf '  %-16s %s\n' EXPECT_AVX2 "$(EXPECT_AVX2)"
	@printf '  %-16s %s\n' EXPECT_RAID "$(EXPECT_RAID)"
	@printf '  %-16s %s\n' EXPECT_NVME "$(EXPECT_NVME)"
	@printf '  %-16s %s\n' EXPECT_REBUILD "$(EXPECT_REBUILD)"
	@printf '  %-16s %s\n' EXPECT_AHCI "$(EXPECT_AHCI)"
	@printf '  %-16s %s\n' EXPECT_CRYPT "$(EXPECT_CRYPT)"
	@printf '  %-16s %s\n' EXPECT_AES "$(EXPECT_AES)"
	@printf '  %-16s %s\n' EXPECT_NET "$(EXPECT_NET)"
	@printf '  %-16s %s\n' EXPECT_DNS "$(EXPECT_DNS)"
	@printf '  %-16s %s\n' EXPECT_USB "$(EXPECT_USB)"
	@printf '  %-16s %s\n' EXPECT_MOUSE "$(EXPECT_MOUSE)"
	@printf '  %-16s %s\n' EXPECT_AUDIO "$(EXPECT_AUDIO)"
	@printf '  %-16s %s\n' EXPECT_GFX "$(EXPECT_GFX)"
	@printf '  %-16s %s\n' NET_MODE "$(NET_MODE)"
	@printf '  %-16s %s\n' NET_HOSTFWD "$(NET_HOSTFWD)"
	@printf '  %-16s %s\n' TCP_SERVE_SEC "$(TCP_SERVE_SEC)"
	@printf '  %-16s %s\n' TCP_RUN_TIMEOUT_SEC "$(TCP_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' DNS_NAME "$(DNS_NAME)"
	@printf '  %-16s %s\n' DNS_ADDRESS "$(DNS_ADDRESS)"
	@printf '  %-16s %s\n' DNS_TTL "$(DNS_TTL)"
	@printf '  %-16s %s\n' DNS_RUN_TIMEOUT_SEC "$(DNS_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' USB_WAIT "$(USB_WAIT) (seconds)"
	@printf '  %-16s %s\n' USB_KEYS "$(USB_KEYS)"
	@printf '  %-16s %s\n' USB_RUN_TIMEOUT_SEC "$(USB_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' MOUSE_MOVES "$(MOUSE_MOVES)"
	@printf '  %-16s %s\n' MOUSE_WAIT "$(MOUSE_WAIT) (seconds)"
	@printf '  %-16s %s\n' MOUSE_RUN_TIMEOUT_SEC "$(MOUSE_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' AUDIO_RUN_TIMEOUT_SEC "$(AUDIO_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' AUDIO_QEMU_EXTRA "$(AUDIO_QEMU_EXTRA)"
	@printf '  %-16s %s\n' GFX_WAIT "$(GFX_WAIT) (seconds)"
	@printf '  %-16s %s\n' GFX_MOVES "$(GFX_MOVES)"
	@printf '  %-16s %s\n' GFX_RUN_TIMEOUT_SEC "$(GFX_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' GFX_QEMU_EXTRA "$(GFX_QEMU_EXTRA)"
	@printf '  %-16s %s\n' MEMORY_MB "$(MEMORY_MB)"
	@printf '  %-16s %s\n' SMP "$(SMP)"
	@printf '  %-16s %s\n' TIMEOUT_SEC "$(TIMEOUT_SEC)"
	@printf '  %-16s %s\n' QEMU_ACCEL "$(QEMU_ACCEL)"
	@printf '  %-16s %s\n' QEMU_DISPLAY "$(QEMU_DISPLAY)"
	@printf '  %-16s %s\n' QEMU_EXTRA "$(QEMU_EXTRA)"
	@printf '  %-16s %s\n' MACHINE_EXTRA "$(MACHINE_EXTRA)"
	@printf '  %-16s %s\n' OVMF_CODE "$(OVMF_CODE)"
	@printf '  %-16s %s\n' OVMF_VARS "$(OVMF_VARS)"
	@printf '  %-16s %s\n' DISK_IMG "$(DISK_IMG)"
	@printf '  %-16s %s\n' DISK_SIZE_MB "$(DISK_SIZE_MB)"
	@printf '  %-16s %s\n' CRASH_IMG "$(CRASH_IMG)"
	@printf '  %-16s %s\n' CRASH_REPLAY_IMG "$(CRASH_REPLAY_IMG)"
	@printf '  %-16s %s\n' RAID_DISK_A "$(RAID_DISK_A)"
	@printf '  %-16s %s\n' RAID_DISK_B "$(RAID_DISK_B)"
	@printf '  %-16s %s\n' RAID10_DISK_A "$(RAID10_DISK_A)"
	@printf '  %-16s %s\n' RAID10_DISK_B "$(RAID10_DISK_B)"
	@printf '  %-16s %s\n' RAID10_DISK_C "$(RAID10_DISK_C)"
	@printf '  %-16s %s\n' RAID10_DISK_D "$(RAID10_DISK_D)"
	@printf '  %-16s %s\n' NVME_IMG "$(NVME_IMG)"
	@printf '  %-16s %s\n' NVME_SERIAL "$(NVME_SERIAL)"
	@printf '  %-16s %s\n' AHCI_IMG "$(AHCI_IMG)"
	@printf '  %-16s %s\n' CRYPT_IMG "$(CRYPT_IMG)"
	@printf '  %-16s %s\n' CRYPT_TAMPER_IMG "$(CRYPT_TAMPER_IMG)"
	@printf '  %-16s %s\n' CRYPT_PASS "$(CRYPT_PASS)"
	@printf '  %-16s %s\n' CRYPT_RECOVERY_PASS "$(CRYPT_RECOVERY_PASS)"
	@printf '  %-16s %s\n' CRYPT_M "$(CRYPT_M)"
	@printf '  %-16s %s\n' MCRYPT "$(MCRYPT)"
	@printf '  %-16s %s\n' CRYPT_PROMPT_TIMEOUT_SEC "$(CRYPT_PROMPT_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' CRYPT_PROMPT_WRONG_PASS "$(CRYPT_PROMPT_WRONG_PASS)"
	@printf '  %-16s %s\n' CRYPT_RUN_DISPLAY "$(CRYPT_RUN_DISPLAY)"
	@printf '  %-16s %s\n' CRYPT_RUN_TIMEOUT_SEC "$(CRYPT_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' HOST_CC "$(HOST_CC)"
	@printf '  %-16s %s\n' CMDLINE_EXTRA "$(CMDLINE_EXTRA)"
	@printf '  %-16s %s\n' SERIAL_TCP "$(SERIAL_TCP)"
	@printf '  %-16s %s\n' NET_MODE "$(NET_MODE) (user = slirp NIC)"
	@printf '  %-16s %s\n' EXTRA_DISKS "$(EXTRA_DISKS)"
	@printf '  %-16s %s\n' BUILD "$(BUILD)"
	@printf '  %-16s %s\n' KERNEL "$(KERNEL)"
	@printf '  %-16s %s\n' LOADER "$(LOADER)"
	@printf '  %-16s %s\n' ESP "$(ESP)"
	@printf '  %-16s %s\n' SERIAL_LOG "$(SERIAL_LOG)"
	@printf '  %-16s %s\n' DEBUG_LOG "$(DEBUG_LOG)"
	@printf '  %-16s %s\n' QEMU_EXIT "$(QEMU_EXIT)"

clean:
	$(Q)rm -rf $(BUILD)
	@echo "  RM    $(BUILD)"

distclean: clean
	$(Q)$(MAKE) -C tests/host clean
	$(Q)rm -rf build
	@echo "  RM    build"

help:
	@sed -n '2,/^$$/p' Makefile | sed 's/^# \{0,1\}//'

-include $(KERNEL_DEPS) $(LOADER_DEPS)
