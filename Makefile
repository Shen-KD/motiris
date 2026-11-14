# motiris - Iris as a mote. Zero-dependency C11 build.
#
# Component layout (each compiles into a static archive, then linked):
#   src/core/    agent loop, config, transports      -> libmotiris_core.a
#   src/tools/   built-in tool bundles               -> libmotiris_tools.a
#   src/harness/ repl, gateway, cron, plugins, mcp   -> libmotiris_harness.a
#   src/vendor/  vendored cJSON (MIT)                - linked directly
#   deps/        linenoise (line editing), libcurl headers
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
.DEFAULT_GOAL := motiris
# include paths live in CPPFLAGS so overriding CFLAGS (e.g. -Werror in CI)
# never drops the vendored headers
CPPFLAGS ?= -Iinclude -Isrc/vendor -Ideps/libcurl/include -Ideps/linenoise
# build metadata for --version (survives CFLAGS overrides in CI)
MOTIRIS_SHA := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
MOTIRIS_DT  := $(shell date -u +%Y%m%d)
MOTIRIS_VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null || echo 0.1.0)
CPPFLAGS += -DMOTIRIS_SHA=\"$(MOTIRIS_SHA)\" -DMOTIRIS_DT=\"$(MOTIRIS_DT)\" \
            -DMOTIRIS_VERSION=\"$(MOTIRIS_VERSION)\"
LDFLAGS ?= -Wl,-l:libcurl.so.4
PREFIX  ?= /usr/local

FILE_TOOLS ?= 1
CORE_SRC    = $(wildcard src/core/*.c)
TOOLS_ALL   = $(wildcard src/tools/*.c)
ifeq ($(FILE_TOOLS),0)
TOOLS_SRC   = $(filter-out src/tools/filetools.c,$(TOOLS_ALL))
else
TOOLS_SRC   = $(filter-out src/tools/filetools_nofs.c,$(TOOLS_ALL))
endif
HARNESS_SRC = $(wildcard src/harness/*.c)
CORE_OBJ    = $(CORE_SRC:.c=.o)
TOOLS_OBJ   = $(TOOLS_SRC:.c=.o)
HARNESS_OBJ = $(HARNESS_SRC:.c=.o)
HDR = include/motiris.h

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

libmotiris_core.a: $(CORE_OBJ)
	rm -f $@ && ar rcs $@ $^

libmotiris_tools.a: $(TOOLS_OBJ)
	rm -f $@ && ar rcs $@ $^

libmotiris_harness.a: $(HARNESS_OBJ)
	rm -f $@ && ar rcs $@ $^

motiris: libmotiris_core.a libmotiris_tools.a libmotiris_harness.a \
		src/main.o src/compat.o src/vendor/cJSON.o deps/linenoise/linenoise.o
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/main.o src/compat.o src/vendor/cJSON.o \
		deps/linenoise/linenoise.o \
		-Wl,--start-group libmotiris_core.a libmotiris_tools.a \
		libmotiris_harness.a -Wl,--end-group \
		$(LDFLAGS) -ldl -Wl,--export-dynamic
	-strip $@

examples/hello_plugin.so: examples/hello_plugin.c include/motiris.h
	$(CC) $(CFLAGS) -fPIC -shared -Iinclude -Isrc/vendor -o $@ $<

test: motiris
	sh tests/smoke.sh

# Core-only portability probe: strict C11, no POSIX/file I/O/libcurl.
# The small test adapter supplies only tool/schema registries and invokes echo.
NOFS_DIR      = build/nofs
NOFS_CFLAGS   ?= -O2 -std=c11 -Wall -Wextra -Werror
NOFS_CPPFLAGS = -Iinclude -Isrc/vendor -DMOTIRIS_NO_FILESYSTEM
NOFS_CORE_OBJ = $(patsubst src/core/%.c,$(NOFS_DIR)/%.o,$(CORE_SRC))

$(NOFS_DIR)/%.o: src/core/%.c $(HDR)
	@mkdir -p $(NOFS_DIR)
	$(CC) $(NOFS_CFLAGS) $(NOFS_CPPFLAGS) -c $< -o $@

$(NOFS_DIR)/cJSON.o: src/vendor/cJSON.c src/vendor/cJSON.h
	@mkdir -p $(NOFS_DIR)
	$(CC) $(NOFS_CFLAGS) $(NOFS_CPPFLAGS) -c $< -o $@

$(NOFS_DIR)/core_nofs.o: tests/core_nofs.c $(HDR)
	@mkdir -p $(NOFS_DIR)
	$(CC) $(NOFS_CFLAGS) $(NOFS_CPPFLAGS) -c $< -o $@

$(NOFS_DIR)/core-nofs-test: $(NOFS_CORE_OBJ) $(NOFS_DIR)/cJSON.o \
		$(NOFS_DIR)/core_nofs.o
	$(CC) $(NOFS_CFLAGS) -o $@ $^

test-core-nofs: $(NOFS_DIR)/core-nofs-test
	$<

# Full host CLI with the POSIX file backend replaced by bounded stubs.
test-nofs: test-core-nofs
	$(MAKE) clean
	$(MAKE) FILE_TOOLS=0 motiris
	sh tests/nofs.sh

# run the full smoke suite inside a throwaway container: isolated HOME,
# ports and processes (no orphan mocks on the host)
test-docker:
	docker build -q -t motiris-test -f Dockerfile.test .
	docker run --rm -u $(shell id -u):$(shell id -g) \
		-v $(CURDIR):/app -w /app -e HOME=/tmp/home \
		motiris-test bash -c 'mkdir -p /tmp/home && make clean && make && sh tests/smoke.sh'

clean:
	rm -f motiris *.a
	rm -rf build/nofs
	find src deps -name '*.o' -delete

install: motiris
	install -m 0755 motiris $(PREFIX)/bin/motiris

.PHONY: test test-core-nofs test-nofs test-docker clean install
