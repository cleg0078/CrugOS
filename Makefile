# CrugOS top-level build system.
#
#   make            show this help (same as 'make help')
#   make all        build kernel, UEFI loader and ESP tree
#   make run        boot the ESP image under QEMU + OVMF
#   make demo       boot the Workbench desktop demo on the virtio-gpu scanout
#   make iso        build a bootable hybrid UEFI ISO image
#   make check-iso  validate the ISO image without booting it
#   make test-iso   boot the ISO from a USB stick and from a CD-ROM
#   make usb        write the ISO to a USB block device (USB_DEV=/dev/sdX)
#   make test       boot and verify the serial markers / exit code
#   make test-host  build and run host unit tests
#   make test-compat  boot a compatibility build (no AVX2 required)
#   make disk       build the GPT/ext4 test disk
#   make check-disk run e2fsck and file checks on the test disk
#   make test-journal-csum  crash-stop after a journal commit, then replay it with e2fsck
#   make test-journal-replay  reboot after a crash and replay the journal in the kernel
#   make test-hardening      boot crafted images and prove the kernel rejects them
#   make test-heap           corrupt the kernel heap and prove the integrity checker catches it
#   make test-trap           inject crafted CPU faults and check the crash reports
#   make test-fuzz           mutate a real disk image and fuzz the mount path
#   make test-user           run a ring-3 program and prove the user/kernel walls
#   make test-user2          run two ring-3 programs at once and prove their spaces are isolated
#   make test-user3          run a ring-3 shell that reads a script and writes a file
#   make test-user4          spawn and exec ring-3 programs and wait for their exit codes
#   make test-user5          fork a ring-3 program and prove the child gets a private copy
#   make test-user6          share frames on fork and prove copy-on-write keeps them private
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
#   make test-workbench  drag a window on the software-rendered desktop
#   make test-workbench-menu  click menus and gadgets on the Workbench desktop
#   make test-workbench-shell  type commands into the Workbench shell window
#   make test-workbench-keys  resize a window and drive the desktop from the keyboard
#   make test-workbench-desk  open a drawer from the desktop volume icon and send it back
#   make test-workbench-icons  drag a desktop icon, raise a window and drive the icons by keyboard
#   make test-workbench-drawer  scroll, refresh and resize the System: drawer
#   make test-workbench-select  select a drawer row and drag its scrollbar
#   make test-workbench-screens  switch screens and move a window between them
#   make test-workbench-cx       toggle commodity hotkeys and fire two of them
#   make test-workbench-pkg  install, remove and reinstall a package from the desktop
#   make test-app-bundles        launch a .app bundle from the desktop and from the shell
#   make test-gpu    boot on a virtio-gpu scanout and verify the GPU-backed desktop
#   make test-gpu-usb  scanout plus a hotplugged USB mouse above 4 GiB
#   make test-gpu-edid  pick the display mode from the device EDID
#   make test-gpu-cursor  move the pointer on the virtio-gpu cursor plane
#   make test-gpu-cursor-shape  prove the cursor plane really carries pixels over VNC
#   make test-gpu-cursor-sw  show the software sprite inside the GPU scanout (the demo pointer)
#   make test-pkg    install, update and remove a signed package
#   make test-caps   launch an installed bundle and prove its capability wall
#   make test-dns-hostile  reject forged DNS replies and deep compression chains
#   make test-trap-vector  kill only the program that issues a DPL-3 vector 4
#   make test-net-synflood  reap half-open SYNs and answer RST on overflow
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
EXPECT_WB ?= no
EXPECT_MENU ?= no
EXPECT_SHELL ?= no
EXPECT_KEYS ?= no
EXPECT_DESK ?= no
EXPECT_ICONS ?= no
EXPECT_DRAWER ?= no
EXPECT_SELECT ?= no
EXPECT_HEAP ?= no
EXPECT_USER ?= no
EXPECT_USERS ?= no
EXPECT_USHELL ?= no
EXPECT_SPAWN ?= no
EXPECT_FORK ?= no
EXPECT_FORKBIG ?= no
EXPECT_PKG ?= no
EXPECT_CAPS ?= no

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
WB_WAIT ?= 30
WB_DRAG ?= 1
WB_RUN_TIMEOUT_SEC ?= 240
WB_QEMU_EXTRA ?= -device qemu-xhci,id=xhci
WB_MENU_WAIT ?= 30
WB_MENU_ACTS ?= 4
WB_MENU_RUN_TIMEOUT_SEC ?= 240
WB_SHELL_WAIT ?= 30
WB_SHELL_CMDS ?= 4
WB_SHELL_RUN_TIMEOUT_SEC ?= 240
WB_KEYS_WAIT ?= 30
WB_KEYS_ACTS ?= 2
WB_KEYS_RUN_TIMEOUT_SEC ?= 240
WB_DESK_WAIT ?= 30
WB_DESK_ACTS ?= 3
WB_DESK_RUN_TIMEOUT_SEC ?= 240
WB_ICONS_WAIT ?= 30
WB_ICONS_ACTS ?= 5
WB_ICONS_RUN_TIMEOUT_SEC ?= 240
WB_DRAWER_WAIT ?= 30
WB_DRAWER_ACTS ?= 4
WB_DRAWER_FILL ?= 24
WB_DRAWER_RUN_TIMEOUT_SEC ?= 240
WB_SELECT_WAIT ?= 30
WB_SELECT_ACTS ?= 3
WB_SELECT_FILL ?= 24
WB_SELECT_RUN_TIMEOUT_SEC ?= 240

# Screens gate (make test-workbench-screens): wb.screens=1 adds a second screen.
WB_SCREEN_WAIT ?= 30
WB_SCREEN_ACTS ?= 4
WB_SCREEN_RUN_TIMEOUT_SEC ?= 240

# Exchange gate (make test-workbench-cx): wb.cx=1 registers the hotkey
# commodities and opens the manager window.
WB_CX_WAIT ?= 30
WB_CX_ACTS ?= 4
WB_CX_RUN_TIMEOUT_SEC ?= 240

# App-bundle gate (make test-app-bundles): wb.app=1 puts the Applications icon
# on the desktop, where a double click lists the bundles and launches one.  A
# second boot with user.test=7 drives the same registry from /bin/sh.
WB_APP_WAIT ?= 30
WB_APP_ACTS ?= 1
WB_APP_RUN_TIMEOUT_SEC ?= 240
APP_RUN_TIMEOUT_SEC ?= 300

# Packages-window gate (make test-workbench-pkg): wb.pkg=1 opens a window whose
# body installs, removes and reinstalls the signed captest bundle per click.
WB_PKG_WAIT ?= 30
WB_PKG_RUN_TIMEOUT_SEC ?= 240

# GPU gate (make test-gpu): the VM has no VGA adapter, so the virtio-gpu 2D
# resource is the only display.  A QMP screendump of that device proves the
# desktop was composited into the GPU framebuffer and flushed to the host.
GPU_WB_WAIT ?= 30
GPU_RUN_TIMEOUT_SEC ?= 300
GPU_QEMU_EXTRA ?= -vga none -device virtio-gpu-pci,id=gpu0
GPU_USB_QEMU_EXTRA ?= $(GPU_QEMU_EXTRA) -device qemu-xhci,id=xhci

