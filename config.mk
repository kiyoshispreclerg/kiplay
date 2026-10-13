# Kiplay build configuration. Override any variable on the command line,
# e.g. `make CC=clang PREFIX=/usr`.

VERSION = 0.0.1

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
LOCALEDIR ?= $(DATADIR)/locale
DESTDIR ?=

PKG_CONFIG ?= pkg-config

# C11 without GNU extensions; XSI (implies POSIX.1-2008).
STDFLAGS  = -std=c11 -D_XOPEN_SOURCE=700
WARNFLAGS = -Wall -Wextra -Werror -Wpedantic -Wshadow -Wformat=2 \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla

OPTFLAGS ?= -O2 -g

# libkp (src/) is what the unit tests link: no GTK in it.
# Library headers go in as -isystem so -Wpedantic/-Werror only judge our code.
PKGS       = glib-2.0 gio-2.0 gio-unix-2.0
PKG_CFLAGS = $(if $(PKGS),$(shell $(PKG_CONFIG) --cflags $(PKGS) | sed 's/-I/-isystem /g'))
PKG_LIBS   = $(if $(PKGS),$(shell $(PKG_CONFIG) --libs $(PKGS)))

# Phase 0 prototypes that need GTK.
GTK_PKGS   = gtk+-2.0 gio-unix-2.0 x11
GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(GTK_PKGS) | sed 's/-I/-isystem /g')
GTK_LIBS   = $(shell $(PKG_CONFIG) --libs $(GTK_PKGS))

CPPFLAGS += -Isrc -Ivendor/cjson -DKP_VERSION='"$(VERSION)"'
LDLIBS   += $(PKG_LIBS) -lm

# Third-party code: our pedantic flags don't judge it.
VENDOR_WARNFLAGS = -Wall -Wextra -Werror

# `make asan` sets SANITIZE=1.
SANITIZE ?= 0
ifeq ($(SANITIZE),1)
OPTFLAGS = -O1 -g -fno-omit-frame-pointer
SANFLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all
endif
