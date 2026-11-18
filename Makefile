# Project layout
#   bootloader/   UEFI application that loads the kernel and injects KIPs
#   kernel/       Mesosphere patches and the generic-aarch64 board files
#   sysmodules/   custom system modules, each with its own Makefile (test-only
#                 ones in sysmodules/tests/), and the
#                 patches to Atmosphere's userland that they need
#   scripts/      disk-image builders and the local QEMU runner
#   kips/         KIPs copied onto the boot disk by the scripts
#   vendor/       third-party sources (git submodules)
#   build/        every build output (ignored by git)

CC      = clang
LINK    = lld-link

TARGET  = --target=aarch64-unknown-windows
CFLAGS  = $(TARGET) -ffreestanding

BUILD   = build

BOOT_DIR  = bootloader
BOOT_OBJ  = $(BUILD)/bootloader
SRCS    = $(BOOT_DIR)/efi_boot.c $(BOOT_DIR)/kip.c
OBJS    = $(patsubst $(BOOT_DIR)/%.c,$(BOOT_OBJ)/%.o,$(SRCS))
OUT     = $(BUILD)/BOOTAA64.EFI

ATMOS   = vendor/atmosphere
# Two independent patch sets for the vendor tree. They touch disjoint files, so
# either can be applied or reversed without the other.
#   kernel/patches       the kernel: libmesosphere, kernel_ldr, their build files
#   sysmodules/patches   userland: libstratosphere and Atmosphere's system modules
PATCHES           = $(sort $(wildcard kernel/patches/*.patch))
SYSMODULE_PATCHES = $(sort $(wildcard sysmodules/patches/*.patch))
BOARD   = kernel/board/generic/aarch64

# ---------------------------------------------------------------------------
# Default: build the UEFI application
# ---------------------------------------------------------------------------
.PHONY: bootloader
bootloader: $(OUT)

$(OUT): $(OBJS)
	$(LINK) /subsystem:efi_application /entry:efi_main /dynamicbase $(OBJS) /out:$(OUT)

$(BOOT_OBJ)/%.o: $(BOOT_DIR)/%.c $(wildcard $(BOOT_DIR)/*.h) | $(BOOT_OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_OBJ):
	mkdir -p $@

# ---------------------------------------------------------------------------
# setup: apply the kernel patches + create symlinks into vendor/atmosphere
#
# This is all the kernel needs. The userland patches are separate; see
# setup-sysmodules below.
#
# Patching is idempotent and independent of the symlinks: each patch is
# applied only if it is not already present, so `make setup` repairs the tree
# after a manual `git reset --hard` in the submodule. The check is per patch,
# which relies on patches not modifying the same hunks as one another.
# ---------------------------------------------------------------------------
# The board files live in $(BOARD), split into subfolders that mirror where
# upstream boards (nintendo/nx, qemu/virt) keep theirs. Each LINK_* below maps
# one vendor location to its subfolder; the two *.board.*.cpp files are linked
# individually because upstream keeps those directly in libmesosphere/source.
LINK_CONFIG  = $(ATMOS)/libraries/config/board/generic/aarch64
LINK_KINC    = $(ATMOS)/libraries/libmesosphere/include/mesosphere/board/generic/aarch64
LINK_KSRC    = $(ATMOS)/libraries/libmesosphere/source/board/generic/aarch64
LINK_SVC     = $(ATMOS)/libraries/libvapours/include/vapours/svc/board/generic/aarch64
LINK_DBGLOG  = $(ATMOS)/libraries/libmesosphere/source/kern_debug_log_impl.board.generic_aarch64.cpp
LINK_MEMLAY  = $(ATMOS)/libraries/libmesosphere/source/kern_k_memory_layout.board.generic_aarch64.cpp

LINKS = $(LINK_CONFIG) $(LINK_KINC) $(LINK_KSRC) $(LINK_SVC) $(LINK_DBGLOG) $(LINK_MEMLAY)

reverse = $(if $(1),$(call reverse,$(wordlist 2,$(words $(1)),$(1))) $(firstword $(1)))

# apply-patches: apply each patch in $(1) that is not already applied.
define apply-patches
	@for p in $(1); do \
		if git -C $(ATMOS) apply --reverse --check $(CURDIR)/$$p 2>/dev/null; then \
			: already applied; \
		elif git -C $(ATMOS) apply --check $(CURDIR)/$$p 2>/dev/null; then \
			echo "applying $$p"; \
			git -C $(ATMOS) apply $(CURDIR)/$$p || exit 1; \
		else \
			echo "error: $$p neither applies nor is already applied to $(ATMOS)." >&2; \
			echo "       The vendor tree is in an unexpected state; inspect it with 'git -C $(ATMOS) status'." >&2; \
			exit 1; \
		fi; \
	done
endef

# revert-patches: reverse each patch in $(1) that is applied, last first.
define revert-patches
	@for p in $(call reverse,$(1)); do \
		if git -C $(ATMOS) apply --reverse --check $(CURDIR)/$$p 2>/dev/null; then \
			echo "reverting $$p"; \
			git -C $(ATMOS) apply --reverse $(CURDIR)/$$p || exit 1; \
		fi; \
	done
endef

.PHONY: setup patch unsetup
setup: patch $(LINKS)

patch:
	$(call apply-patches,$(PATCHES))

# unsetup: remove the symlinks and reverse the kernel patches, leaving any
# other changes and all build output in the vendor tree alone.
unsetup:
	@for l in $(LINKS); do \
		if [ -L $$l ]; then rm $$l; fi; \
	done
	@rmdir $(dir $(LINK_KINC)) $(dir $(LINK_KSRC)) $(dir $(LINK_SVC)) 2>/dev/null || true
	$(call revert-patches,$(PATCHES))

# ---------------------------------------------------------------------------
# setup-sysmodules: apply the userland patches (sysmodules/patches)
#
# Needed to build sysmodules/fs and Atmosphere's own system modules (pm,
# loader) for this OS; the kernel does not depend on it. sysmodules/fs runs it
# before building. Idempotent, like setup.
# ---------------------------------------------------------------------------
.PHONY: setup-sysmodules unsetup-sysmodules
setup-sysmodules:
	$(call apply-patches,$(SYSMODULE_PATCHES))

unsetup-sysmodules:
	$(call revert-patches,$(SYSMODULE_PATCHES))

$(LINK_CONFIG):
	ln -s ../../../../../../$(BOARD)/config $@

$(LINK_KINC):
	mkdir -p $(dir $@)
	ln -s ../../../../../../../../$(BOARD)/libmesosphere/include $@

$(LINK_KSRC):
	mkdir -p $(dir $@)
	ln -s ../../../../../../../$(BOARD)/libmesosphere/source/board $@

$(LINK_SVC):
	mkdir -p $(dir $@)
	ln -s ../../../../../../../../../$(BOARD)/libvapours/svc $@

$(LINK_DBGLOG) $(LINK_MEMLAY):
	ln -s ../../../../../$(BOARD)/libmesosphere/source/$(notdir $@) $@

# ---------------------------------------------------------------------------
# kernel: build Mesosphere for generic-aarch64 (requires setup first)
# ---------------------------------------------------------------------------
KERN_OUT  = vendor/atmosphere/mesosphere/kernel/out/generic_aarch64_arm64_armv8a/debug/kernel.bin
KLDR_OUT  = vendor/atmosphere/mesosphere/kernel_ldr/out/generic_aarch64_arm64_armv8a/debug/kernel_ldr.bin
BUILD_MESO = vendor/atmosphere/mesosphere/build_mesosphere.py
MESO_OUT  = $(BUILD)/kernel.bin

.PHONY: kernel
kernel: setup | $(BOOT_OBJ)
	$(MAKE) -C $(ATMOS)/libraries/libmesosphere generic_aarch64_a57_debug
	$(MAKE) -C $(ATMOS)/mesosphere/kernel_ldr generic_aarch64_a57_debug
	$(MAKE) -C $(ATMOS)/mesosphere/kernel generic_aarch64_a57_debug
	python3 $(BUILD_MESO) $(KLDR_OUT) $(KERN_OUT) $(MESO_OUT)

# ---------------------------------------------------------------------------
# run / images
# ---------------------------------------------------------------------------
.PHONY: run os run-os
run:
	scripts/run-qemu.sh

# os: build everything a configuration lists (bootloader, kernel, system
# modules) and package it as one bootable disk image.
#   make os                              os.conf -> build/os/default/disk.img
#   make os CONFIG=configs/minimal.conf           -> build/os/minimal/disk.img
CONFIG ?= os.conf
os:
	scripts/build-os.py $(CONFIG)

run-os:
	scripts/run-os.sh $(if $(filter os.conf,$(CONFIG)),build/os/default/disk.img,build/os/$(basename $(notdir $(CONFIG)))/disk.img)

# ---------------------------------------------------------------------------
# clean
# ---------------------------------------------------------------------------
.PHONY: clean clean-kernel
clean:
	rm -f $(OBJS) $(OUT) $(MESO_OUT)

clean-kernel:
	$(MAKE) -C $(ATMOS)/libraries/libmesosphere clean-generic_aarch64_a57_debug 2>/dev/null; true
	$(MAKE) -C $(ATMOS)/mesosphere/kernel_ldr  clean-generic_aarch64_a57_debug 2>/dev/null; true
	$(MAKE) -C $(ATMOS)/mesosphere/kernel      clean-generic_aarch64_a57_debug 2>/dev/null; true
