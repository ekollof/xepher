#!/usr/bin/env -S gmake all
# vim: set noexpandtab:

UNAME_S := $(shell uname -s)
IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -i clang)

ifeq ($(UNAME_S),Darwin)
HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
export PKG_CONFIG_PATH := $(HOMEBREW_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
# bison, flex, and llvm are keg-only — prepend their bin dirs so make recipes
# and $(shell) calls use the Homebrew versions, not the ancient macOS stubs.
export PATH := $(HOMEBREW_PREFIX)/opt/bison/bin:$(HOMEBREW_PREFIX)/opt/flex/bin:$(HOMEBREW_PREFIX)/opt/llvm/bin:$(PATH)
# Use Homebrew clang/clang++ by default on macOS (overridable: CC=... make)
CC  ?= $(HOMEBREW_PREFIX)/opt/llvm/bin/clang
CXX ?= $(HOMEBREW_PREFIX)/opt/llvm/bin/clang++
endif

BISON ?= bison
FLEX  ?= flex

ifdef DEBUG
	DBGCFLAGS=-DDEBUG -fno-omit-frame-pointer -fsanitize=address #-fsanitize=undefined -fsanitize=leak
ifeq ($(UNAME_S),Linux)
	DBGLDFLAGS=-lasan -lrt -lasan #-lubsan -llsan
endif
endif

CC ?= cc
CXX ?= c++
SHELL = /bin/sh
RM ?= rm -f
FIND ?= find

INCLUDES=-Ilibstrophe -Ideps/lmdbxx -Ideps -Isrc -I. \
	 $(shell pkg-config --cflags libstrophe) \
	 $(shell xml2-config --cflags) \
	 $(shell pkg-config --cflags gpgme) \
	 $(shell pkg-config --cflags libsignal-protocol-c) \
	 $(shell pkg-config --cflags libomemo-c)
ifeq ($(UNAME_S),Darwin)
DWARF_FLAG := -g
else
DWARF_FLAG := -gdwarf-4
endif

CFLAGS+=$(DBGCFLAGS) \
	-fno-omit-frame-pointer -fPIC \
	-fvisibility=hidden -fvisibility-inlines-hidden \
	-fdebug-prefix-map=.=$(CURDIR) \
	-std=gnu99 $(DWARF_FLAG) \
	-Wall -Wextra -pedantic -Werror\
	-Werror-implicit-function-declaration \
	-Wno-missing-field-initializers \
	$(INCLUDES)
ifeq ($(UNAME_S),Linux)
CFLAGS+=-D_XOPEN_SOURCE=700
endif
ifeq ($(CC),clang)
	CFLAGS+=
else
	CFLAGS+=
endif
CPPFLAGS+=$(DBGCFLAGS) \
	  -fno-omit-frame-pointer -fPIC \
	  -fvisibility=hidden -fvisibility-inlines-hidden \
	  -std=c++23 $(DWARF_FLAG) \
	  -Wall -Wextra -pedantic -Werror \
	  -Wno-missing-field-initializers \
	  -Wno-variadic-macros \
	  $(INCLUDES)
# -DDOCTEST_CONFIG_DISABLE
ifneq ($(IS_CLANG),)
	CPPFLAGS+=-Wno-gnu-zero-variadic-macro-arguments
endif
ifeq ($(CXX),clang)
	CPPFLAGS+=
else
	CPPFLAGS+=
endif
	 #-fuse-ld=mold
LDFLAGS+=$(DBGLDFLAGS) \
	 -std=c++23 $(DWARF_FLAG) \
	 $(DBGCFLAGS)
LDLIBS=$(shell pkg-config --libs libstrophe) \
	   -lpthread \
	   $(shell pkg-config --libs libcurl) \
	   $(shell pkg-config --libs openssl) \
	   $(shell xml2-config --libs) \
	   $(shell pkg-config --libs gpgme) \
	   $(shell pkg-config --libs libomemo-c) \
	   -lgcrypt \
	   -llmdb -lfmt

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

OBJCOPY ?= $(shell command -v objcopy 2>/dev/null || command -v llvm-objcopy 2>/dev/null || true)

ifeq ($(UNAME_S),Linux)
AS_NEEDED := -Wl,--as-needed
else
AS_NEEDED :=
endif

ifeq ($(UNAME_S),Darwin)
SHARED_FLAG := -dynamiclib
else
SHARED_FLAG := -shared
endif

HDRS=src/plugin.hh \
	 src/account.hh \
	 src/avatar.hh \
	 src/buffer.hh \
	 src/channel.hh \
	 src/color.hh \
	 src/command.hh \
	 src/completion.hh \
	 src/config.hh \
	 src/connection.hh \
	 src/input.hh \
	 src/message.hh \
	 src/omemo.hh \
	 src/pgp.hh \
	 src/user.hh \
	 src/util.hh \
	 src/config/breadcrumb.hh \
	 src/config/file.hh \
	 src/config/section.hh \
	 src/config/account.hh \
	 src/config/option.hh \
	 src/data/omemo.hh \
	 src/data/capability.hh \
	 src/xmpp/stanza.hh \
	 src/xmpp/ns.hh \
	 src/xmpp/node.hh \
	 src/ui/picker.hh \

SRCS=src/plugin.cpp \
	 src/account.cpp \
	 src/avatar.cpp \
	 src/buffer.cpp \
	 src/color.cpp \
	 src/channel.cpp \
	 src/command.cpp \
	 src/completion.cpp \
	 src/config.cpp \
	 src/connection.cpp \
	 src/connection/helpers.cpp \
	 src/connection/presence_handler.cpp \
	 src/connection/message_handler.cpp \
	 src/connection/iq_handler.cpp \
	 src/connection/session_lifecycle.cpp \
	 src/account/callbacks.cpp \
	 src/account/lmdb_cache.cpp \
	 src/command/account.cpp \
	 src/command/channel.cpp \
	 src/command/messaging.cpp \
	 src/command/ephemeral.cpp \
	 src/command/notify.cpp \
	 src/command/archive.cpp \
	 src/command/encryption.cpp \
	 src/command/history.cpp \
	 src/command/presence.cpp \
	 src/command/roster.cpp \
	 src/command/rooms.cpp \
	 src/command/muc_admin.cpp \
	 src/input.cpp \
	 src/message.cpp \
	 src/omemo/api.cpp \
	 src/pgp.cpp \
	 src/user.cpp \
	 src/util.cpp \
	 src/config/breadcrumb.cpp \
	 src/config/file.cpp \
	 src/config/section.cpp \
	 src/config/account.cpp \
	 src/config/option.cpp \
	 src/data/omemo.cpp \
	 src/data/capability.cpp \
	 src/xmpp/presence.cpp \
	 src/xmpp/iq.cpp \
	 src/xmpp/node.cpp \
	 src/xmpp/atom.cpp \
	 src/xmpp/xhtml.cpp \
	 src/xmpp/embed.cpp \
	 src/ui/picker.cpp \

DEPS=deps/diff/libdiff.a \
	 sexp/sexp.a \

OBJS=$(patsubst src/%.cpp,obj/%.o,$(patsubst src/%.c,obj/%.o,$(SRCS)))
COVS=$(patsubst src/%.cpp,obj/%.cov.o,$(SRCS))

SUFFIX=$(shell date +%s)

$(eval GIT_REF=$(shell git describe --abbrev=6 --always --dirty 2>/dev/null || true))

.DEFAULT_GOAL := all

include test.mk
include install.mk
include clean.mk
include depend.mk

.PHONY: all
all: depend
	$(MAKE) weechat-xmpp && $(MAKE) test

.PHONY: weechat-xmpp
weechat-xmpp: $(DEPS) xmpp.so

xmpp.so: $(DEPS) $(OBJS) $(HDRS)
	$(CXX) $(SHARED_FLAG) $(LDFLAGS) -o $@ $(AS_NEEDED) $(OBJS) $(DEPS) $(LDLIBS)
ifneq ($(OBJCOPY),)
	git ls-files | xargs ls -d | xargs tar cz | $(OBJCOPY) --add-section .source=/dev/stdin xmpp.so
endif

sexp/sexp.a: sexp/parser.o sexp/lexer.o sexp/driver.o
	ar -r $@ $^

sexp/parser.o: sexp/parser.yy
	cd sexp && $(BISON) -t -d -v parser.yy
ifneq ($(IS_CLANG),)
	$(CXX) $(CPPFLAGS) -fvisibility=default -Wno-unused-variable -c sexp/parser.tab.cc -o $@
else
	$(CXX) $(CPPFLAGS) -fvisibility=default -Wno-unused-but-set-variable -c sexp/parser.tab.cc -o $@
endif

sexp/lexer.o: sexp/lexer.l
	cd sexp && $(FLEX) -d --outfile=lexer.yy.cc lexer.l
	$(CXX) $(CPPFLAGS) -fvisibility=default -c sexp/lexer.yy.cc -o $@

sexp/driver.o: sexp/driver.cpp
	$(CXX) $(CPPFLAGS) -fvisibility=default -c $< -o $@

obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) -DGIT_COMMIT=$(GIT_REF) $(CFLAGS) -c $< -o $@

obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -DGIT_COMMIT=$(GIT_REF) $(CPPFLAGS) -c $< -o $@

obj/%.cov.o: src/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) --coverage $(CPPFLAGS) -c $< -o $@

.PHONY: diff
deps/diff/libdiff.a:
	git submodule update --init --recursive deps/diff
	echo "HAVE___PROGNAME=1" > deps/diff/configure.local
	cd deps/diff && env -u MAKEFLAGS ./configure
	$(MAKE) -C deps/diff CFLAGS=-fPIC
diff: deps/diff/libdiff.a
