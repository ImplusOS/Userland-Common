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

# Resource/ is the unmodified upstream Linux Firefox release (x86_64; on
# arm64 it runs under the translator): not built here, only staged. Not
# committed to git (see .gitignore). `make fetch` downloads the pinned
# release below from Mozilla, checks its sha256 and unpacks it into
# Resource/ -- the only step that needs the network; `all` never fetches.
FIREFOX_VERSION ?= 160.0.1
FIREFOX_LOCALE  ?= ja
FIREFOX_SHA256  ?= 23ada07d9e9b3d7f1b37915814b2368aaaf1d9f5a9bd5a5c2c3ba51f1cc821d7
FIREFOX_TARBALL := firefox-$(FIREFOX_VERSION).tar.xz
FIREFOX_URL     := https://download-installer.cdn.mozilla.net/pub/firefox/releases/$(FIREFOX_VERSION)/linux-x86_64/$(FIREFOX_LOCALE)/$(FIREFOX_TARBALL)

all: $(ELF) copy_resources

.PHONY: fetch
fetch:
	@set -e; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	echo "fetch: $(FIREFOX_URL)"; \
	curl -fL --retry 3 -o "$$tmp/$(FIREFOX_TARBALL)" "$(FIREFOX_URL)"; \
	got=$$(shasum -a 256 "$$tmp/$(FIREFOX_TARBALL)" 2>/dev/null || sha256sum "$$tmp/$(FIREFOX_TARBALL)"); \
	got=$${got%% *}; \
	if [ "$$got" != "$(FIREFOX_SHA256)" ]; then \
		echo "fetch: sha256 mismatch: $$got" >&2; exit 1; \
	fi; \
	mkdir -p "$$tmp/x"; tar -xJf "$$tmp/$(FIREFOX_TARBALL)" -C "$$tmp/x"; \
	rm -rf $(RESOURCE_DIR); mv "$$tmp/x/firefox" $(RESOURCE_DIR); \
	echo "fetch: Firefox $(FIREFOX_VERSION) ($(FIREFOX_LOCALE)) -> $(RESOURCE_DIR)/"

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
