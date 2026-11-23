.PHONY: all clean

APP_NAME := $(notdir $(patsubst %/,%,$(CURDIR)))
SRC_DIR  := .
TOP_BUILD_DIR ?= ../../../Build/$(ARCH)
BUILD_DIR := $(TOP_BUILD_DIR)/Userland/$(APP_NAME)
ELF      := $(BUILD_DIR)/$(APP_NAME).ELF

APP_SRC := $(shell find $(SRC_DIR) -name '*.c')
APP_OBJS := $(APP_SRC:%.c=$(BUILD_DIR)/%.o)

include ../../Source/AppCommon.mk

OBJS := $(APP_OBJS) $(COMMON_OBJS)

CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-builtin \
          $(ARCH_CFLAGS) -nostdlib -nostartfiles -nodefaultlibs \
          -I. \
          -I../../../libc/I_libc/Source/include -I../../../libc/I_libc/Source/include/sys \
          -I../../Source \
          -I../../API/Source \
          -I../../../Library/Source \
          -I../../../Vendor \
          -Wall -Wextra -Wtype-limits -Wconversion -Wsign-conversion -Wshadow -MMD -MP \
          -O2 -ffunction-sections -fdata-sections
LDFLAGS := -T ../../Source/Userland.ld -e _start -nostdlib --build-id=none --gc-sections

all: $(ELF)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(APP_OBJS:.o=.d) $(COMMON_DEPS)
