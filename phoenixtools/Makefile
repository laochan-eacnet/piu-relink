CROSS ?= i686-linux-gnu-
CC := $(CROSS)gcc
PYTHON ?= python3
QEMU ?= qemu-i386
QEMU_LD_PREFIX ?= /usr/i686-linux-gnu

BIN := bin
OBJ := $(BIN)/obj
TEST_BIN := $(BIN)/tests
LIB_BIN := $(BIN)/lib

CPPFLAGS := -Isrc
CFLAGS ?= -O2 -g -std=gnu11 -Wall -Wextra -Werror -fPIC \
          -fno-stack-protector -mpreferred-stack-boundary=2 \
          -mincoming-stack-boundary=2
DEPFLAGS := -MMD -MP
LDFLAGS_SO := -shared -Wl,-z,relro -Wl,-z,now

HOOK_EXCLUDED_SOURCES := src/compat/legacy_curl.c
SOURCES := $(filter-out $(HOOK_EXCLUDED_SOURCES),$(shell find src -name '*.c' -print | sort))
OBJECTS := $(patsubst %.c,$(OBJ)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

HOOK := $(BIN)/phoenixtools.so
TESTS := $(TEST_BIN)/resolver_test $(TEST_BIN)/lxio_test \
         $(TEST_BIN)/libusb_forward_test $(TEST_BIN)/network_test \
         $(TEST_BIN)/network_policy_test $(TEST_BIN)/legacy_curl_test \
         $(TEST_BIN)/disk_identity_test $(TEST_BIN)/card_input_test
FIXTURE := $(TEST_BIN)/libusb_forward_fixture.so
HARDWARE_FIXTURE := $(TEST_BIN)/fixtures/hardware
DISK_FIXTURE := $(TEST_BIN)/fixtures/disk
CONFIG_FIXTURE_HOOKS := $(HARDWARE_FIXTURE)/phoenixtools.so \
                        $(DISK_FIXTURE)/phoenixtools.so
CURL_STUB_SOURCE := src/compat/legacy_curl.c
CURL_STUB := $(LIB_BIN)/network/libcurl.so.4
PLAIN_ROOT ?= /home/geeki/piuxx/phoenix-2024/data
CONFIG_SOURCES := src/config/config.c src/config/config_apply.c \
                  src/config/config_values.c
CONFIG_HEADERS := src/config/config.h src/config/config_internal.h
CARD_TEST_SOURCES := tests/card_input_test.c tests/card_file_test.c \
                     tests/card_input_fixture.c tests/card_input_x11_fixture.c \
                     tests/card_input_native_fixture.c \
                     $(CONFIG_SOURCES) src/input/bindings.c \
                     src/input/x11_input.c src/safety/network_card.c \
                     src/phoenix/patch_engine.c
CARD_TEST_HEADERS := tests/card_input_fixture.h $(CONFIG_HEADERS) \
                     src/core/runtime.h \
                     src/core/limits.h src/core/log.h src/input/bindings.h \
                     src/input/input.h src/input/lxio.h \
                     src/safety/network_card.h src/phoenix/patch_engine.h

.PHONY: all audit binary-audit clean test

all: audit binary-audit $(TESTS) $(FIXTURE) $(CURL_STUB)

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(HOOK): $(OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS_SO) -o $@ $(OBJECTS) -ldl -pthread

binary-audit: $(HOOK)
	@if $(CROSS)nm -D --defined-only $(HOOK) | awk '{print $$3}' | \
	  grep -Eq '^(av(codec|format|util|io|_|frame|packet|picture)|sws_)'; then \
	  echo 'error: phoenixtools.so must not interpose FFmpeg' >&2; exit 1; \
	fi

$(TEST_BIN)/%: tests/%.c
	@mkdir -p $(@D)
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra -Werror -o $@ $< -ldl

$(TEST_BIN)/card_input_test: $(CARD_TEST_SOURCES) $(CARD_TEST_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIE -pie -Ddlsym=card_fixture_dlsym \
	  -o $@ $(CARD_TEST_SOURCES) -Wl,--wrap=open -Wl,--wrap=fopen \
	  -pthread -ldl

$(TEST_BIN)/network_policy_test: tests/network_policy_test.c \
                               src/safety/network.c src/safety/network_policy.c \
                               $(CONFIG_SOURCES) $(CONFIG_HEADERS) \
                               src/safety/private_http.h src/safety/safety.h \
                               src/core/runtime.h src/core/limits.h src/core/log.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -O2 -g -std=gnu11 -Wall -Wextra -Werror \
	  -Ddlsym=network_test_dlsym -o $@ tests/network_policy_test.c \
	  src/safety/network.c src/safety/network_policy.c $(CONFIG_SOURCES) -ldl

$(TEST_BIN)/legacy_curl_test: tests/legacy_curl_test.c $(CURL_STUB_SOURCE) \
                            src/safety/private_http.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -O2 -g -std=gnu11 -Wall -Wextra -Werror \
	  -Dconnect=legacy_test_connect -o $@ tests/legacy_curl_test.c $(CURL_STUB_SOURCE)

$(FIXTURE): tests/libusb_forward_fixture.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -shared -o $@ $<

$(TEST_BIN)/libusb_forward_test: tests/libusb_forward_test.c $(FIXTURE)
	@mkdir -p $(@D)
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra -Werror -o $@ $< \
	  -L$(TEST_BIN) -Wl,--no-as-needed -l:libusb_forward_fixture.so \
	  -Wl,--as-needed -Wl,-rpath,'$$ORIGIN' -ldl

$(CURL_STUB): $(CURL_STUB_SOURCE) src/maps/curl.map src/safety/private_http.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -Wl,-soname,libcurl.so.4 \
	  -Wl,--version-script=src/maps/curl.map -o $@ $<

$(CONFIG_FIXTURE_HOOKS): $(HOOK)
	@mkdir -p $(@D)
	cp $< $@

$(HARDWARE_FIXTURE)/phoenixtools.ini:
	@mkdir -p $(@D)
	@printf '[input]\nvirtual_lxio=false\n' > $@

$(DISK_FIXTURE)/phoenixtools.ini:
	@mkdir -p $(@D)
	@rm -rf $(@D)/runtime
	@printf '[input]\nvirtual_lxio=false\n[paths]\nruntime_dir=runtime\nvirtual_disk=runtime/virtual-sda.img\n' > $@

audit:
	$(PYTHON) tools/source_audit.py

test: all $(CONFIG_FIXTURE_HOOKS) $(HARDWARE_FIXTURE)/phoenixtools.ini \
          $(DISK_FIXTURE)/phoenixtools.ini
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" $(QEMU) \
	  $(TEST_BIN)/resolver_test ./$(HOOK) "$(PLAIN_ROOT)" \
	  config/sfx/photo.txt 264
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" $(QEMU) \
	  $(TEST_BIN)/lxio_test ./$(HOOK)
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" \
	QEMU_SET_ENV="LD_PRELOAD=$(CURDIR)/$(HARDWARE_FIXTURE)/phoenixtools.so" \
	  $(QEMU) $(TEST_BIN)/libusb_forward_test
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" \
	QEMU_SET_ENV="LD_PRELOAD=$(CURDIR)/$(HOOK):$(CURDIR)/$(CURL_STUB)" \
	  $(QEMU) $(TEST_BIN)/network_test ./$(HOOK) ./$(CURL_STUB)
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" $(QEMU) $(TEST_BIN)/network_policy_test
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" $(QEMU) $(TEST_BIN)/legacy_curl_test
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" $(QEMU) $(TEST_BIN)/card_input_test
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" \
	QEMU_SET_ENV="LD_PRELOAD=$(CURDIR)/$(DISK_FIXTURE)/phoenixtools.so" \
	  $(QEMU) $(TEST_BIN)/disk_identity_test

clean:
	rm -rf $(BIN)

-include $(DEPS)
