ifeq ($(strip $(ARCH)),)
$(error ARCH must be supplied by the top-level Makefile)
endif

ifeq ($(ARCH),arm64)
CROSS_COMPILE ?= aarch64-elf-
USERLAND_ARCH_CFLAGS ?= -mstrict-align -mno-outline-atomics
else ifeq ($(ARCH),x86_64)
CROSS_COMPILE ?= x86_64-elf-
USERLAND_ARCH_CFLAGS ?= -mcmodel=large -mno-red-zone
else
$(error Unsupported ARCH '$(ARCH)'. Use x86_64 or arm64.)
endif

CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
LD := $(CROSS_COMPILE)ld
AR := $(CROSS_COMPILE)ar
ARCH_CFLAGS := $(USERLAND_ARCH_CFLAGS)

TOP_BUILD_DIR ?= ../../Build/$(ARCH)
COMMON_LIBS_DIR := $(TOP_BUILD_DIR)/Userland

LIBRARY_SRCS := $(shell find ../../Library/Source -name "*.c" 2>/dev/null)
COMMON_LIBRARY_OBJS := $(patsubst ../../Library/Source/%.c,$(TOP_BUILD_DIR)/Library/%.o,$(LIBRARY_SRCS))

COMMON_OBJS := $(COMMON_LIBS_DIR)/Syscalls.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/string.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/iconv.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/stdlib.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/errno.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/posix.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/dlfcn.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/sys/syscalls.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/sys/$(ARCH)/hal_syscall.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/sys/$(ARCH)/setjmp.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/assert.o \
               $(COMMON_LIBS_DIR)/API/XMLParser.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/math.o \
               $(COMMON_LIBS_DIR)/libc/I_libc/src/stdio.o \
               $(COMMON_LIBS_DIR)/Service/service_client.o \
               $(COMMON_LIBS_DIR)/Service/com.ImplusOS.netstack/DNS/DNS.o \
               $(COMMON_LIBRARY_OBJS)

COMMON_DEPS :=
