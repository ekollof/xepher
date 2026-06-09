#!/usr/bin/env -S gmake test coverage
# vim: set noexpandtab:

ifeq ($(UNAME_S),Darwin)
TEST_LDFLAGS := -Wl,-undefined,dynamic_lookup -Wl,-rpath,$(PWD)/tests
else ifneq (,$(filter $(UNAME_S),FreeBSD OpenBSD NetBSD))
# BSD lld does not support --allow-shlib-undefined; use -undefined,dynamic_lookup instead
TEST_LDFLAGS := -Wl,-undefined,dynamic_lookup -Wl,-rpath,$(PWD)/tests
else
TEST_LDFLAGS := -Wl,--allow-shlib-undefined -Wl,-rpath,$$PWD/tests
endif

.PHONY: debug
ifeq ($(UNAME_S),Darwin)
debug: xmpp.so
	@echo "debug target: use lldb on macOS"
	lldb -- \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
else ifneq (,$(filter $(UNAME_S),FreeBSD OpenBSD NetBSD))
debug: xmpp.so
	lldb -- \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
else
debug: xmpp.so
	gdb -ex "handle SIGPIPE nostop noprint pass" --args \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
endif

tests/xmpp.cov.so: $(COVS) $(DEPS) $(HDRS)
	$(CXX) --coverage $(SHARED_FLAG) $(LDFLAGS) -o tests/xmpp.cov.so $(AS_NEEDED) $(COVS) $(DEPS) $(LDLIBS)

# tests/run is built from tests/; rewrite repo-root -I paths (incl. vendored doctest).
TEST_CPPFLAGS := $(subst -Ideps,-I../deps,$(subst -Isrc,-I../src,$(subst -Ilibstrophe,-I../libstrophe,$(CPPFLAGS))))

tests/run: $(COVS) tests/main.cc tests/xmpp.cov.so $(wildcard tests/*.inl)
	cd tests && $(CXX) $(TEST_CPPFLAGS) $(LDFLAGS) -o run main.cc $(patsubst %,../%,$(DEPS)) $(LDLIBS) \
		$(TEST_LDFLAGS) $(PWD)/tests/xmpp.cov.so

# Hard cap for the whole doctest binary (per-test SIGALRM is in tests/timeout_listener.hh).
TEST_RUN_TIMEOUT ?= 120

.PHONY: test
test: tests/run
	cd tests && timeout --kill-after=5 $(TEST_RUN_TIMEOUT) ./run -sm

.PHONY: coverage
coverage: tests/run
	gcovr --txt -s --merge-mode-functions=separate

.PHONY: check
check:
	clang-check --analyze src/*.c src/*.cc src/*.cpp
