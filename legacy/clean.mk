#!/usr/bin/env -S gmake clean
# vim: set noexpandtab:

.PHONY: tidy
tidy:
	$(RM) -rf obj

.PHONY: clean
clean: tidy
	$(RM) -f xmpp.so $(OBJS) $(COVS) \
		sexp/parser.tab.cc sexp/parser.tab.hh \
		sexp/location.hh sexp/position.hh \
		sexp/stack.hh sexp/parser.output sexp/parser.o \
		sexp/lexer.o sexp/lexer.yy.cc sexp/sexp.a
	$(MAKE) -C deps/diff clean || true

.PHONY: distclean
distclean: clean
	$(RM) *~ .depend
