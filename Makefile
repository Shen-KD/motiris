# hermote - Hermes as a mote. Zero-dependency C11 build.
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pedantic -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
PREFIX  ?= /usr/local

SRC = src/vendor/cJSON.c src/transport.c src/tools.c src/plugin.c src/agent.c src/main.c
HDR = include/hermote.h

hermote: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Iinclude -Isrc/vendor -o $@ $(SRC) $(LDFLAGS) -ldl -Wl,--export-dynamic
	-strip $@

examples/hello_plugin.so: examples/hello_plugin.c include/hermote.h
	$(CC) $(CFLAGS) -fPIC -shared -Iinclude -Isrc/vendor -o $@ $<

test: hermote
	sh tests/smoke.sh

clean:
	rm -f hermote

install: hermote
	install -m 0755 hermote $(PREFIX)/bin/hermote

.PHONY: test clean install