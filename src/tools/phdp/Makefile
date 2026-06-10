#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# PHDP — Pocket-Host Debug Protocol tools
#
# Builds:
#   phdpd  — daemon (background, owns UART + protocol state)
#   phdp   — CLI client (thin, talks to daemon via Unix socket)
#
# Usage:
#   make           Build both tools
#   make install   Install to /usr/local/bin
#   make clean     Remove build artifacts

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=

PREFIX  ?= /usr/local

TARGETS = phdpd phdp

all: $(TARGETS)

phdpd: phdpd.c phdp_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

phdp: phdp.c phdp_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGETS)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 phdpd $(DESTDIR)$(PREFIX)/bin/
	install -m 755 phdp  $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f $(TARGETS)

.PHONY: all install clean
