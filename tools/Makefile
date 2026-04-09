# tools/Makefile — builds standalone developer tools.
# Does NOT affect the main xmpp.so build (root Makefile).

CXX      ?= g++
CXXFLAGS  = -std=c++23 -O2 -Wall -Wextra -Wpedantic
LDFLAGS   =
LDLIBS    = -llmdb

BINDIR   ?= .

.PHONY: all clean

all: $(BINDIR)/dump_omemo_db $(BINDIR)/dump_mam_db

$(BINDIR)/dump_omemo_db: dump_omemo_db.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

$(BINDIR)/dump_mam_db: dump_mam_db.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f $(BINDIR)/dump_omemo_db $(BINDIR)/dump_mam_db
