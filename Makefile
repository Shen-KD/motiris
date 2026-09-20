# motiris - Iris as a mote. Zero-dependency C11 build.
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pedantic -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
PREFIX  ?= /usr/local

SRC = src/vendor/cJSON.c src/transport.c src/tools.c src/plugin.c src/agent.c src/main.c
HDR = include/motiris.h

motiris: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -Iinclude -Isrc/vendor -o $@ $(SRC) $(LDFLAGS) -ldl -Wl,--export-dynamic
	-strip $@

examples/hello_plugin.so: examples/hello_plugin.c include/motiris.h
	$(CC) $(CFLAGS) -fPIC -shared -Iinclude -Isrc/vendor -o $@ $<

test: motiris
	sh tests/smoke.sh

clean:
	rm -f motiris

install: motiris
	install -m 0755 motiris $(PREFIX)/bin/motiris

.PHONY: test clean install