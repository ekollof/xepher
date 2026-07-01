#!/usr/bin/env -S gmake install
# vim: set noexpandtab:

WEECHATHOME ?= ~/.local/share/weechat/

install-deps:
	@echo "Installing system dependencies..."
	./install-deps.sh

install: xmpp.so
ifeq ($(shell id -u),0)
	mkdir -p $(DESTDIR)$(LIBDIR)/weechat/plugins
	cp xmpp.so $(DESTDIR)$(LIBDIR)/weechat/plugins/xmpp.so
	chmod 644 $(DESTDIR)$(LIBDIR)/weechat/plugins/xmpp.so
else
	mkdir -p $(WEECHATHOME)/plugins
	cp xmpp.so $(WEECHATHOME)/plugins/xmpp.so
	chmod 755 $(WEECHATHOME)/plugins/xmpp.so
	mkdir -p $(WEECHATHOME)/python/autoload
	cp scripts/feed_compose.py $(WEECHATHOME)/python/feed_compose.py
	chmod 644 $(WEECHATHOME)/python/feed_compose.py
	ln -sf ../feed_compose.py $(WEECHATHOME)/python/autoload/feed_compose.py
endif

release: xmpp.so
	cp xmpp.so .xmpp.so.$(SUFFIX)
	ln -sf .xmpp.so.$(SUFFIX) .xmpp.so

.xmpp.so.%:
	mkdir src$@
ifneq ($(OBJCOPY),)
	$(OBJCOPY) --dump-section .source=/dev/stdout $@ | tar -C src$@ xz
endif

.PHONY: install install-deps release .xmpp.so.%
