.PHONY: all clean copy_resources test

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

# Host-compiled unit tests. They build against the host libc with the shell's
# file and display syscalls stubbed out, so they run here rather than needing
# a QEMU boot; `make test` is the fast check before one.
TEST_DIR   := $(BUILD_DIR)/Tests
TEST_CFLAGS := -std=c11 -D_GNU_SOURCE -I. -I../../API/Source -I../../Source \
               -Wall -Wextra -g

test:
	@mkdir -p $(TEST_DIR)
	$(HOST_CC) $(TEST_CFLAGS) -o $(TEST_DIR)/WM_CoreTest \
		Tests/WM_CoreTest.c Compositor/WM_Damage.c Compositor/WM_Raster.c \
		Core/WM_Display.c Core/WM_EventQueue.c ../../API/Source/Material.c
	$(HOST_CC) $(TEST_CFLAGS) -o $(TEST_DIR)/WM_DesktopEntryTest \
		Tests/WM_DesktopEntryTest.c Linux/WM_DesktopEntry.c
	$(HOST_CC) $(TEST_CFLAGS) -o $(TEST_DIR)/MaterialTest \
		Tests/MaterialTest.c ../../API/Source/Material.c
	@$(TEST_DIR)/WM_CoreTest
	@$(TEST_DIR)/WM_DesktopEntryTest
	@$(TEST_DIR)/MaterialTest

clean:
	rm -rf $(BUILD_DIR)

-include $(APP_OBJS:.o=.d) $(COMMON_DEPS)
