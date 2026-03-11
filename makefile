#!/usr/bin/env -S gmake all
# vim: set noexpandtab:

ifdef DEBUG
	DBGCFLAGS=-DDEBUG -fno-omit-frame-pointer -fsanitize=address #-fsanitize=undefined -fsanitize=leak
	DBGLDFLAGS=-lasan -lrt -lasan #-lubsan -llsan
endif

CC ?= gcc
CXX ?= g++
SHELL = bash
RM ?= rm -f
FIND ?= find

INCLUDES=-Ilibstrophe -Ideps/lmdbxx -Ideps -Isrc -I. -I/usr/include/omemo/ \
	 $(shell xml2-config --cflags) \
	 $(shell pkg-config --cflags gpgme) \
	 $(shell pkg-config --cflags libsignal-protocol-c)
CFLAGS+=$(DBGCFLAGS) \
	-fno-omit-frame-pointer -fPIC \
	-fvisibility=hidden -fvisibility-inlines-hidden \
	-fdebug-prefix-map=.=$(shell readlink -f .) \
	-std=gnu99 -gdwarf-4 \
	-Wall -Wextra -pedantic \
	-Werror-implicit-function-declaration \
	-Wno-missing-field-initializers \
	-D_XOPEN_SOURCE=700 \
	$(INCLUDES)
ifeq ($(CC),clang)
	CFLAGS+=
else
	CFLAGS+=
endif
CPPFLAGS+=$(DBGCFLAGS) \
	  -fno-omit-frame-pointer -fPIC \
	  -fvisibility=hidden -fvisibility-inlines-hidden \
	  -std=c++23 -gdwarf-4 \
	  -Wall -Wextra -pedantic \
	  -Wno-missing-field-initializers \
	  $(INCLUDES)
# -DDOCTEST_CONFIG_DISABLE
ifeq ($(CXX),clang)
	CPPFLAGS+=
else
	CPPFLAGS+=
endif
	 #-fuse-ld=mold
LDFLAGS+=$(DBGLDFLAGS) \
	 -std=c++23 -gdwarf-4 \
	 $(DBGCFLAGS)
LDLIBS=-lstrophe \
	   -lpthread \
	   -lcurl \
	   -lcrypto \
	   $(shell xml2-config --libs) \
	   $(shell pkg-config --libs gpgme) \
	   $(shell pkg-config --libs libsignal-protocol-c) \
	   -lgcrypt \
	   -llmdb -lfmt

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

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
	 src/input.cpp \
	 src/message.cpp \
	 src/omemo.cpp \
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
	$(CXX) -shared $(LDFLAGS) -o $@ -Wl,--as-needed $(OBJS) $(DEPS) $(LDLIBS)
	git ls-files | xargs ls -d | xargs tar cz | objcopy --add-section .source=/dev/stdin xmpp.so

sexp/sexp.a: sexp/parser.o sexp/lexer.o sexp/driver.o
	ar -r $@ $^

sexp/parser.o: sexp/parser.yy
	cd sexp && bison -t -d -v parser.yy
	$(CXX) $(CPPFLAGS) -fvisibility=default -c sexp/parser.tab.cc -o $@

sexp/lexer.o: sexp/lexer.l
	cd sexp && flex -d --outfile=lexer.yy.cc lexer.l
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