# EDID gate (make test-gpu-edid): the device is pinned to a non-default
# 1152x864 timing, so the mode and the scanout can only match it when the
# driver decoded the EDID instead of its 1024x768 fallback list.
GPU_EDID_DEVICE ?= virtio-gpu-pci,id=gpu0,xres=1152,yres=864
GPU_EDID_QEMU_EXTRA ?= -vga none -device $(GPU_EDID_DEVICE)

# Cursor shape gate (make test-gpu-cursor-shape): a QMP screendump cannot see
# the hardware cursor plane, and QEMU pushes no completion on the cursor queue,
# so a VNC client asks the device for its alpha cursor (VNC encoding -314) and
# checks the pixels that come back.  Without a working UPDATE_CURSOR the host
# never defines a cursor and the client waits forever.
GPU_SHAPE_WB_WAIT ?= 30
GPU_SHAPE_BOOT_TIMEOUT_SEC ?= 180
GPU_SHAPE_TIMEOUT_SEC ?= 240
GPU_SHAPE_RUN_TIMEOUT_SEC ?= 300
GPU_SHAPE_QEMU_EXTRA ?= -vga none -device virtio-gpu-pci,id=gpu0

# Package gates (make test-pkg / make test-caps): pkg.test=1 installs the
# signed captest bundle, updates it, refuses a downgrade and an untrusted
# key, then removes it; pkg.test=2 launches the installed bundle and checks
# the capability wall around it.  Both packages are built from a fixed seed
# and timestamp so two images are byte-identical.
PKG_SEED ?= 0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0
PKG_BAD_SEED ?= 1122334455667788990011223344556677889900112233445566778899001122
PKG_RUN_TIMEOUT_SEC ?= 240
CAPS_RUN_TIMEOUT_SEC ?= 240

# Hardening gate (make test-hardening): how long each crafted image may boot.
HARDEN_TIMEOUT_SEC ?= 120

# Mount-path fuzzing gate (make test-fuzz): the harness is a host binary, so the
# case count and the per-case timeout are the only knobs it needs.
FUZZ_CASES ?= 20000
FUZZ_CRYPT_CASES ?= 2000
FUZZ_SEED ?= 1337
FUZZ_TIMEOUT_SEC ?= 10

# Heap gate (make test-heap): random allocation churn plus the corruption probes.
HEAP_STRESS_OPS ?= 4000
HEAP_RUN_TIMEOUT_SEC ?= 240

# Trap gate (make test-trap): how long one crafted fault may boot.
TRAP_TIMEOUT_SEC ?= 60

# Security gates (phase S1): each one boots the unmodified kernel with a
# selftest token, so the only knob is how long that boot may take.
S1_RUN_TIMEOUT_SEC ?= 240

# Userspace gate (make test-user): how long the ring-3 selftest may take.
USER_RUN_TIMEOUT_SEC ?= 240

# Address-space isolation gate (make test-user2): two ring-3 programs at once.
USER2_RUN_TIMEOUT_SEC ?= 240

# Ring-3 shell gate (make test-user3): file syscalls, argv and /bin/sh.
SHELL_RUN_TIMEOUT_SEC ?= 240

# Spawn gate (make test-user4): SYS_SPAWN, SYS_WAIT and SYS_EXEC at CPL3.
SPAWN_RUN_TIMEOUT_SEC ?= 240

# Fork gate (make test-user5): SYS_FORK shares the parent's frames copy-on-write.
FORK_RUN_TIMEOUT_SEC ?= 240

# Large-fork gate (make test-user6): a 300-page image forks without copying.
FORKBIG_RUN_TIMEOUT_SEC ?= 300

# Interactive demo (make demo): a real QEMU window with a USB keyboard and
# mouse, the whole Workbench feature set switched on (desktop object and its
# drawer, screen switching, Commodities Exchange, menus and gadgets, live
# Shell) and a hold so the window stays open.  wb.wait=0 keeps every boot
# gate disarmed, so the demo boots straight to the desktop; the drawer fill
# only runs the first time the drawer is opened, and 0 leaves the volume
# untouched.  The demo boots on the virtio-gpu 2D scanout: gpu.enable=1 in
# the command line and a virtio-gpu-pci device with no VGA adapter, so the
# window shows the guest's own scanout rather than firmware memory.  Set
# DEMO_GPU=0 to boot the software framebuffer instead.
DEMO_DISPLAY ?= gtk
DEMO_HOLD_SEC ?= 600
DEMO_TIMEOUT_SEC ?= 900
DEMO_FILL_FILES ?= 12
DEMO_GPU ?= 1
DEMO_GPU_SIZE ?= 1024x768
# DEMO_CURSOR picks the pointer the demo draws: sw bakes the guest sprite
# into the scanout (visible in every window manager), hw asks the device for
# its cursor plane (a VNC viewer shows it, a GTK window hides it).
DEMO_GPU_QEMU_EXTRA ?= -vga none -device virtio-gpu-pci,id=gpu0
DEMO_CURSOR ?= sw
DEMO_CMDLINE ?= gpu.enable=$(DEMO_GPU) gpu.size=$(DEMO_GPU_SIZE) gpu.cursor=$(DEMO_CURSOR) wb.enable=1 wb.wait=0 wb.desk=1 wb.dr=1 wb.dsel=1 wb.screens=1 wb.cx=1 wb.shell=1 wb.fill=$(DEMO_FILL_FILES) wb.hold=$(DEMO_HOLD_SEC)
DEMO_MACHINE_EXTRA ?= usb=off,i8042=off
DEMO_QEMU_EXTRA ?= $(if $(filter-out 0,$(DEMO_GPU)),$(DEMO_GPU_QEMU_EXTRA)) -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0

# Bootable hybrid UEFI ISO (make iso). The ESP travels as a FAT16 image behind
# an El Torito boot entry, the tree also carries the plain ESP files, and
# ISO_CMDLINE defaults to storage=none so the image boots with no block device.
ISO_IMAGE ?= $(BUILD)/crugos.iso
ISO_STAGE ?= $(BUILD)/iso
ISO_FAT_IMG ?= $(ISO_STAGE)/tree/boot/efi.img
ISO_SIZE_MB ?= 24
ISO_VOLID ?= CRUGOS
ISO_CMDLINE ?= storage=none
ISO_DATE ?= 2026010100000000
ISO_RUN_TIMEOUT_SEC ?= 240
EXPECT_ISO ?= no
# Boot media forwarded to scripts/run-qemu.sh by 'make run' when set.
ISO_IMG ?=
USB_IMG ?= $(BUILD)/usb-stick.img
USB_STICK ?=
USB_DEV ?=
USB_CONFIRM ?= no
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
MKPKG := $(BUILD)/mkpkg
PKG_DATA_DIR ?= $(BUILD)/pkg
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
                -Iinclude -Iboot/efi -MMD -MP $(VERSION_FLAGS) $(AVX2_FLAGS) \
                $(WARN_FLAGS) $(MODE_FLAGS)
KERNEL_FLAGS := $(COMMON_FLAGS) -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
                -mno-avx -mno-avx2 -mno-avx512f -fno-builtin
LOADER_FLAGS := $(COMMON_FLAGS) -fshort-wchar

KERNEL_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
                  -Wl,-T,kernel/arch/$(ARCH)/linker.ld -Wl,-Map,$(KERNEL_MAP)

