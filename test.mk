#!/usr/bin/env -S gmake test coverage
# vim: set noexpandtab:

.PHONY: debug
debug: xmpp.so
	env LD_PRELOAD=$(DEBUG) gdb -ex "handle SIGPIPE nostop noprint pass" --args \
		weechat -a -P 'alias,buflist,exec,irc,relay' -r '/plugin load ./xmpp.so'

tests/xmpp.cov.so: $(COVS) $(DEPS) $(HDRS)
	$(CXX) --coverage -shared $(LDFLAGS) -o tests/xmpp.cov.so $(AS_NEEDED) $(COVS) $(DEPS) $(LDLIBS)

tests/run: $(COVS) tests/main.cc tests/xmpp.cov.so $(wildcard tests/*.inl)
	cd tests && $(CXX) $(CPPFLAGS) $(LDFLAGS) -o run main.cc $(patsubst %,../%,$(DEPS)) $(LDLIBS) \
		-Wl,--allow-shlib-undefined -Wl,-rpath,$$PWD $$PWD/xmpp.cov.so

.PHONY: test
test: tests/run
	cd tests && ./run -sm

.PHONY: coverage
coverage: tests/run
	gcovr --txt -s --merge-mode-functions=separate

.PHONY: check
check:
	clang-check --analyze src/*.c src/*.cc src/*.cpp
