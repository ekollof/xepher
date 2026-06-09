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
	env DYLD_INSERT_LIBRARIES=$(DEBUG) lldb -- \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
else ifneq (,$(filter $(UNAME_S),FreeBSD OpenBSD NetBSD))
debug: xmpp.so
	env LD_PRELOAD=$(DEBUG) lldb -- \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
else
debug: xmpp.so
	env LD_PRELOAD=$(DEBUG) gdb -ex "handle SIGPIPE nostop noprint pass" --args \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
endif

tests/xmpp.cov.so: $(COVS) $(DEPS) $(HDRS)
	$(CXX) --coverage $(SHARED_FLAG) $(LDFLAGS) -o tests/xmpp.cov.so $(AS_NEEDED) $(COVS) $(DEPS) $(LDLIBS)

TEST_CPPFLAGS := $(subst -Ideps/lmdbxx,-I../deps/lmdbxx,$(subst -Isrc,-I../src,$(CPPFLAGS)))

tests/run: $(COVS) tests/main.cc tests/xmpp.cov.so $(wildcard tests/*.inl)
	cd tests && $(CXX) $(TEST_CPPFLAGS) $(LDFLAGS) -o run main.cc $(patsubst %,../%,$(DEPS)) $(LDLIBS) \
		$(TEST_LDFLAGS) $(PWD)/tests/xmpp.cov.so

# Hard cap for the whole doctest binary (per-test SIGALRM is in tests/timeout_listener.hh).
TEST_RUN_TIMEOUT ?= 120

.PHONY: test
ifeq ($(SKIP_DOCTEST),1)
test:
	@echo ">>> Doctests skipped (SKIP_DOCTEST=1)."
	@echo ">>> Run on Linux/CI, or vendor doctest headers under deps/doctest/ and: gmake SKIP_DOCTEST=0 test"
else
test: tests/run
	cd tests && timeout --kill-after=5 $(TEST_RUN_TIMEOUT) ./run -sm
endif

.PHONY: coverage
ifeq ($(SKIP_DOCTEST),1)
coverage:
	@echo ">>> Coverage skipped (doctests disabled; SKIP_DOCTEST=1)"
else
coverage: tests/run
	gcovr --txt -s --merge-mode-functions=separate
endif

.PHONY: check
check:
	clang-check --analyze src/*.c src/*.cc src/*.cpp
