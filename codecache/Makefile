# Builds CLRCodeCachedll_0.xex, the module xcoreclr takes executable memory from (codecache.c). In WSL: make
# tools\Configure.ps1 records the xsdk checkout in ../config.mk; XSDK=... on the command line overrides it.
-include $(dir $(lastword $(MAKEFILE_LIST)))../config.mk
XSDK ?= $(error set XSDK to an xsdk checkout with libxsdk.a built: run tools\Configure.ps1, or make XSDK=~/xsdk)
TOOLCHAIN ?= $(HOME)/xsdk-gcc/bin/powerpc-xsdk-elf-
CC := $(TOOLCHAIN)gcc
BUILD := build
BASE := 8D000000

CFLAGS := -mcpu=cell -fno-pic -O2 -std=gnu17 -Wall -Wextra -Werror -ffreestanding -fno-builtin -I$(XSDK)/include

all: CLRCodeCachedll_0.xex

CLRCodeCachedll_0.xex: $(BUILD)/codecache.elf $(XSDK)/tools/elf2xex.py
	python3 $(XSDK)/tools/elf2xex.py --dll --base $(BASE) --export-name LINKHACKFUNCTIONPROC0 \
		--export-name LINKHACKFUNCTIONPROCLAST $< $@

$(BUILD)/codecache.elf: $(BUILD)/codecache.o codecache.ld
	$(CC) -static -nostdlib -T codecache.ld -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) CLRCodeCachedll_0.xex

.PHONY: all clean
