# motiris - Iris as a mote. Zero-dependency C11 build.
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
# include paths live in CPPFLAGS so overriding CFLAGS (e.g. -Werror in CI)
# never drops the vendored headers
CPPFLAGS ?= -Iinclude -Isrc/vendor -Ideps/libcurl/include -Ideps/linenoise
LDFLAGS ?= -Wl,-l:libcurl.so.4
PREFIX  ?= /usr/local

SRC = src/vendor/cJSON.c src/transport.c src/tools.c src/plugin.c \
      src/agent.c src/repl.c src/config.c src/gateway.c src/filetools.c \
      src/webtools.c src/memorytools.c src/skilltools.c src/subagent.c \
      src/cron.c src/mcp.c src/browser.c src/main.c deps/linenoise/linenoise.c
HDR = include/motiris.h

motiris: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(LDFLAGS) -ldl -Wl,--export-dynamic
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
	rm -f motiris

install: motiris
	install -m 0755 motiris $(PREFIX)/bin/motiris

.PHONY: test test-docker clean install