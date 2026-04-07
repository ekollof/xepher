#!/usr/bin/env -S gmake test coverage
# vim: set noexpandtab:

ifeq ($(UNAME_S),Darwin)
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
else
debug: xmpp.so
	env LD_PRELOAD=$(DEBUG) gdb -ex "handle SIGPIPE nostop noprint pass" --args \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'
endif

tests/xmpp.cov.so: $(COVS) $(DEPS) $(HDRS)
	$(CXX) --coverage $(SHARED_FLAG) $(LDFLAGS) -o tests/xmpp.cov.so $(AS_NEEDED) $(COVS) $(DEPS) $(LDLIBS)

tests/run: $(COVS) tests/main.cc tests/xmpp.cov.so $(wildcard tests/*.inl)
	cd tests && $(CXX) $(subst -Ideps/lmdbxx,-I../deps/lmdbxx,$(CPPFLAGS)) $(LDFLAGS) -o run main.cc $(patsubst %,../%,$(DEPS)) $(LDLIBS) \
		$(TEST_LDFLAGS) $(PWD)/tests/xmpp.cov.so

.PHONY: test
test: tests/run
	cd tests && ./run -sm

.PHONY: coverage
coverage: tests/run
	gcovr --txt -s --merge-mode-functions=separate

.PHONY: check
check:
	clang-check --analyze src/*.c src/*.cc src/*.cpp
