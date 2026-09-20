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
LDFLAGS ?= -Wl,-l:libcurl.so.4
PREFIX  ?= /usr/local

CORE_SRC    = $(wildcard src/core/*.c)
TOOLS_SRC   = $(wildcard src/tools/*.c)
HARNESS_SRC = $(wildcard src/harness/*.c)
CORE_OBJ    = $(CORE_SRC:.c=.o)
TOOLS_OBJ   = $(TOOLS_SRC:.c=.o)
HARNESS_OBJ = $(HARNESS_SRC:.c=.o)
HDR = include/motiris.h

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

libmotiris_core.a: $(CORE_OBJ)
	ar rcs $@ $^

libmotiris_tools.a: $(TOOLS_OBJ)
	ar rcs $@ $^

libmotiris_harness.a: $(HARNESS_OBJ)
	ar rcs $@ $^

motiris: libmotiris_core.a libmotiris_tools.a libmotiris_harness.a \
		src/main.o src/vendor/cJSON.o deps/linenoise/linenoise.o
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/main.o src/vendor/cJSON.o \
		deps/linenoise/linenoise.o \
		-Wl,--start-group libmotiris_core.a libmotiris_tools.a \
		libmotiris_harness.a -Wl,--end-group \
		$(LDFLAGS) -ldl -Wl,--export-dynamic
	-strip $@

examples/hello_plugin.so: examples/hello_plugin.c include/motiris.h
	$(CC) $(CFLAGS) -fPIC -shared -Iinclude -Isrc/vendor -o $@ $<

test: motiris
	sh tests/smoke.sh

# run the full smoke suite inside a throwaway container: isolated HOME,
# ports and processes (no orphan mocks on the host)
test-docker:
	docker build -q -t motiris-test -f Dockerfile.test .
	docker run --rm -u $(shell id -u):$(shell id -g) \
		-v $(CURDIR):/app -w /app -e HOME=/tmp/home \
		motiris-test bash -c 'mkdir -p /tmp/home && make clean && make && sh tests/smoke.sh'

clean:
	rm -f motiris *.a
	find src deps -name '*.o' -delete

install: motiris
	install -m 0755 motiris $(PREFIX)/bin/motiris

.PHONY: test test-docker clean install