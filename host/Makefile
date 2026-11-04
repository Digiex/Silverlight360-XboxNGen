# Builds ngenhost.xex, the title that runs xcoreclr's zapper, with xsdk's toolchain. In WSL: make
# tools\Configure.ps1 records the xsdk checkout in ../config.mk; XSDK=... on the command line overrides it.
-include $(dir $(lastword $(MAKEFILE_LIST)))../config.mk
XSDK ?= $(error set XSDK to an xsdk checkout with libxsdk.a built: run tools\Configure.ps1, or make XSDK=~/xsdk)
TOOLCHAIN ?= $(HOME)/xsdk-gcc/bin/powerpc-xsdk-elf-
CC := $(TOOLCHAIN)gcc
BUILD := build
# The zapper recurses deeply through type loading; give it room.
STACK := 0x400000

# r16 holds the current Thread in xcoreclr, whose callbacks read it without reloading it: never touched here.
CFLAGS := -mcpu=cell -ffixed-r14 -ffixed-r15 -ffixed-r16 -fno-pic -O2 -g -std=gnu17 -Wall -Wextra -Werror -I$(XSDK)/include
LIBS := -Wl,--start-group $(XSDK)/build/libxsdk.a -lc -lm -lgcc -Wl,--end-group

all: ngenhost.xex

ngenhost.xex: $(BUILD)/ngenhost.elf
	python3 $(XSDK)/tools/elf2xex.py --title-id 0x584E474E --title-name "XboxNGen host" --stack-size $(STACK) $< $@

$(BUILD)/ngenhost.elf: $(BUILD)/ngenhost.o
	$(CC) -static -nostdlib -T $(XSDK)/src/xsdk.ld -Wl,-Map,$(BUILD)/ngenhost.map -o $@ $< $(LIBS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) ngenhost.xex

.PHONY: all clean
