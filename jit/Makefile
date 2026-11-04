# Builds xclrjit.dll, the JIT xcoreclr's zapper loads, with xsdk's toolchain. In WSL: make
# XSDK is an xsdk checkout with a built libxsdk.a; TOOLCHAIN the toolchain's prefix.
# tools\Configure.ps1 records the xsdk checkout in ../config.mk; XSDK=... on the command line overrides it.
-include $(dir $(lastword $(MAKEFILE_LIST)))../config.mk
XSDK ?= $(error set XSDK to an xsdk checkout with libxsdk.a built: run tools\Configure.ps1, or make XSDK=~/xsdk)
TOOLCHAIN ?= $(HOME)/xsdk-gcc/bin/powerpc-xsdk-elf-
CC := $(TOOLCHAIN)gcc
BUILD := build
BASE := 8C800000

SOURCES := xclrjit.c compiler.c
OBJECTS := $(SOURCES:%.c=$(BUILD)/%.o)

# r16 holds the current Thread in xcoreclr, whose callbacks read it without reloading it: never touched here.
# r14 and r15 are the runtime's too (the ephemeral generation's bounds its write barriers read): a value the
# compiler kept there across a callback came back changed (a fixup's kind read back as another, a loop in a stub).
CFLAGS := -mcpu=cell -ffixed-r14 -ffixed-r15 -ffixed-r16 -fno-pic -O2 -g -std=gnu17 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
	-I$(XSDK)/include
# Extra flags for diagnostic builds (make EXTRA_CFLAGS='-include decline.h').
CFLAGS += $(EXTRA_CFLAGS)
LIBS := -Wl,--start-group $(XSDK)/build/libxsdk.a -lc -lgcc -Wl,--end-group

all: xclrjit.dll

xclrjit.dll: $(BUILD)/xclrjit.elf $(XSDK)/tools/elf2xex.py
	python3 $(XSDK)/tools/elf2xex.py --dll --base $(BASE) --export-name sxsJitStartup --export-name getJit $< $@

$(BUILD)/xclrjit.elf: $(OBJECTS) xclrjit.ld
	$(CC) -static -nostdlib -T xclrjit.ld -Wl,-Map,$(BUILD)/xclrjit.map -o $@ $(OBJECTS) $(LIBS)

$(BUILD)/%.o: %.c jitee.h compiler.h gcinfo.h ppc.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) xclrjit.dll

.PHONY: all clean
