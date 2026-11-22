# ImplusOS Translator -- runs Linux programs built for the other CPU.
#
# Built for the host ARCH, with the guest it runs on that host:
#   ARCH=arm64   -> Build/arm64/Userland/Translator/x86_64.ELF   (x86_64 guests)
#   ARCH=x86_64  -> Build/x86_64/Userland/Translator/aarch64.ELF (AArch64 guests)
# The image stages this directory as /Userland/Translator/, where the
# kernel looks for <guest>.ELF (OS_CONFIG_TRANSLATOR_DIR).
#
# The output is a static Linux-ABI program (EI_OSABI = ELFOSABI_LINUX) linked
# at the native code base, without a C library. Not built with -Wconversion:
# an instruction interpreter is all deliberate truncation.

.PHONY: all clean

ifeq ($(strip $(ARCH)),)
$(error ARCH must be supplied by the top-level Makefile)
endif

# GUEST can be overridden to build a same-CPU translator, which is only
# useful for testing the interpreter (GUEST=aarch64 ARCH=arm64 runs AArch64
# programs on an AArch64 host, natively checkable).
ifeq ($(ARCH),arm64)
CROSS_COMPILE ?= aarch64-elf-
GUEST         ?= x86_64
HOST_CFLAGS   := -mno-outline-atomics
else ifeq ($(ARCH),x86_64)
CROSS_COMPILE ?= x86_64-elf-
GUEST         ?= aarch64
HOST_CFLAGS   := -mno-red-zone
else
$(error Unsupported ARCH '$(ARCH)')
endif

CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld

TOP_BUILD_DIR ?= ../../../Build/$(ARCH)
BUILD_DIR := $(TOP_BUILD_DIR)/Userland/Translator
OBJ_DIR   := $(TOP_BUILD_DIR)/Obj/Translator
ELF       := $(BUILD_DIR)/$(GUEST).ELF

# Where the translator itself is linked (USER_CODE_BASE); the programs it
# runs are mapped wherever the host's mmap() puts them.
LINK_BASE := 0x4000000000

GUEST_DIR := Source/guest/$(GUEST)
SRCS := $(wildcard Source/*.c) $(wildcard $(GUEST_DIR)/*.c)
ASMS := Source/host_$(ARCH).S
OBJS := $(SRCS:%.c=$(OBJ_DIR)/%.o) $(ASMS:%.S=$(OBJ_DIR)/%.o)

CFLAGS := -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fpie \
          -fno-tree-loop-distribute-patterns -fno-math-errno \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          $(HOST_CFLAGS) -ISource -I$(GUEST_DIR) \
          -Wall -Wextra -Wshadow -MMD -MP $(XL_EXTRA_CFLAGS)
LDFLAGS := -static -nostdlib --build-id=none -z noexecstack \
           -Ttext-segment=$(LINK_BASE) -e _start --gc-sections
LIBGCC := $(shell $(CC) -print-libgcc-file-name)

ifeq ($(wildcard $(GUEST_DIR)),)
all:
	@echo "Translator: no $(GUEST) guest yet; skipped for ARCH=$(ARCH)"
else
all: $(ELF)
endif

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ $(LIBGCC) -o $@
	@# EI_OSABI = ELFOSABI_LINUX: the kernel runs it under the Linux ABI.
	@printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc status=none

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d)
