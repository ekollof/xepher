#!/usr/bin/env -S gmake all
# vim: set noexpandtab:
# Thin GNU make wrapper around CMake + Ninja.
# Legacy pre-CMake rules: legacy/makefile.full (reference only).

UNAME_S := $(shell uname -s)
BUILD_DIR := build

# Parallel compilation: enabled by default; override with `make -j1` when debugging.
NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Default toolchain: Clang/Clang++ (C++23). GNU make predefines CXX=g++.
ifeq ($(UNAME_S),Darwin)
HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
export PKG_CONFIG_PATH := $(HOMEBREW_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
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

# ccache via CMAKE_*_COMPILER_LAUNCHER (CMake-native). CXX="ccache clang++" is
# normalized to launcher + real compiler. Set CCACHE=0 to disable.
ifneq ($(CCACHE),0)
ifneq ($(findstring ccache,$(CXX))$(findstring ccache,$(CC)),)
CXX := $(patsubst ccache ,,$(CXX))
CC := $(patsubst ccache ,,$(CC))
endif
ifneq ($(shell command -v ccache 2>/dev/null),)
CCACHE_LAUNCHER := ccache
endif
endif
export CC
export CXX

CMAKE ?= cmake
ifneq ($(shell command -v ninja 2>/dev/null),)
CMAKE_GENERATOR := -G Ninja
else
CMAKE_GENERATOR := -G "Unix Makefiles"
endif

CMAKE_ARGS := -S . -B $(BUILD_DIR) $(CMAKE_GENERATOR)
CMAKE_ARGS += -DCMAKE_C_COMPILER="$(CC)" -DCMAKE_CXX_COMPILER="$(CXX)"
ifneq ($(CCACHE_LAUNCHER),)
CMAKE_ARGS += -DCMAKE_C_COMPILER_LAUNCHER="$(CCACHE_LAUNCHER)"
CMAKE_ARGS += -DCMAKE_CXX_COMPILER_LAUNCHER="$(CCACHE_LAUNCHER)"
else ifeq ($(CCACHE),0)
CMAKE_ARGS += -DCMAKE_C_COMPILER_LAUNCHER=
CMAKE_ARGS += -DCMAKE_CXX_COMPILER_LAUNCHER=
endif
CMAKE_ARGS += -DCMAKE_BUILD_TYPE=$(if $(DEBUG),Debug,Release)
CMAKE_ARGS += -DXEPHER_PACKAGE_BUILD=$(if $(PACKAGE_BUILD),ON,OFF)
CMAKE_ARGS += -DXEPHER_BUILD_TOOLS=$(if $(PACKAGE_BUILD),OFF,ON)
CMAKE_ARGS += -DXEPHER_ENABLE_ASAN=$(if $(ASAN),ON,OFF)
# .source ELF embed is opt-in (slow post-link tar). make release sets EMBED_SOURCE=1.
CMAKE_ARGS += -DXEPHER_EMBED_SOURCE=$(if $(filter 1,$(EMBED_SOURCE)),ON,OFF)

BUILD_ARGS := --build $(BUILD_DIR) -j$(NPROC)

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
OBJCOPY ?= $(shell command -v objcopy 2>/dev/null || command -v llvm-objcopy 2>/dev/null || true)
export LIBDIR
export OBJCOPY

.DEFAULT_GOAL := all

.PHONY: all configure weechat-xmpp xmpp.so test clean distclean install install-deps release coverage debug check diff tools seed-libdiff

configure:
	$(CMAKE) $(CMAKE_ARGS)

all: weechat-xmpp
ifneq ($(DEBUG),)
	+$(MAKE) test
else
	@echo ">>> Skipping doctests (use DEBUG=1 or make test)."
endif

weechat-xmpp: configure
	$(CMAKE) $(BUILD_ARGS) --target weechat-xmpp

xmpp.so: weechat-xmpp

test: configure
ifneq ($(DEBUG),)
	$(CMAKE) $(BUILD_ARGS) --target xepher_test
else
	@echo ">>> Doctests require DEBUG=1 (or reconfigure with -DCMAKE_BUILD_TYPE=Debug)."
	@exit 1
endif

clean:
	@if [ -d "$(BUILD_DIR)" ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi
	$(RM) -f xmpp.so tests/xmpp.cov.so tests/run tests/run.cov compile_commands.json
	$(RM) -f tools/dump_mam_db tools/dump_omemo_db
	$(RM) -rf obj
	$(MAKE) -C deps/diff clean || true

distclean: clean
	$(RM) -rf $(BUILD_DIR) .depend *~

install: weechat-xmpp
	@$(MAKE) -f legacy/install.mk install

install-deps:
	./install-deps.sh

# Rebuild with .source embed so install.mk release can dump the section.
release:
	$(MAKE) EMBED_SOURCE=1 weechat-xmpp
	@$(MAKE) -f legacy/install.mk release

coverage: configure
ifneq ($(DEBUG),)
	$(CMAKE) $(BUILD_ARGS) --target coverage
else
	@echo ">>> Coverage requires DEBUG=1 (coverage-instrumented objects)."
	@exit 1
endif

tools: configure
	$(CMAKE) $(BUILD_ARGS) --target tools

debug: weechat-xmpp
ifeq ($(UNAME_S),Darwin)
	lldb -- weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
else ifneq (,$(filter $(UNAME_S),FreeBSD OpenBSD NetBSD))
	lldb -- weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
else
	gdb -ex "handle SIGPIPE nostop noprint pass" --args \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
endif

check:
	clang-check --analyze src/*.c src/*.cc src/*.cpp

diff:
	$(CMAKE) $(BUILD_ARGS) --target xepher_libdiff

# Seed vendored libdiff.a without a full CMake configure (packaging tarballs lack .git).
seed-libdiff:
	@if [ ! -f deps/diff/libdiff.a ]; then \
		echo ">>> Seeding deps/diff/libdiff.a"; \
		cd deps/diff && \
		echo "HAVE___PROGNAME=1" > configure.local && \
		env -u MAKEFLAGS -u MFLAGS CC="$(CC)" sh ./configure && \
		env -u MAKEFLAGS -u MFLAGS $(MAKE) -j1 CC="$(CC)" CFLAGS="-fPIC"; \
	fi