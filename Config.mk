# -*- mode: Makefile; -*-

# A debug build?
debug ?= n

BLKTAP_COMPILE_ARCH    ?= $(shell uname -m | sed -e s/i.86/x86_32/ \
                            -e s/ppc/powerpc/ -e s/i86pc/x86_32/)
BLKTAP_TARGET_ARCH     ?= $(BLKTAP_COMPILE_ARCH)
BLKTAP_OS              ?= $(shell uname -s)

CONFIG_$(BLKTAP_OS) := y

SHELL     ?= /bin/sh

DESTDIR     ?= /

include $(BLKTAP_ROOT)/config/$(BLKTAP_OS).mk
include $(BLKTAP_ROOT)/config/$(BLKTAP_TARGET_ARCH).mk

# cc-option: Check if compiler supports first option, else fall back to second.
# Usage: cflags-y += $(call cc-option,$(CC),-march=winchip-c6,-march=i586)
cc-option = $(shell if test -z "`$(1) $(2) -S -o /dev/null -xc \
              /dev/null 2>&1`"; then echo "$(2)"; else echo "$(3)"; fi ;)

CFLAGS += -g

CFLAGS += -fno-strict-aliasing

CFLAGS += -std=gnu99

CFLAGS += -Wall -Wstrict-prototypes

# -Wunused-value makes GCC 4.x too aggressive for my taste: ignoring the
# result of any casted expression causes a warning.
CFLAGS += -Wno-unused-value

CFLAGS     += $(call cc-option,$(CC),-Wdeclaration-after-statement,)
