#!/usr/bin/env -S gmake install
# vim: set noexpandtab:

WEECHATHOME ?= ~/.local/share/weechat/

install-deps:
	@echo "Installing system dependencies..."
	./install-deps.sh

# Install via temp + rename so a running WeeChat that has xmpp.so mmapped
# keeps the old inode until unload (in-place `cp` truncates → SIGBUS).
# After install, load the new binary with `/plugin reload xmpp`.
define install_plugin_so
	mkdir -p $(1)
	cp -f xmpp.so $(1)/xmpp.so.new
	chmod $(2) $(1)/xmpp.so.new
	mv -f $(1)/xmpp.so.new $(1)/xmpp.so
endef

install: xmpp.so
ifeq ($(shell id -u),0)
	$(call install_plugin_so,$(DESTDIR)$(LIBDIR)/weechat/plugins,644)
else
	$(call install_plugin_so,$(WEECHATHOME)/plugins,755)
	mkdir -p $(WEECHATHOME)/python/autoload
	cp -f scripts/feed_compose.py $(WEECHATHOME)/python/feed_compose.py
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
