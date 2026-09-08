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

# Try BOOT_NONE first and read --print-memory-usage. smack-versio had to take
# BOOT_SRAM because its image was ~137 KB against the STM32H750's 128 KB of
# internal flash, but that build carries the USB serial logger (7.1 KB on its
# own) which this one does not. If FLASH exceeds 131072 B, switch to BOOT_SRAM
# and follow smack-versio's FLASHING.md — the tradeoff is a bootloader install
# and the ~2000 ms DFU window, versus flashing straight from ST ROM DFU.
APP_TYPE ?= BOOT_NONE

CPP_SOURCES = smack_patch.cpp

# smack_core_patch.c #includes vendor/smack_core.c with calloc/free redirected
# to the SDRAM pool. Never list vendor/smack_core.c here as well — that would
# define every engine symbol twice.
C_SOURCES = clock_adapter.c patch_alloc.c smack_core_patch.c

# The engine ships lo-fi varispeed on purpose; -ffast-math would also let the
# compiler reorder the float paths the seeded FX depend on. Leave it off.
OPT = -Os

# NOTE: no `-u _printf_float` here. That flag is needed whenever newlib-nano
# has to format a %f — it silently prints nothing otherwise. This firmware
# formats only %d and %s (BPM is cast to int before printing). If you add a
# %f to the display code you must add the flag or the field renders empty.

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
