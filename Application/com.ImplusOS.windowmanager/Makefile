.PHONY: all clean copy_resources

APP_NAME := $(notdir $(patsubst %/,%,$(CURDIR)))
SRC_DIR  := .
TOP_BUILD_DIR ?= ../../../Build/$(ARCH)
BUILD_DIR := $(TOP_BUILD_DIR)/Userland/$(APP_NAME)
ELF      := $(BUILD_DIR)/$(APP_NAME).ELF

APP_SRC := $(shell find $(SRC_DIR) -name '*.c' ! -path './Tests/*')
APP_OBJS := $(APP_SRC:%.c=$(BUILD_DIR)/%.o)

include ../../Source/AppCommon.mk

OBJS := $(APP_OBJS) $(COMMON_OBJS)
OPT_FLAGS ?= -O2 -fomit-frame-pointer -fno-strict-aliasing -DNDEBUG

CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-builtin \
          $(ARCH_CFLAGS) -nostdlib -nostartfiles -nodefaultlibs \
          -I. \
          -I../../../libc/I_libc/Source/include -I../../../libc/I_libc/Source/include/sys \
		  -I../../Source \
		  -I../../API/Source \
		  -I../../../Vendor/Library/freetype/include \
		  -Wall -Wextra -Wtype-limits -Wconversion -Wsign-conversion -Wshadow -MMD -MP \
          $(OPT_FLAGS) -ffunction-sections -fdata-sections
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS := -T ../../Source/Userland.ld -e _start -nostdlib --build-id=none --gc-sections -L$(TOP_BUILD_DIR)/Vendor/Library
LDLIBS := -lfreetype

RESOURCE_DIR := Resource
HOST_CC ?= cc

all: $(ELF) copy_resources

copy_resources:
	@if [ -d $(RESOURCE_DIR) ]; then \
		mkdir -p $(BUILD_DIR); \
		cp -a $(RESOURCE_DIR) $(BUILD_DIR)/; \
	fi

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(ELF): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(APP_OBJS:.o=.d) $(COMMON_DEPS)
