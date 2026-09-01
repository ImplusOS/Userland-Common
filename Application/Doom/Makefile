.PHONY: all clean copy_resources wad

APP_NAME := $(notdir $(patsubst %/,%,$(CURDIR)))
SRC_DIR  := .
TOP_BUILD_DIR ?= ../../../Build/$(ARCH)
BUILD_DIR := $(TOP_BUILD_DIR)/Userland/$(APP_NAME)
ELF      := $(BUILD_DIR)/$(APP_NAME).ELF

APP_SRC := $(shell find $(SRC_DIR) -name '*.c')
APP_OBJS := $(APP_SRC:%.c=$(BUILD_DIR)/%.o)

include ../../AppCommon.mk

OBJS := $(APP_OBJS) $(COMMON_OBJS)

CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-builtin \
          $(ARCH_CFLAGS) -nostdlib -nostartfiles -nodefaultlibs \
          -I../../../libc/I_libc/include -I../../../libc/I_libc/include/sys \
          -I../../../Userland \
          -I../../../Userland/API \
          -Wall -Wextra -Wtype-limits -Wconversion -Wsign-conversion -Wshadow -MMD -MP
LDFLAGS := -T ../../../Userland/Userland.ld -e _start -nostdlib --build-id=none

RESOURCE_DIR := Resource

all: $(ELF) wad copy_resources

# Doom shareware IWAD（自由再配布可・リポジトリ非コミット）。初回のみネットワーク。
# 取得済みなら no-op。オフライン等で失敗しても Doom 以外のビルドは止めない。
wad:
	@sh $(RESOURCE_DIR)/fetch-wad.sh || \
		echo "WARN: doom1.wad not fetched; Doom will exit with 'W_InitFiles: no files found'"

copy_resources: wad
	@if [ -d $(RESOURCE_DIR) ]; then \
		mkdir -p $(BUILD_DIR); \
		cp -a $(RESOURCE_DIR) $(BUILD_DIR)/; \
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
