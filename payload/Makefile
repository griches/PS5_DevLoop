# PS5 DevLoop - resident input and screen-capture payload build.
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds build/ps5loop.elf. Run tools/build.sh, which resolves PS5_PAYLOAD_SDK.

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is undefined; run tools/build.sh)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ROOT := $(abspath $(CURDIR)/..)
OUTPUT := $(ROOT)/build
ELF := $(OUTPUT)/ps5loop.elf

CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror
LDLIBS := -lScePad -lSceUserService -lpthread

all: $(ELF)

$(ELF): main.c | $(OUTPUT)
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

$(OUTPUT):
	mkdir -p $(OUTPUT)

clean:
	rm -f $(ELF)

.PHONY: all clean
