%global debug_package %{nil}

Name:           xepher
Version:        0.10.2
Release:        1%{?dist}
Summary:        Xepher — WeeChat plugin for XMPP/Jabber protocol

License:        MPL-2.0
URL:            https://github.com/ekollof/xepher
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  clang >= 14
BuildRequires:  cmake >= 3.22
BuildRequires:  ninja-build
BuildRequires:  expat-devel
BuildRequires:  git
BuildRequires:  bison
BuildRequires:  flex
BuildRequires:  libstrophe-devel
BuildRequires:  libxml2-devel
BuildRequires:  lmdb-devel
BuildRequires:  libsignal-protocol-c-devel
BuildRequires:  libomemo-c-devel
BuildRequires:  gpgme-devel
BuildRequires:  libgcrypt-devel
BuildRequires:  fmt-devel
BuildRequires:  libcurl-devel
BuildRequires:  openssl-devel
BuildRequires:  weechat-devel

Requires:       weechat >= 3.0
Requires:       libstrophe
Requires:       libxml2
Requires:       lmdb
Requires:       libsignal-protocol-c
Requires:       libomemo-c
Requires:       gpgme
Requires:       fmt
Requires:       libcurl
Requires:       openssl-libs

%description
WeeChat plugin for XMPP/Jabber protocol support.

Features include:
- Multi-user chat (MUC) support
- Message Archive Management (MAM - XEP-0313)
- OMEMO encryption (XEP-0384)
- PGP encryption (XEP-0027)
- Service Discovery (XEP-0030)
- User Avatar (XEP-0084)
- HTTP File Upload (XEP-0363)
- Message Carbons (XEP-0280)
- Chat State Notifications (typing indicators)
- Bookmarks management (XEP-0048)

%prep
%autosetup -n %{name}-%{version}
# Note: git submodules (deps/diff) must be bundled in the source tarball.
# When creating the tarball, run:
#   git submodule update --init --recursive
#   tar czf xepher-%%{version}.tar.gz --exclude=.git xepher/

%build
%make_build PACKAGE_BUILD=1 weechat-xmpp

%install
install -D -m 0755 xmpp.so %{buildroot}%{_libdir}/weechat/plugins/xmpp.so

%files
%license LICENSE
%doc README.md
%{_libdir}/weechat/plugins/xmpp.so

%changelog
* Wed Jul 01 2026 Emiel Kollof <emiel@kollof.nl> - 0.10.2-1
- Update to v0.10.2
- Fix(packaging): cmake/ninja in Fedora CI deps, expat-devel for Void/RPM

* Wed Jul 01 2026 Emiel Kollof <emiel@kollof.nl> - 0.10.1-1
- Update to v0.10.1
- Build: migrate to CMake + Ninja with thin gmake/make wrapper
- Build: CTest doctests, cmake/ninja packaging deps, ccache launcher
- Refactor: ConnectionPort, parse_presence, string_view member APIs
- Fix(muc): real-JID member lookup and null-safe account JID

* Tue Jun 30 2026 Emiel Kollof <emiel@kollof.nl> - 0.10.0-1
- Update to v0.10.0
- Feat: XEP-0490 MDS sync — publish MDS PEP for MUC and process MDS fetch on connect
- Feat: emoji shortcode tab completion and GitHub emoji conversion
- Feat: XEP-0045 MUC admin, registration, invites, /decline, /names
- Feat: nicklist sorting (online first) and offline roster display
- Feat: XEP-0231 BoB send, hosting, and caps advertisement
- Feat: Movim stickers, XEP-0449 stickers, XEP-0514 custom emoji via icat
- Feat: emoticon to emoji conversion in outgoing messages
- Fix(omemo): LMDB txn scoping, prekey decrypt, session bootstrap, MUC real-JID guards
- Fix(sm): preserve SM state on unexpected disconnect for auto-reconnect resume
- Fix(lmdb): recover from stale reader slots and prevent depth counter corruption
- Fix(fmt): add FMT_DEPRECATED_HEAVY_CORE for fmt 11 compatibility
- Fix(muc): prevent bare JID from being treated as occupant real JID
- Fix(edit/retract): delivery glyph alignment, /edit prefill, /reply local echo
- Perf: OMEMO GCM cipher reuse, scoped LMDB reads, on-demand MUC key fetch
- Perf: batch LMDB writes during MAM replay, cap backward buffer line scans
- Refactor: port abstraction phases 1-6 (StanzaView, UiPort, BufferPort, RenderEvent)
- Refactor: ConnectionPort, parse_presence, string_view member APIs
- Fix(muc): real-JID member lookup and null-safe account JID
- Build: migrate to CMake + Ninja with thin gmake/make wrapper
- Build: CTest doctests, cmake/ninja packaging deps, ccache launcher

* Wed Jun 10 2026 Emiel Kollof <emiel@kollof.nl> - 0.9.0-1
- Update to v0.9.0
- Fix(omemo): MUC bundle prefetch, non-anonymous gating, PM key-transport routing
- Fix(carbons): sent and plain-text carbon routing across clients
- Fix(feed): pubsub discovery, /feed close, MAM push isolation for closed feeds
- Feat: release.sh for GitHub releases and CI package builds
- Feat(packaging): FreeBSD and OpenBSD port skeletons

* Mon Jun 08 2026 Emiel Kollof <emiel@kollof.nl> - 0.8.1-1
- Update to v0.8.1
- Fix(packaging): skip .source ELF embed in distribution builds (fixes ~1.3 GB RPM/APK)

