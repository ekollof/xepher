#!/usr/bin/env -S gmake all
# Thin delegator — canonical build is CMake (tools/CMakeLists.txt).

.PHONY: all clean

all:
	@$(MAKE) -C .. tools

clean:
	$(RM) -f dump_mam_db dump_omemo_db