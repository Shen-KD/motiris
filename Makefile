# mote - a mote of an agent framework. Zero-dependency C11 build.
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pedantic -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
PREFIX  ?= /usr/local

SRC = src/vendor/cJSON.c src/transport.c src/tools.c src/plugin.c src/agent.c src/main.c
HDR = include/mote.h

mote: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Iinclude -Isrc/vendor -o $@ $(SRC) $(LDFLAGS) -ldl -Wl,--export-dynamic
	-strip $@

examples/hello_plugin.so: examples/hello_plugin.c include/mote.h
	$(CC) $(CFLAGS) -fPIC -shared -Iinclude -Isrc/vendor -o $@ $<

test: mote
	sh tests/smoke.sh

clean:
	rm -f mote

install: mote
	install -m 0755 mote $(PREFIX)/bin/mote

.PHONY: test clean install