* Mon Jun 08 2026 Emiel Kollof <emiel@kollof.nl> - 0.8.0-1
- Update to v0.8.0
- Feat(muc): /create command for room creation (XEP-0045)
- Feat(muc): channel mode display (XEP-0045 §6.4/6.5)
- Feat(icat): vendored weechat-icat script; dynamic dimensions from image metadata
- Fix(icat): sync inline image previews during MAM replay; aesgcm upload support
- Fix(muc): /setmodes form submit, -k --confirm, muc#owner IQ handler registration
- Feat(xep0437): actionable Room Activity Indicator notifications
- Refactor: port abstraction layer and handler splits (Wave 1–4); 109 unit tests
- Refactor: C++23 modernization sweep; fluent Atom/RDF stanza builders (Wave 3)

* Wed Jun 03 2026 Emiel Kollof <emiel@kollof.nl> - 0.7.0-1
- Update to v0.7.0
- Fix(upload): replace unsafe OpenSSL BIO base64 with weechat_string_base_encode to prevent corrupted hash values and connection drops
- Fix(upload): use gmtime_r for thread safety; fix dangling shared_ptr in upload completion callback
- Feat(omemo): implement BTBV trust_jid(); fix /omemo trust accidentally calling distrust
- Feat(omemo): add optional [device-id] to /omemo trust; display trust levels in /omemo devices
- Refactor(omemo): remove stale omemo_atm config option and XEP-0450/XEP-0434 DOAP entries
- Feat(xep0437): implement Room Activity Indicators — subscribe on connect, display notifications

* Wed Apr 08 2026 Emiel Kollof <emiel@kollof.nl> - 0.6.1-1
- Update to v0.6.1
- Fix(omemo): cache OMEMO plaintext under all three message IDs to fix MAM replay cache miss
- Fix(user): guard weechat_info_get/weechat_color null returns; fix PM nick fallback
- Fix(message): PM nick column always shows bare JID

* Wed Apr 08 2026 Emiel Kollof <emiel@kollof.nl> - 0.6.0-1
- Update to v0.6.0
- Refactor(omemo): switch to axolotl-only (legacy XEP-0384) OMEMO with BTBV TOFU trust model
- Fix(omemo): emit flat legacy <key rid="..."> layout for Gajim/Conversations compatibility
- Fix(omemo): rewrite get_devicelist() to preserve peer devices via LMDB cache (singleton-clobber bug)

* Tue Apr 07 2026 Emiel Kollof <emiel@kollof.nl> - 0.5.1-1
- Update to v0.5.1
- Fix(omemo): show self-outbound MAM OMEMO messages as placeholder during replay
- Fix(omemo): suppress noisy bundle-fetch and stale-session-recovery log messages during MAM catchup

* Thu Apr 02 2026 Emiel Kollof <emiel@kollof.nl> - 0.5.0-1
- Update to v0.5.0
- Feat(omemo/atm): add /omemo fingerprint, /omemo approve, /omemo distrust manual trust UI (XEP-0450)
- Fix(omemo/atm): encrypt trust messages with OMEMO+SCE per XEP-0450 §4
- Fix(omemo/atm): batch multiple key-owners into one XEP-0434 trust message per XEP-0434 §4
- Fix(omemo/atm): enforce sender authentication gate and add deferred trust queues
- Fix(omemo/atm): implement XEP-0450 §4.2 distrust send path

* Thu Apr 02 2026 Emiel Kollof <emiel@kollof.nl> - 0.4.1-1
- Update to v0.4.1
- Perf: cache hdata handles in message_handler (single weechat_hdata_get() per handle per process)
- Perf: replace contains()+find() double-lookups with single find() throughout message_handler
- Perf: static constexpr nicklist group strings, const option overloads, unordered_set peer_features
- Perf: cache bare JID to avoid re-parsing on every stanza
- Perf: iq_handler ping — single find() instead of count()+operator[]
- Fix(xep0402): guard autojoin=false buffer-close with !joining to prevent MUC disappear on connect
- Fix(xep0402): accept autojoin="1" alongside autojoin="true" in XEP-0049 IQ handler

* Wed Apr 01 2026 Emiel Kollof <emiel@kollof.nl> - 0.4.0-1
- Update to v0.4.0
- UI: prefix edited messages (XEP-0308) with pencil glyph instead of diff markup
- Refactor: eliminate all raw libstrophe stanza__* / xmpp_jid_* / xmpp_uuid_gen calls

* Tue Mar 31 2026 Emiel Kollof <emiel@kollof.nl> - 0.3.0-1
- Update to v0.3.0
- Add macOS/Homebrew build support
- Add libomemo-c runtime and build dependency
- Raise minimum weechat version to 3.0
- Fix: submodule bundling note in %%prep

* Mon Jan 19 2026 Emiel Kollof <emiel@kollof.nl> - 0.2.0-1
- Initial RPM package release
- Feature: PM buffer close fix (prevents unwanted reopening)
- Feature: Typing indicators show nicknames in MUC
- Feature: Plain text is default for new PMs
- Feature: Auto-encryption detection
- Feature: /bookmark command for MUC management
- Feature: /list command for room discovery
- Feature: /ping command with feedback
- Feature: Capability caching
- Feature: HTTP Upload support (XEP-0363)
- Fix: Biboumi/IRC gateway room handling
