#!/usr/bin/env -S gmake all
# vim: set noexpandtab:

UNAME_S := $(shell uname -s)

# PACKAGE_BUILD=1 skips embedding the .source ELF section (distribution packages).
ifneq ($(PACKAGE_BUILD),)
export PACKAGE_BUILD
endif

# Parallel compilation: enabled by default; override with `make -j1` when debugging.
NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
	MAKEFLAGS += -j$(NPROC)
endif

# Default toolchain: Clang/Clang++ (C++23). Matches OpenBSD/FreeBSD CI behaviour and
# catches Clang-only warnings (e.g. -Wunused-lambda-capture) under -Werror.
# GNU make predefines CXX=g++ — `?=` does not override that, so assign explicitly
# unless the user passed CC/CXX on the command line (make CC=... CXX=...).
ifeq ($(UNAME_S),Darwin)
HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
export PKG_CONFIG_PATH := $(HOMEBREW_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
# bison, flex, and llvm are keg-only — prepend their bin dirs so make recipes
# and $(shell) calls use the Homebrew versions, not the ancient macOS stubs.
export PATH := $(HOMEBREW_PREFIX)/opt/bison/bin:$(HOMEBREW_PREFIX)/opt/flex/bin:$(HOMEBREW_PREFIX)/opt/llvm/bin:$(PATH)
DEFAULT_CC := $(HOMEBREW_PREFIX)/opt/llvm/bin/clang
DEFAULT_CXX := $(HOMEBREW_PREFIX)/opt/llvm/bin/clang++
else
DEFAULT_CC := clang
DEFAULT_CXX := clang++
endif
ifneq ($(origin CC),command line)
CC := $(DEFAULT_CC)
endif
ifneq ($(origin CXX),command line)
CXX := $(DEFAULT_CXX)
endif
export CC
export CXX

IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -i clang)

BISON ?= bison
FLEX  ?= flex

# Build profiles (combine ASAN=1 with DEBUG=1 for instrumented dev builds):
#   default  — -O2 -DNDEBUG
#   DEBUG=1  — -O0 -DDEBUG (assertions on, fast rebuilds)
#   ASAN=1   — -fsanitize=address (Linux: links libasan)
ifneq ($(DEBUG),)
OPTFLAGS := -O0
CPPDEFINES := -DDEBUG
else
OPTFLAGS := -O2
CPPDEFINES := -DNDEBUG
endif

SANFLAGS :=
SANLDFLAGS :=
ifneq ($(ASAN),)
SANFLAGS := -fsanitize=address
ifeq ($(UNAME_S),Linux)
SANLDFLAGS := -lasan -lrt
endif
endif

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
else ifneq (,$(filter $(UNAME_S),FreeBSD OpenBSD NetBSD))
# BSDs with Clang 13+ support DWARF 4; use it for consistent debug info
DWARF_FLAG := -gdwarf-4
else
DWARF_FLAG := -gdwarf-4
endif

CFLAGS+=$(OPTFLAGS) $(CPPDEFINES) $(SANFLAGS) \
	-fno-omit-frame-pointer -fPIC \
	-fvisibility=hidden -fvisibility-inlines-hidden \
	-fdebug-prefix-map=.=$(CURDIR) \
	-std=c11 $(DWARF_FLAG) \
	-Wall -Wextra -pedantic -Werror\
	-Werror-implicit-function-declaration \
	-Wno-missing-field-initializers \
	$(INCLUDES)
ifeq ($(UNAME_S),Linux)
CFLAGS+=-D_XOPEN_SOURCE=700
endif
CPPFLAGS+=$(OPTFLAGS) $(CPPDEFINES) $(SANFLAGS) \
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
	 #-fuse-ld=mold
LDFLAGS+=$(SANLDFLAGS) $(SANFLAGS) \
	 -std=c++23 $(DWARF_FLAG)
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
	 src/data/capability.hh \
	 src/xmpp/stanza.hh \
	 src/xmpp/ns.hh \
	 src/xmpp/node.hh \
	 src/ui/picker.hh \