KERNEL_SOURCES := $(wildcard kernel/arch/$(ARCH)/*.c) $(wildcard kernel/arch/$(ARCH)/*.S) \
                  $(wildcard kernel/core/*.c) $(wildcard kernel/lib/*.c) \
                  $(wildcard kernel/mm/*.c) $(wildcard kernel/block/*.c) \
                  $(wildcard kernel/fs/*.c) $(wildcard kernel/exec/*.c) \
                  $(wildcard kernel/input/*.c) \
                  $(wildcard kernel/net/*.c) $(wildcard kernel/audio/*.c) \
                  $(wildcard kernel/gfx/*.c) $(wildcard kernel/gpu/*.c) \
                  $(wildcard kernel/app/*.c) \
                  $(wildcard kernel/shell/*.c) \
                  $(wildcard drivers/*/*.c) \
                  $(wildcard kernel/crypto/*.c) $(wildcard kernel/crypto/*.S)
KERNEL_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(basename $(KERNEL_SOURCES)))
# The ELF64 loader in boot/efi/elf.c is shared with the UEFI loader, but the
# kernel links it with the kernel flags, so it is an explicit object.
KERNEL_OBJECTS += $(OBJ)/shared/elf.o
KERNEL_DEPS := $(KERNEL_OBJECTS:.o=.d)

USER_ELF_DIR := $(BUILD)/user
USER_OBJ := $(BUILD)/user/obj
USER_PROGRAMS := hello badptr fault iso sh spawn child fork forkbig trap4 captest
# Phase 7A: these two are installed as .app bundle payloads, not in /bin.
APP_PROGRAMS := app_hello app_orbit
USER_ELF := $(patsubst %,$(USER_ELF_DIR)/%,$(USER_PROGRAMS) $(APP_PROGRAMS))
USER_FLAGS := -std=gnu11 -fcrug-kernel -ffreestanding -fno-pic -fno-pie \
              -mcmodel=large -fno-stack-protector -fno-builtin -fno-ident \
              -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -Iinclude -Iuser -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
              -mno-80387 -mno-avx -mno-avx2 -mno-avx512f \
              -Wall -Wextra -Wno-unused-parameter $(MODE_FLAGS)
USER_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
                -Wl,-T,user/user.ld
# The isolation probe needs a third segment on a fixed page, so it links with
# its own script.
ISO_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
               -Wl,-T,user/iso.ld
# The shell has read-only text and read-write data segments, so it needs a
# script that keeps them apart for the W^X mapping in elf_exec.c.
SH_LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
              -Wl,-T,user/sh.ld

LOADER_SOURCES := $(wildcard boot/efi/*.c)
LOADER_OBJECTS := $(patsubst %,$(OBJ)/%.o,$(basename $(LOADER_SOURCES)))
LOADER_DEPS := $(LOADER_OBJECTS:.o=.d)

HOST_CRYPT_SOURCES := tools/mkcrypt.c kernel/block/crypt_meta.c \
                      $(wildcard kernel/crypto/*.c) $(wildcard kernel/crypto/*.S) \
                      kernel/lib/string.c

# Phase 7B host signer: the package format needs the kernel's SHA-512 and
# Ed25519 routines, so the tool compiles those sources for the host.
HOST_MKPKG_SOURCES := tools/mkpkg.c kernel/crypto/ed25519.c \
                      kernel/crypto/sha512.c kernel/lib/string.c

.PHONY: all kernel loader user esp disk raid-disks raid10-disks nvme-disk ahci-disk \
        mkcrypt crypt-disk raid-repair-disks raid10-repair-disks run demo test test-compat \
        test-raid test-raid-degraded test-raid10 test-raid10-degraded test-nvme test-ahci \
        test-raid-repair test-raid10-repair test-journal-csum test-journal-replay \
        test-hardening test-heap test-trap test-fuzz test-user test-user2 test-user3 \
        test-user4 test-user5 test-user6 \
        test-crypt test-crypt-recovery test-crypt-wrongkey test-crypt-tamper test-crypt-ni \
        test-crypt-prompt test-crypt-prompt-wrong test-crypt-prompt-timeout \
        test-crypt-keyboard test-crypt-keyboard-wrong crypt-run test-net test-tcp \
        test-net-dns test-usb-hotplug test-usb-mouse test-audio test-gfx \
        test-workbench test-workbench-menu test-workbench-shell test-workbench-keys \
        test-workbench-desk test-workbench-icons test-workbench-drawer \
        test-workbench-select test-workbench-screens test-workbench-cx \
        test-workbench-pkg test-app-bundles test-gpu test-gpu-usb \
        test-gpu-edid test-gpu-cursor test-gpu-cursor-shape \
        test-gpu-cursor-sw test-pkg \
        test-dns-hostile test-trap-vector test-net-synflood \
        test-caps packages mkpkg \
        iso check-iso test-iso usb \
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
	$(Q)rm -f $(ESP)/crugos/cmdline.txt
	$(Q)cmdline=""; \
	if [ -n "$(CRYPT_PASS)" ]; then cmdline="crypt.pass=$(CRYPT_PASS)"; fi; \
	if [ -n "$(CMDLINE_EXTRA)" ]; then cmdline="$$cmdline $(CMDLINE_EXTRA)"; fi; \
	if [ -n "$$cmdline" ]; then printf '%s\n' "$$cmdline" > $(ESP)/crugos/cmdline.txt; fi
	@echo "  ESP   $(ESP) ($(CRUGOS_VERSION))"

$(KERNEL_OBJECTS): VERSION
$(LOADER_OBJECTS): VERSION
$(USER_ELF): VERSION

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

$(MKPKG): $(HOST_MKPKG_SOURCES) include/crug/pkg.h
	@mkdir -p $(dir $@)
	$(Q)$(HOST_CC) -Iinclude -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
		-fno-builtin -o $@ $(HOST_MKPKG_SOURCES)
	@echo "  HOSTCC $@"

mkpkg: $(MKPKG)

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

$(OBJ)/shared/elf.o: boot/efi/elf.c
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(KERNEL_FLAGS) -c $< -o $@
	@echo "  CC    $<"

$(USER_OBJ)/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_FLAGS) -MMD -MP -c $< -o $@
	@echo "  CC    $<"

$(USER_OBJ)/%.o: user/%.S
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_FLAGS) -MMD -MP -c $< -o $@
	@echo "  AS    $<"

$(USER_ELF_DIR)/%: $(USER_OBJ)/start.o $(USER_OBJ)/%.o user/user.ld
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_FLAGS) $(USER_LDFLAGS) -o $@ $(USER_OBJ)/start.o $(USER_OBJ)/$*.o
	@echo "  USRLD $@"

$(USER_ELF_DIR)/iso: $(USER_OBJ)/start.o $(USER_OBJ)/iso.o user/iso.ld
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_FLAGS) $(ISO_LDFLAGS) -o $@ $(USER_OBJ)/start.o $(USER_OBJ)/iso.o
	@echo "  USRLD $@"

$(USER_ELF_DIR)/sh: $(USER_OBJ)/start.o $(USER_OBJ)/sh.o user/sh.ld
	@mkdir -p $(dir $@)
	$(Q)$(CC) $(USER_FLAGS) $(SH_LDFLAGS) -o $@ $(USER_OBJ)/start.o $(USER_OBJ)/sh.o
	@echo "  USRLD $@"

user: $(USER_ELF)

# Phase 7B: sign the test bundles the installer gates install.  The packages are
# built from the user ELFs by the host signer, so this target needs both.
packages: $(MKPKG) $(USER_ELF)
	$(Q)PKG_SEED='$(PKG_SEED)' PKG_BAD_SEED='$(PKG_BAD_SEED)' \
	  sh scripts/mkpkgtest.sh $(abspath $(PKG_DATA_DIR)) \
	  $(abspath $(USER_ELF_DIR)/captest) $(abspath $(MKPKG))
	@echo "  PKG   $(PKG_DATA_DIR)"

disk: user packages
	$(Q)USER_DIR=$(abspath $(USER_ELF_DIR)) APP_DIR=$(abspath $(USER_ELF_DIR)) \
	  PKG_DIR=$(abspath $(PKG_DATA_DIR)) \
	  scripts/mkdisk.sh $(DISK_IMG) $(DISK_SIZE_MB)
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
	$(Q)TMPDIR=$(abspath $(BUILD)/tmp) \
		SERIAL_LOG=$(abspath $(SERIAL_LOG)) DEBUG_LOG=$(abspath $(DEBUG_LOG)) \
		EXIT_CODE_FILE=$(abspath $(QEMU_EXIT)) \
		QEMU="$(QEMU)" OVMF_CODE="$(OVMF_CODE)" OVMF_VARS="$(OVMF_VARS)" \
		MEMORY_MB="$(MEMORY_MB)" SMP="$(SMP)" TIMEOUT_SEC="$(TIMEOUT_SEC)" \
		QEMU_CPU="$(QEMU_CPU)" QEMU_ACCEL="$(QEMU_ACCEL)" \
		QEMU_DISPLAY="$(QEMU_DISPLAY)" QEMU_EXTRA="$(QEMU_EXTRA)" \
		SERIAL_TCP="$(SERIAL_TCP)" NET_MODE="$(NET_MODE)" \
		MACHINE_EXTRA="$(MACHINE_EXTRA)" \
		NET_HOSTFWD="$(NET_HOSTFWD)" \
		ESP_DIR=$(if $(or $(ISO_IMG),$(USB_STICK)),,$(abspath $(ESP))) \
		ISO_IMG=$(if $(ISO_IMG),$(abspath $(ISO_IMG)),) \
		USB_STICK=$(if $(USB_STICK),$(abspath $(USB_STICK)),) \
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

# Bootable hybrid UEFI ISO: the ESP becomes a FAT16 image behind El Torito and
# the ISO tree also ships the plain ESP files.  The image is bootable both from
# a CD-ROM and from a raw USB stick, so 'make usb' can dd it to real hardware.
iso: esp
	$(Q)mkdir -p $(ISO_STAGE)
	$(Q)ESP_DIR=$(abspath $(ESP)) ISO_IMG=$(abspath $(ISO_IMAGE)) \
		ISO_STAGE=$(abspath $(ISO_STAGE)) ISO_SIZE_MB=$(ISO_SIZE_MB) \
		ISO_CMDLINE="$(ISO_CMDLINE)" ISO_VOLID="$(ISO_VOLID)" \
		ISO_DATE=$(ISO_DATE) PYTHON="$(PYTHON)" scripts/mkiso.sh

check-iso: iso
	$(Q)$(PYTHON) scripts/check-iso.py --iso $(abspath $(ISO_IMAGE)) \
		--mkfat $(abspath scripts/mkfat.py)

# ISO gate: dd the image onto a stick and boot it twice, once through
# usb-storage on an xHCI controller and once as a plain CD-ROM.  Both boots run
# with no block device, so the guest cmdline must say storage=none.
test-iso: iso check-iso
	$(Q)rm -f $(USB_IMG)
	$(Q)dd if=$(ISO_IMAGE) of=$(USB_IMG) bs=1M status=none
	$(Q)$(MAKE) --no-print-directory run DISK_IMG= NET_MODE=none SERIAL_TCP= \
		MACHINE_EXTRA="usb=off,i8042=off" TIMEOUT_SEC=$(ISO_RUN_TIMEOUT_SEC) \
		USB_STICK=$(abspath $(USB_IMG)) \
		SERIAL_LOG=$(abspath $(BUILD)/iso-usb-serial.log) \
		QEMU_EXIT=$(abspath $(BUILD)/iso-usb-exit.txt)
	$(Q)$(PYTHON) scripts/boot-check.py $(BUILD)/iso-usb-serial.log \
		$(BUILD)/iso-usb-exit.txt VERSION "$(EXPECT_AVX2)" no no no no no any \
		no no no no no no no no --storage=none
	@echo "  OK    iso boots from a USB stick (exit 33)"
	$(Q)$(MAKE) --no-print-directory run DISK_IMG= NET_MODE=none SERIAL_TCP= \
		MACHINE_EXTRA="usb=off,i8042=off" TIMEOUT_SEC=$(ISO_RUN_TIMEOUT_SEC) \
		ISO_IMG=$(abspath $(ISO_IMAGE)) \
		SERIAL_LOG=$(abspath $(BUILD)/iso-cd-serial.log) \
		QEMU_EXIT=$(abspath $(BUILD)/iso-cd-exit.txt)
	$(Q)$(PYTHON) scripts/boot-check.py $(BUILD)/iso-cd-serial.log \
		$(BUILD)/iso-cd-exit.txt VERSION "$(EXPECT_AVX2)" no no no no no any \
		no no no no no no no no --storage=none
	@echo "  OK    iso boots from a CD-ROM (exit 33)"

# Write the ISO to a real USB stick.  This is the only target that touches a
# device outside build/, so it needs an absolute block device and an explicit
# USB_CONFIRM=yes, and it refuses a device that is currently mounted.
usb: iso
	$(Q)if [ "$(USB_CONFIRM)" != "yes" ]; then \
		echo "  usb: refusing to write $(ISO_IMAGE): set USB_CONFIRM=yes"; exit 2; \
	fi
	$(Q)if [ -z "$(USB_DEV)" ] || [ ! -b "$(USB_DEV)" ]; then \
		echo "  usb: USB_DEV=$(USB_DEV) is not a block device"; exit 2; \
	fi
	$(Q)case "$(USB_DEV)" in /*) ;; *) echo "  usb: USB_DEV must be absolute"; exit 2;; esac
	$(Q)if grep -q "^$(USB_DEV)" /proc/mounts; then \
		echo "  usb: $(USB_DEV) is mounted, unmount it first"; exit 2; \
	fi
	$(Q)dd if=$(ISO_IMAGE) of=$(USB_DEV) bs=4M conv=fsync status=progress
	@echo "  OK    wrote $(ISO_IMAGE) to $(USB_DEV)"

# Interactive demo: boot the full Workbench in a real QEMU window.  The demo
# command line switches every desktop feature on (desktop object and drawer,
# screen switching, Commodities Exchange, menus and gadgets, live Shell), and
# wb_hold() keeps the desktop on screen for DEMO_HOLD_SEC seconds while the
# USB keyboard and mouse stay live, so everything can be clicked and typed.
demo: esp
	$(Q)$(MAKE) --no-print-directory run DISK_IMG=$(DISK_IMG) CRYPT_PASS= \
		NET_MODE=none QEMU_DISPLAY=$(DEMO_DISPLAY) \
		TIMEOUT_SEC=$(DEMO_TIMEOUT_SEC) \
		MACHINE_EXTRA="$(DEMO_MACHINE_EXTRA)" \
		QEMU_EXTRA="$(DEMO_QEMU_EXTRA)" \
		CMDLINE_EXTRA="$(DEMO_CMDLINE)"
	@echo "  NOTE  the window is the virtio-gpu 2D scanout (DEMO_GPU=0 uses the software framebuffer)"
	@echo "  NOTE  the pointer is the guest's software sprite (DEMO_CURSOR=hw uses the device cursor plane)"
	@echo "  NOTE  the full Workbench is on screen; drag any title bar to move a window"
	@echo "  NOTE  double-click the 'System:' icon to open its drawer (rows select, scrollbar drags)"
	@echo "  NOTE  click the Workbench/Tools boxes at the top right to switch screens"
	@echo "  NOTE  F2/F3/F4/F6 fire the commodities: volume up/down, mute, clock"
	@echo "  NOTE  type into the Shell window and press Enter; Ctrl+Alt+G releases the pointer"
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

# Workbench gate: the guest boots with wb.enable=1, opens its windows and waits
# for a title-bar drag; the driver hotplugs a usb-mouse over QMP, presses on the
# title bar, drags it and releases. A screendump then proves the active title
# bar is drawn at the new window origin and no longer at the old one.
test-workbench: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --drag $(WB_DRAG) \
		--wb-wait $(WB_WAIT) --run-timeout $(WB_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no no no no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Menu gate: the guest boots with wb.menu=1, opens its menus and gadgets and
# waits for a number of actions; the driver hotplugs a usb-mouse over QMP, opens
# a menu, hovers an item, selects it, clicks a gadget, closes a window with its
# close gadget and selects another menu item. Screendumps prove the popup
# highlight and that the closed window area went back to the desktop.
test-workbench-menu: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-menu.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_MENU_ACTS) \
		--wb-wait $(WB_MENU_WAIT) --run-timeout $(WB_MENU_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no no no no no no yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)
# Shell gate: the guest boots with wb.shell=1, opens the shell window on the
# Workbench desktop and waits for shell commands; the driver hotplugs a usb-kbd
# over QMP and types four commands at it. Serial markers prove each line ran and
# a screendump proves the scrollback area repainted.
test-workbench-shell: EXPECT_SHELL := yes
test-workbench-shell: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-shell.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --cmds $(WB_SHELL_CMDS) \
		--wb-wait $(WB_SHELL_WAIT) --run-timeout $(WB_SHELL_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no no no no no no no "$(EXPECT_SHELL)"
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Keys gate: the guest boots with wb.keys=1 and waits for actions; the driver
# hotplugs a usb-mouse and drags the resize grip to shrink the Shell window,
# then hotplugs a usb-kbd and uses Tab, the arrow keys, Enter and the menu
# popup. A screendump proves the enlarged desktop area left by the resize.
test-workbench-keys: EXPECT_KEYS := yes
test-workbench-keys: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-keys.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_KEYS_ACTS) \
		--wb-wait $(WB_KEYS_WAIT) --run-timeout $(WB_KEYS_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION "$(EXPECT_AVX2)" no no no no no any no no no no no no no no no no "$(EXPECT_KEYS)"
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Desktop gate: the guest boots with wb.desk=1 and waits for actions; the driver
# clicks the System: volume icon twice to open the drawer, then clicks the
# drawer's depth gadget to send it to the back. Serial markers prove the icon,
# the drawer and the depth event; screendumps prove the redraw.
test-workbench-desk: EXPECT_DESK := yes
test-workbench-desk: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-desk.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_DESK_ACTS) \
		--wb-wait $(WB_DESK_WAIT) --run-timeout $(WB_DESK_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no $(EXPECT_DESK)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-workbench-icons: EXPECT_ICONS := yes
test-workbench-icons: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-icons.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_ICONS_ACTS) \
		--wb-wait $(WB_ICONS_WAIT) --run-timeout $(WB_ICONS_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no $(EXPECT_ICONS)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Drawer gate: wb.dr=1 fills the volume, opens the drawer from the volume icon,
# scrolls it with the arrow keys, refreshes it with 'r' and resizes it with the
# grip so the row count grows.  Serial markers prove each step, screendumps prove
# the scroll and the reflow.
test-workbench-drawer: EXPECT_DRAWER := yes
test-workbench-drawer: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-drawer.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_DRAWER_ACTS) \
		--fill $(WB_DRAWER_FILL) --wb-wait $(WB_DRAWER_WAIT) \
		--run-timeout $(WB_DRAWER_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no $(EXPECT_ICONS) $(EXPECT_DRAWER)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Selection gate: wb.dsel=1 makes the drawer rows selectable and turns the
# scrollbar thumb into a drag handle.  Serial markers prove the selection and the
# scrollbar drag; screendumps prove the amber band and that the view moved.
test-workbench-select: EXPECT_SELECT := yes
test-workbench-select: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-select.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_SELECT_ACTS) \
		--fill $(WB_SELECT_FILL) --wb-wait $(WB_SELECT_WAIT) \
		--run-timeout $(WB_SELECT_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no $(EXPECT_ICONS) $(EXPECT_DRAWER) $(EXPECT_SELECT)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Screens gate: wb.screens=1 adds a second screen with its own desktop and a
# window on it.  Serial markers prove the two switches and the window move,
# screendumps prove each screen paints its own background and that the moved
# window is missing from the screen it left.
test-workbench-screens: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-screens.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_SCREEN_ACTS) \
		--wb-wait $(WB_SCREEN_WAIT) \
		--run-timeout $(WB_SCREEN_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-screen=yes

# Exchange gate (phase 6J): wb.cx=1 registers four hotkey commodities and
# opens the manager window.  A row click switches a hotkey off and on again,
# a press while it is off is reported as blocked, and two fires run the mute
# and clock utilities.
test-workbench-cx: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(PYTHON) scripts/run-workbench-cx.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_CX_ACTS) \
		--wb-wait $(WB_CX_WAIT) \
		--run-timeout $(WB_CX_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-cx=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Packages gate (phase 7B): wb.pkg=1 opens a Packages window whose body installs
# the signed captest bundle with one click; the desktop reports the registry,
# the install and its own selftest before the boot finishes.
test-workbench-pkg: esp user packages
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-workbench-pkg.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--wb-wait $(WB_PKG_WAIT) \
		--run-timeout $(WB_PKG_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-wbpkg=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# GPU gate (phase 8A): the QEMU command line removes the VGA adapter, so the
# virtio-gpu 2D resource is the only display.  The driver attaches that
# resource as the linear framebuffer before the desktop exists, therefore a
# QMP screendump of the gpu0 device shows only pixels the guest transferred
# and flushed over virtio.  The driver also compares the desktop colours and
# the GPU selftest counters, and the boot still ends at isa-debug-exit 0x10.
test-gpu: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-gpu.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--wb-wait $(GPU_WB_WAIT) \
		--run-timeout $(GPU_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(GPU_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-gpu=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# GPU + USB gate (regression, phase 8A): with the virtio-gpu present OVMF
# places the xHCI controller 64-bit BAR above 4 GiB (0xc000004000).  Decoding
# only the low dword mapped the identity page at 0x4000, so the controller
# never reset and a hotplugged mouse never appeared.  This gate hotplugs one
# and requires pointer motion on the GPU scanout.
test-gpu-usb: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-gpu.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--wb-wait $(GPU_WB_WAIT) \
		--run-timeout $(GPU_RUN_TIMEOUT_SEC) \
		--usb --moves 2 \
		--qemu-extra "$(GPU_USB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-gpu=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# EDID gate (phase 8B): the device is pinned to 1152x864, which is neither
# the driver's 1024x768 fallback nor QEMU's default, so a guest that ends up
# at that mode can only have adopted the GET_EDID timing.  The verdict also
# re-checks the scanout pixels, because a wrong mode moves the desktop
# samples out of place.
test-gpu-edid: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-gpu.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--wb-wait $(GPU_WB_WAIT) \
		--run-timeout $(GPU_RUN_TIMEOUT_SEC) \
		--expect-edid \
		--qemu-extra "$(GPU_EDID_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-gpu=yes --expect-gpu-edid=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Hardware-cursor gate (phase 8B): UPDATE_CURSOR and MOVE_CURSOR travel on
# virtqueue 1 with their own 64x64 resource; a screendump of the scanout
# device never contains them, so the serial counters are the evidence that the
# plane (not the software sprite) moved.
test-gpu-cursor: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-gpu.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--wb-wait $(GPU_WB_WAIT) \
		--run-timeout $(GPU_RUN_TIMEOUT_SEC) \
		--expect-cursor \
		--qemu-extra "$(GPU_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-gpu=yes --expect-gpu-cursor=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Cursor shape gate (phase 8B): QEMU answers UPDATE_CURSOR by defining a cursor
# on the console, which only a display client can see.  The guest cannot even
# tell whether the host accepted the command, because the cursor queue is not
# answered, so the proof is the alpha-cursor rect a VNC client receives.
test-gpu-cursor-sw: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-gpu.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--wb-wait $(GPU_WB_WAIT) \
		--run-timeout $(GPU_RUN_TIMEOUT_SEC) \
		--expect-sprite \
		--qemu-extra "$(GPU_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-gpu=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-gpu-cursor-shape: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-gpu-cursor-shape.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --wb-wait $(GPU_SHAPE_WB_WAIT) \
		--boot-timeout $(GPU_SHAPE_BOOT_TIMEOUT_SEC) \
		--shape-timeout $(GPU_SHAPE_TIMEOUT_SEC) \
		--run-timeout $(GPU_SHAPE_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(GPU_SHAPE_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-gpu=yes --expect-gpu-cursor=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# App-bundle gate (phase 7A): wb.app=1 adds the Applications icon; a double
# click opens the bundle list and a double click on a row launches that
# program, which the desktop then reaps with its exit status.  A second boot
# with user.test=7 drives the same registry from /bin/sh ('app list|info|run').
test-pkg: EXPECT_PKG := yes
test-pkg: esp user packages
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(PKG_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="pkg.test=1"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL) $(EXPECT_SPAWN) $(EXPECT_FORK) $(EXPECT_FORKBIG) \
		--expect-pkg=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-caps: EXPECT_CAPS := yes
test-caps: esp user packages
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(CAPS_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="pkg.test=2"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL) $(EXPECT_SPAWN) $(EXPECT_FORK) $(EXPECT_FORKBIG) \
		--expect-caps=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-app-bundles: EXPECT_USER := yes
test-app-bundles: EXPECT_USERS := yes
test-app-bundles: EXPECT_USHELL := yes
test-app-bundles: EXPECT_SPAWN := yes
test-app-bundles: EXPECT_FORK := yes
test-app-bundles: EXPECT_FORKBIG := yes
test-app-bundles: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-workbench-app.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) --serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) --acts $(WB_APP_ACTS) \
		--wb-wait $(WB_APP_WAIT) \
		--run-timeout $(WB_APP_RUN_TIMEOUT_SEC) \
		--qemu-extra "$(WB_QEMU_EXTRA)"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-app=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(APP_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=7"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL) $(EXPECT_SPAWN) $(EXPECT_FORK) $(EXPECT_FORKBIG)
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

test-hardening: esp
	$(Q)rm -f $(DISK_IMG) $(CRYPT_IMG)
	$(Q)$(MAKE) --no-print-directory disk crypt-disk
	$(Q)$(PYTHON) scripts/run-hardening.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) \
		--crypt-disk $(abspath $(CRYPT_IMG)) \
		--crypt-pass "$(CRYPT_PASS)" \
		--serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--timeout $(HARDEN_TIMEOUT_SEC)

# Security gates (phase S1).  Every one boots the stock kernel with a selftest
# token and proves a finding from specs/fix-p2 with serial evidence only, so no
# input has to be scripted from the host.
test-dns-hostile: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(S1_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="dns.test=hostile"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-dnshostile=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-trap-vector: EXPECT_USER := yes
test-trap-vector: EXPECT_USERS := yes
test-trap-vector: EXPECT_USHELL := yes
test-trap-vector: EXPECT_SPAWN := yes
test-trap-vector: EXPECT_FORK := yes
test-trap-vector: EXPECT_FORKBIG := yes
test-trap-vector: esp user
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(S1_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=8"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL) $(EXPECT_SPAWN) \
	$(EXPECT_FORK) $(EXPECT_FORKBIG) --expect-trapvector=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-net-synflood: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(S1_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="net.synflood=1"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no --expect-synflood=yes
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)
# Heap gate: boot with heap.stress= and heap.guard=1 so the kernel churns the
# allocator and then plants three corruptions (a forged free-list link, a zeroed
# slab cookie and a zeroed large-block cookie) that heap_check() must catch.
# No input is scripted, so the gate is a plain boot plus serial markers.
test-heap: EXPECT_HEAP := yes
test-heap: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(HEAP_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="heap.stress=$(HEAP_STRESS_OPS) heap.guard=1"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION $(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no $(EXPECT_HEAP)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

test-trap: esp
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(PYTHON) scripts/run-trap.py --repo $(CURDIR) \
		--disk $(abspath $(DISK_IMG)) \
		--serial-log $(abspath $(SERIAL_LOG)) \
		--exit-code $(abspath $(QEMU_EXIT)) \
		--timeout $(TRAP_TIMEOUT_SEC)

# Fuzzing gate: rebuild a known-good ext4 disk and crypt volume, then let the
# host harness mutate them and mount every case in a forked child. A child that
# dies on a signal or hits the kernel panic hook fails the gate.
test-fuzz:
	$(Q)rm -f $(DISK_IMG) $(CRYPT_IMG)
	$(Q)$(MAKE) --no-print-directory disk crypt-disk
	$(Q)$(MAKE) --no-print-directory -C tests/host fuzz
	$(Q)tests/host/build/fuzz-mount --image $(abspath $(DISK_IMG)) --cases 1 --selftest
	$(Q)tests/host/build/fuzz-mount --image $(abspath $(DISK_IMG)) --crypt-image $(abspath $(CRYPT_IMG)) --cases $(FUZZ_CASES) --crypt-cases $(FUZZ_CRYPT_CASES) --seed $(FUZZ_SEED) --timeout $(FUZZ_TIMEOUT_SEC) --crash-dir $(abspath $(BUILD_ROOT))/fuzz

# Userspace gate: boot with user.test=1, so the kernel loads three ELF programs
# from the root filesystem, runs them at CPL3, checks that a kernel pointer is
# refused with EFAULT, and that a user-mode page fault kills only that program.
test-user: EXPECT_USER := yes
test-user: esp user
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(USER_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=1"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Address-space gate: boot with user.test=2, so the kernel runs the three U1
# programs and then starts two copies of /bin/iso at the same time. Each copy
# writes its own tag to 0x8000030000, yields, and reads it back, and the kernel
# checks that the same virtual address maps to different physical frames.
test-user2: EXPECT_USER := yes
test-user2: EXPECT_USERS := yes
test-user2: esp user
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(USER2_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=2"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Ring-3 shell gate: boot with user.test=3, so the kernel also runs /bin/sh at
# CPL3. The shell reads /etc/sh.rc through SYS_OPEN/SYS_READ, lists the root,
# writes /shout.txt and exits 7; check-disk proves the file survived umount.
test-user3: EXPECT_USER := yes
test-user3: EXPECT_USERS := yes
test-user3: EXPECT_USHELL := yes
test-user3: esp user
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(SHELL_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=3"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG) --expect-shell-write

# Spawn/exec gate: boot with user.test=4, so /bin/spawn starts /bin/child with
# SYS_SPAWN, blocks in SYS_WAIT for its exit code and then replaces itself with
# /bin/child through SYS_EXEC. Both child images exit 5.
test-user4: EXPECT_USER := yes
test-user4: EXPECT_USERS := yes
test-user4: EXPECT_USHELL := yes
test-user4: EXPECT_SPAWN := yes
test-user4: esp user
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(SPAWN_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=4"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL) $(EXPECT_SPAWN)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Fork gate: boot with user.test=5, so /bin/fork calls SYS_FORK. The child
# shares the parent's frames read-only, stores two new values (both stores
# fault and take a private copy) and exits 3; the parent must still read its
# own copy (0x1111/0x3333), which only holds if the fault path kept the two
# spaces private.
test-user5: EXPECT_USER := yes
test-user5: EXPECT_USERS := yes
test-user5: EXPECT_USHELL := yes
test-user5: EXPECT_SPAWN := yes
test-user5: EXPECT_FORK := yes
test-user5: esp user
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(FORK_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=5"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL) $(EXPECT_SPAWN) $(EXPECT_FORK)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

# Large-fork gate: boot with user.test=6, so /bin/forkbig forks an image with
# 300 mapped pages. The U5 cap was 256 pages because every page was copied;
# sharing frames lifts it, and the child's edits to the first and last byte
# must stay its own.
test-user6: EXPECT_USER := yes
test-user6: EXPECT_USERS := yes
test-user6: EXPECT_USHELL := yes
test-user6: EXPECT_SPAWN := yes
test-user6: EXPECT_FORK := yes
test-user6: EXPECT_FORKBIG := yes
test-user6: esp user
	$(Q)rm -f $(DISK_IMG)
	$(Q)$(MAKE) --no-print-directory disk
	$(Q)$(MAKE) --no-print-directory run TIMEOUT_SEC=$(FORKBIG_RUN_TIMEOUT_SEC) \
		CMDLINE_EXTRA="user.test=6"
	$(Q)$(PYTHON) scripts/boot-check.py $(SERIAL_LOG) $(QEMU_EXIT) VERSION \
		$(EXPECT_AVX2) no no no no no any no no no no no no no no no no no no no no no no \
		$(EXPECT_USER) $(EXPECT_USERS) $(EXPECT_USHELL) $(EXPECT_SPAWN) $(EXPECT_FORK) $(EXPECT_FORKBIG)
	$(Q)$(PYTHON) scripts/check-disk.py $(DISK_IMG)

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
	@printf '  %-16s %s\n' EXPECT_WB "$(EXPECT_WB)"
	@printf '  %-16s %s\n' WB_WAIT "$(WB_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_DRAG "$(WB_DRAG)"
	@printf '  %-16s %s\n' WB_RUN_TIMEOUT_SEC "$(WB_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' WB_QEMU_EXTRA "$(WB_QEMU_EXTRA)"
	@printf '  %-16s %s\n' EXPECT_MENU "$(EXPECT_MENU)"
	@printf '  %-16s %s\n' WB_MENU_WAIT "$(WB_MENU_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_MENU_ACTS "$(WB_MENU_ACTS)"
	@printf '  %-16s %s\n' WB_MENU_RUN_TIMEOUT_SEC "$(WB_MENU_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' EXPECT_SHELL "$(EXPECT_SHELL)"
	@printf '  %-16s %s\n' WB_SHELL_WAIT "$(WB_SHELL_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_SHELL_CMDS "$(WB_SHELL_CMDS)"
	@printf '  %-16s %s\n' WB_SHELL_RUN_TIMEOUT_SEC "$(WB_SHELL_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' EXPECT_KEYS "$(EXPECT_KEYS)"
	@printf '  %-16s %s\n' WB_KEYS_WAIT "$(WB_KEYS_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_KEYS_ACTS "$(WB_KEYS_ACTS)"
	@printf '  %-16s %s\n' WB_KEYS_RUN_TIMEOUT_SEC "$(WB_KEYS_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' EXPECT_DESK "$(EXPECT_DESK)"
	@printf '  %-16s %s\n' WB_DESK_WAIT "$(WB_DESK_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_DESK_ACTS "$(WB_DESK_ACTS)"
	@printf '  %-16s %s\n' WB_DESK_RUN_TIMEOUT_SEC "$(WB_DESK_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' EXPECT_ICONS "$(EXPECT_ICONS)"
	@printf '  %-16s %s\n' WB_ICONS_WAIT "$(WB_ICONS_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_ICONS_ACTS "$(WB_ICONS_ACTS)"
	@printf '  %-16s %s\n' WB_ICONS_RUN_TIMEOUT_SEC "$(WB_ICONS_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' EXPECT_DRAWER "$(EXPECT_DRAWER)"
	@printf '  %-16s %s\n' WB_DRAWER_WAIT "$(WB_DRAWER_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_DRAWER_ACTS "$(WB_DRAWER_ACTS)"
	@printf '  %-16s %s\n' WB_DRAWER_FILL "$(WB_DRAWER_FILL)"
	@printf '  %-16s %s\n' WB_DRAWER_RUN_TIMEOUT_SEC "$(WB_DRAWER_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' EXPECT_SELECT "$(EXPECT_SELECT)"
	@printf '  %-16s %s\n' WB_SELECT_WAIT "$(WB_SELECT_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_SELECT_ACTS "$(WB_SELECT_ACTS)"
	@printf '  %-16s %s\n' WB_SELECT_FILL "$(WB_SELECT_FILL)"
	@printf '  %-16s %s\n' WB_SELECT_RUN_TIMEOUT_SEC "$(WB_SELECT_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' WB_SCREEN_WAIT "$(WB_SCREEN_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_SCREEN_ACTS "$(WB_SCREEN_ACTS)"
	@printf '  %-16s %s\n' WB_SCREEN_RUN_TIMEOUT_SEC "$(WB_SCREEN_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' WB_CX_WAIT "$(WB_CX_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_CX_ACTS "$(WB_CX_ACTS)"
	@printf '  %-16s %s\n' WB_CX_RUN_TIMEOUT_SEC "$(WB_CX_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' WB_APP_WAIT "$(WB_APP_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_APP_ACTS "$(WB_APP_ACTS)"
	@printf '  %-16s %s\n' WB_APP_RUN_TIMEOUT_SEC "$(WB_APP_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' APP_RUN_TIMEOUT_SEC "$(APP_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' WB_PKG_WAIT "$(WB_PKG_WAIT) (seconds)"
	@printf '  %-16s %s\n' WB_PKG_RUN_TIMEOUT_SEC "$(WB_PKG_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' GPU_WB_WAIT "$(GPU_WB_WAIT) (seconds)"
	@printf '  %-16s %s\n' GPU_RUN_TIMEOUT_SEC "$(GPU_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' GPU_QEMU_EXTRA "$(GPU_QEMU_EXTRA)"
	@printf '  %-16s %s\n' GPU_USB_QEMU_EXTRA "$(GPU_USB_QEMU_EXTRA)"
	@printf '  %-16s %s\n' GPU_EDID_DEVICE "$(GPU_EDID_DEVICE)"
	@printf '  %-16s %s\n' GPU_EDID_QEMU_EXTRA "$(GPU_EDID_QEMU_EXTRA)"
	@printf '  %-16s %s\n' GPU_SHAPE_WB_WAIT "$(GPU_SHAPE_WB_WAIT) (seconds)"
	@printf '  %-16s %s\n' GPU_SHAPE_BOOT_TIMEOUT_SEC "$(GPU_SHAPE_BOOT_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' GPU_SHAPE_TIMEOUT_SEC "$(GPU_SHAPE_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' GPU_SHAPE_RUN_TIMEOUT_SEC "$(GPU_SHAPE_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' GPU_SHAPE_QEMU_EXTRA "$(GPU_SHAPE_QEMU_EXTRA)"
	@printf '  %-16s %s\n' EXPECT_PKG "$(EXPECT_PKG)"
	@printf '  %-16s %s\n' PKG_RUN_TIMEOUT_SEC "$(PKG_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_CAPS "$(EXPECT_CAPS)"
	@printf '  %-16s %s\n' CAPS_RUN_TIMEOUT_SEC "$(CAPS_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' MKPKG "$(MKPKG)"
	@printf '  %-16s %s\n' PKG_DATA_DIR "$(PKG_DATA_DIR)"
	@printf '  %-16s %s\n' PKG_SEED "$(PKG_SEED)"
	@printf '  %-16s %s\n' PKG_BAD_SEED "$(PKG_BAD_SEED)"
	@printf '  %-16s %s\n' HARDEN_TIMEOUT_SEC "$(HARDEN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_HEAP "$(EXPECT_HEAP)"
	@printf '  %-16s %s\n' HEAP_STRESS_OPS "$(HEAP_STRESS_OPS)"
	@printf '  %-16s %s\n' HEAP_RUN_TIMEOUT_SEC "$(HEAP_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' TRAP_TIMEOUT_SEC "$(TRAP_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' S1_RUN_TIMEOUT_SEC "$(S1_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_USER "$(EXPECT_USER)"
	@printf '  %-16s %s\n' USER_RUN_TIMEOUT_SEC "$(USER_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_USERS "$(EXPECT_USERS)"
	@printf '  %-16s %s\n' USER2_RUN_TIMEOUT_SEC "$(USER2_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_USHELL "$(EXPECT_USHELL)"
	@printf '  %-16s %s\n' SHELL_RUN_TIMEOUT_SEC "$(SHELL_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_SPAWN "$(EXPECT_SPAWN)"
	@printf '  %-16s %s\n' SPAWN_RUN_TIMEOUT_SEC "$(SPAWN_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_FORK "$(EXPECT_FORK)"
	@printf '  %-16s %s\n' FORK_RUN_TIMEOUT_SEC "$(FORK_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' EXPECT_FORKBIG "$(EXPECT_FORKBIG)"
	@printf '  %-16s %s\n' FORKBIG_RUN_TIMEOUT_SEC "$(FORKBIG_RUN_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' FUZZ_CASES "$(FUZZ_CASES)"
	@printf '  %-16s %s\n' FUZZ_CRYPT_CASES "$(FUZZ_CRYPT_CASES)"
	@printf '  %-16s %s\n' FUZZ_SEED "$(FUZZ_SEED)"
	@printf '  %-16s %s\n' FUZZ_TIMEOUT_SEC "$(FUZZ_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' MEMORY_MB "$(MEMORY_MB)"
	@printf '  %-16s %s\n' SMP "$(SMP)"
	@printf '  %-16s %s\n' TIMEOUT_SEC "$(TIMEOUT_SEC)"
	@printf '  %-16s %s\n' QEMU_ACCEL "$(QEMU_ACCEL)"
	@printf '  %-16s %s\n' QEMU_DISPLAY "$(QEMU_DISPLAY)"
	@printf '  %-16s %s\n' QEMU_EXTRA "$(QEMU_EXTRA)"
	@printf '  %-16s %s\n' MACHINE_EXTRA "$(MACHINE_EXTRA)"
	@printf '  %-16s %s\n' DEMO_DISPLAY "$(DEMO_DISPLAY)"
	@printf '  %-16s %s\n' DEMO_HOLD_SEC "$(DEMO_HOLD_SEC) (seconds)"
	@printf '  %-16s %s\n' DEMO_FILL_FILES "$(DEMO_FILL_FILES) (files)"
	@printf '  %-16s %s\n' DEMO_GPU "$(DEMO_GPU) (1 = virtio-gpu scanout, 0 = software framebuffer)"
	@printf '  %-16s %s\n' DEMO_GPU_SIZE "$(DEMO_GPU_SIZE) (scanout the demo asks for)"
	@printf '  %-16s %s\n' DEMO_GPU_QEMU_EXTRA "$(DEMO_GPU_QEMU_EXTRA)"
	@printf '  %-16s %s\n' DEMO_CURSOR "$(DEMO_CURSOR) (sw = guest sprite, hw = device cursor plane)"
	@printf '  %-16s %s\n' DEMO_TIMEOUT_SEC "$(DEMO_TIMEOUT_SEC) (seconds)"
	@printf '  %-16s %s\n' DEMO_CMDLINE "$(DEMO_CMDLINE)"
	@printf '  %-16s %s\n' DEMO_MACHINE_EXTRA "$(DEMO_MACHINE_EXTRA)"
	@printf '  %-16s %s\n' DEMO_QEMU_EXTRA "$(DEMO_QEMU_EXTRA)"
	@printf '  %-16s %s\n' EXPECT_ISO "$(EXPECT_ISO)"
	@printf '  %-16s %s\n' ISO_IMAGE "$(ISO_IMAGE)"
	@printf '  %-16s %s\n' ISO_STAGE "$(ISO_STAGE)"
	@printf '  %-16s %s\n' ISO_FAT_IMG "$(ISO_FAT_IMG)"
	@printf '  %-16s %s\n' ISO_SIZE_MB "$(ISO_SIZE_MB)"
	@printf '  %-16s %s\n' ISO_VOLID "$(ISO_VOLID)"
	@printf '  %-16s %s\n' ISO_CMDLINE "$(ISO_CMDLINE)"
	@printf '  %-16s %s\n' ISO_DATE "$(ISO_DATE)"
	@printf '  %-16s %s\n' ISO_RUN_TIMEOUT_SEC "$(ISO_RUN_TIMEOUT_SEC)"
	@printf '  %-16s %s\n' ISO_IMG "$(ISO_IMG)"
	@printf '  %-16s %s\n' USB_IMG "$(USB_IMG)"
	@printf '  %-16s %s\n' USB_STICK "$(USB_STICK)"
	@printf '  %-16s %s\n' USB_DEV "$(USB_DEV)"
	@printf '  %-16s %s\n' USB_CONFIRM "$(USB_CONFIRM)"
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
