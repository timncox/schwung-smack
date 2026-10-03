# Smack — Daisy Patch firmware build.
#
#   make -C firmware              build smack_patch.bin
#   make -C firmware program-dfu  flash over USB
#
# Requires libDaisy checked out and built:
#   git clone --recurse-submodules https://github.com/electro-smith/libDaisy \
#       ~/tim-os/daisy-sdk/libDaisy && make -C ~/tim-os/daisy-sdk/libDaisy
#
# DaisySP is deliberately NOT used — smack_core is self-contained DSP.

TARGET = smack_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy

# BOOT_SRAM, and the history is worth keeping because it is a warning.
#
# The first cut of this port fit BOOT_NONE at 123408 B of 131072 B — 94.15%,
# 7664 B free — and the Makefile said out loud that the next feature would
# push it over. The very next commit added -u _printf_float (~4.5 KB, and NOT
# optional: see the note after the include) plus the CV/gate outputs, and it
# landed at 128296 B — 97.88%, 2776 B free. That is not a margin, it is a
# tripwire for whoever touches this next.
#
#   BOOT_NONE   128296 B / 131072 B internal flash   97.88%
#   BOOT_SRAM   128152 B / 491520 B SRAM             26.07%
#
# Cost of BOOT_SRAM: the module needs the Daisy bootloader in internal flash
# once (see smack-versio's FLASHING.md), and the bootloader's DFU window is
# only ~2000 ms after power-up — arm `dfu-util ... -w` FIRST, then power-cycle
# WITHOUT touching BOOT/RESET. Installing the bootloader is itself done over
# USB from ST ROM DFU, so nothing extra is needed to get there. It is also
# reversible: flashing any BOOT_NONE image over internal flash removes it.
#
# Since 2026-09-09 Belt and Mark are BOOT_SRAM too, for a different reason:
# the module picker (module_picker.cpp) switches firmware from the front panel
# by writing a .bin from the SD card into the bootloader's QSPI app slot, so
# every app has to run under the bootloader. See module_picker.h.
APP_TYPE ?= BOOT_SRAM

# FatFS is not in libdaisy.a; this compiles it into the app for the picker.
USE_FATFS = 1

CPP_SOURCES = smack_patch.cpp module_picker.cpp

# smack_core_patch.c #includes vendor/smack_core.c with calloc/free redirected
# to the SDRAM pool. Never list vendor/smack_core.c here as well — that would
# define every engine symbol twice.
C_SOURCES = clock_adapter.c patch_alloc.c smack_core_patch.c

# The engine ships lo-fi varispeed on purpose; -ffast-math would also let the
# compiler reorder the float paths the seeded FX depend on. Leave it off.
OPT = -Os

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# REQUIRED, and this firmware is the reason the rule exists. newlib-nano
# declares _printf_float as a weak reference that nothing defines under
# --specs=nano.specs; unresolved, vfprintf skips the conversion entirely —
# no error, no warning, no truncation marker — and atoi() then sees 0.
#
# Auditing only this shim's own printf calls would talk you out of the flag:
# it formats %d and %s, and casts BPM to int before printing. But the values
# it READS are formatted by the engine, and the one the CV outputs and the
# loop-wrap gate hang off is a float:
#
#   smack_core.c   "%.0f", floor(play_pos)   play_frame  -> CV Out 1 + Gate Out
#
# play_frame is the canonical case: smack-versio's LIVE mode never re-captured
# on any build because smack_get_param("play_frame") handed back "" and the
# trigger `pf < last_pf` compared 0 < 0. Three sibling params used %d and %u
# and worked fine, so the feature looked enabled and did nothing. Hours went
# to the wrong suspects because every host test passed — glibc formats %f
# perfectly, so this bug class cannot be caught off-device.
#
# Applied after the include so it appends to libDaisy's LDFLAGS. ~4.5 KB.
# Verify on the built ELF:
#   arm-none-eabi-nm build/smack_patch.elf | grep _printf_float
# Empty output means every %f in the image is silently producing nothing.
LDFLAGS += -u _printf_float
