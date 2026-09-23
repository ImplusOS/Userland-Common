.PHONY: all clean copy_resources

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
          -I../../../libc/I_libc/Source/include -I../../../libc/I_libc/Source/include/sys \
          -I../../Source \
          -I../../API/Source \
          -Wall -Wextra -Wtype-limits -Wconversion -Wsign-conversion -Wshadow -MMD -MP
LDFLAGS := -T ../../Source/Userland.ld -e _start -nostdlib --build-id=none

RESOURCE_DIR := Resource
JP_FONT_SRC := ../../../BootManager/Source/Resource/Fonts/NotoSansJP-Regular.ttf

# Resource/chrome is an unmodified upstream Chromium binary (~500 MiB,
# ELFOSABI_LINUX): not built here, only staged. Not committed to git (see
# .gitignore) -- placed by hand, same as Doom/Resource/linuxxdoom-x86_64.
all: $(ELF) copy_resources

copy_resources:
	@if [ -d $(RESOURCE_DIR) ]; then \
		mkdir -p $(BUILD_DIR); \
		cp -a $(RESOURCE_DIR) $(BUILD_DIR)/; \
		if [ -f $(JP_FONT_SRC) ]; then \
			mkdir -p $(BUILD_DIR)/$(RESOURCE_DIR)/fonts; \
			cp -f $(JP_FONT_SRC) $(BUILD_DIR)/$(RESOURCE_DIR)/fonts/; \
		fi; \
	fi

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(APP_OBJS:.o=.d) $(COMMON_DEPS)
