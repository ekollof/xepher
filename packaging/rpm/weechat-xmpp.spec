%global debug_package %{nil}

Name:           xepher
Version:        0.3.0
Release:        1%{?dist}
Summary:        Xepher — WeeChat plugin for XMPP/Jabber protocol

License:        MPL-2.0
URL:            https://github.com/ekollof/xepher
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ >= 12
BuildRequires:  git
BuildRequires:  bison
BuildRequires:  flex
BuildRequires:  libstrophe-devel
BuildRequires:  libxml2-devel
BuildRequires:  lmdb-devel
BuildRequires:  libsignal-protocol-c-devel
BuildRequires:  libomemo-c-devel
BuildRequires:  gpgme-devel
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
%make_build weechat-xmpp

%install
install -D -m 0755 xmpp.so %{buildroot}%{_libdir}/weechat/plugins/xmpp.so

%files
%license LICENSE
%doc README.md
%{_libdir}/weechat/plugins/xmpp.so

%changelog
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