SRCS=src/plugin.cpp \
	 src/account.cpp \
	 src/account_omemo_muc.cpp \
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
	 src/connection/pep_handler.cpp \
	 src/connection/iq_ping_handler.cpp \
	 src/connection/iq_avatar_handler.cpp \
	 src/connection/iq_bob_handler.cpp \
	 src/connection/iq_pubsub_feed_handler.cpp \
	 src/connection/iq_omemo_pubsub_handler.cpp \
	 src/connection/iq_upload_handler.cpp \
	 src/connection/iq_mam_handler.cpp \
	 src/connection/iq_disco_handler.cpp \
	 src/connection/iq_vcard_handler.cpp \
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
	 src/command/links.cpp \
	 src/input.cpp \
	 src/message.cpp \
	 src/omemo/api.cpp \
	 src/pgp.cpp \
	 src/user.cpp \
	 src/nicklist.cpp \
	 src/util.cpp \
	 src/weechat/ui_port.cpp \
	 src/weechat/icat_preview.cpp \
	 src/weechat/runtime_port.cpp \
	 src/weechat/render_event.cpp \
	 src/weechat/buffer_port.cpp \
	 src/config/breadcrumb.cpp \
	 src/config/file.cpp \
	 src/config/section.cpp \
	 src/config/account.cpp \
	 src/config/option.cpp \
	 src/data/capability.cpp \
	 src/xmpp/presence.cpp \
	 src/xmpp/iq.cpp \
	 src/xmpp/node.cpp \
	 src/xmpp/stanza_view.cpp \
	 src/xmpp/iq_handlers.cpp \
	 src/xmpp/iq_error.cpp \
	 src/xmpp/data_form.cpp \
	 src/xmpp/iq_ping.cpp \
	 src/xmpp/iq_pubsub_feed.cpp \
	 src/xmpp/iq_omemo_pubsub.cpp \
	 src/xmpp/iq_upload.cpp \
	 src/xmpp/iq_mam.cpp \
	 src/xmpp/iq_disco.cpp \
	 src/xmpp/iq_caps.cpp \
	 src/xmpp/iq_vcard.cpp \
	 src/xmpp/iq_bookmarks.cpp \
	 src/xmpp/message_ack.cpp \
	 src/xmpp/chat_state.cpp \
	 src/xmpp/message_forward.cpp \
	 src/xmpp/message_body.cpp \
	 src/xmpp/message_media.cpp \
	 src/xmpp/message_sticker_emoji.cpp \
	 src/xmpp/message_bob.cpp \
	 src/xmpp/message_omemo.cpp \
	 src/xmpp/message_invite.cpp \
	 src/xmpp/message_ephemeral.cpp \
	 src/xmpp/message_spoiler.cpp \
	 src/xmpp/message_fallback.cpp \
	 src/xmpp/message_line_tag.cpp \
	 src/xmpp/message_correct.cpp \
	 src/xmpp/message_retract.cpp \
	 src/xmpp/message_reactions.cpp \
	 src/xmpp/message_reply.cpp \
	 src/xmpp/message_pep.cpp \
	 src/xmpp/message_pep_feed.cpp \
	 src/weechat/line_store.cpp \
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
	+$(MAKE) weechat-xmpp
ifneq ($(DEBUG),)
	+$(MAKE) test
else
	@echo ">>> Skipping doctests (DEBUG=1 or make test)."
endif

.PHONY: weechat-xmpp
weechat-xmpp: $(DEPS) xmpp.so

xmpp.so: $(DEPS) $(OBJS) $(HDRS)
	$(CXX) $(SHARED_FLAG) $(LDFLAGS) -o $@ $(AS_NEEDED) $(OBJS) $(DEPS) $(LDLIBS)
ifeq ($(UNAME_S),Linux)
ifneq ($(OBJCOPY),)
ifneq ($(PACKAGE_BUILD),1)
	@files=$$(git ls-files 2>/dev/null); \
	if [ -n "$$files" ]; then \
		echo "$$files" | xargs ls -d 2>/dev/null | xargs tar cz | $(OBJCOPY) --add-section .source=/dev/stdin xmpp.so; \
	fi
endif
endif
endif

sexp/sexp.a: sexp/parser.o sexp/lexer.o sexp/driver.o
	ar -r $@ $^

sexp/parser.o: sexp/parser.yy
	cd sexp && $(BISON) -t -d -v parser.yy
ifneq ($(IS_CLANG),)
	$(CXX) $(CPPFLAGS) -fvisibility=default -Wno-unused-variable -Wno-unused-but-set-variable -c sexp/parser.tab.cc -o $@
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
	cd deps/diff && env -u MAKEFLAGS CC="$(CC)" ./configure
	$(MAKE) -C deps/diff CC="$(CC)" CFLAGS=-fPIC
diff: deps/diff/libdiff.a
