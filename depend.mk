#!/usr/bin/env -S gmake depend
# vim: set noexpandtab:

.PHONY: depend
depend: $(DEPS) $(SRCS) $(HDRS)
	echo > ./.depend
	for src in $(SRCS) tests/main.cc; do \
		dir="$$(dirname $$src)"; \
		base="$$(basename $$src)"; \
		case "$$base" in \
		*.cpp) \
			objdir="$$(echo $$dir | sed 's|src|obj|')"; \
			obj="$$(echo $$base | sed 's|\.cpp|\.o|')"; \
			echo "$(CXX) $(CPPFLAGS) -MM -MF - \
				-MT $$objdir/$$obj $$dir/$$base >> ./.depend"; \
			$(CXX) $(CPPFLAGS) -MM -MF - \
				-MT $$objdir/$$obj $$dir/$$base >> ./.depend || true ;; \
		*.c) \
			objdir="$$(echo $$dir | sed 's|src|obj|')"; \
			obj="$$(echo $$base | sed 's|\.c|\.o|')"; \
			echo "$(CC) $(CFLAGS) -MM -MF - \
				-MT $$objdir/$$obj $$dir/$$base >> ./.depend"; \
			$(CC) $(CFLAGS) -MM -MF - \
				-MT $$objdir/$$obj $$dir/$$base >> ./.depend || true ;; \
		*) \
			continue ;; \
		esac; \
	done

-include .depend
