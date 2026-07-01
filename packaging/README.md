# Packaging Guide for Xepher

This directory contains packaging infrastructure for building distribution packages.

## Supported Distributions

- **Debian/Ubuntu** (.deb packages)
- **Fedora/RHEL/CentOS** (.rpm packages)
- **Arch Linux** (.pkg.tar.zst packages)
- **Alpine Linux** (.apk packages)
- **Void Linux** (.xbps packages)
- **FreeBSD** (.pkg packages — port skeleton in `packaging/freebsd/`)
- **OpenBSD** (.tgz packages — port skeleton in `packaging/openbsd/`)

## Quick Start

### Automated Build

```bash
cd packaging
./build-packages.sh [version]
```

This will detect your OS and build the appropriate package. Output goes to `packaging/build/`.

### Manual Building

#### Debian/Ubuntu

```bash
# Install build tools
sudo apt-get install debhelper devscripts build-essential

# Copy debian directory to project root
cp -r packaging/debian .

# Build package
debuild -us -uc -b

# Install
sudo dpkg -i ../xepher_*.deb
```

#### Fedora/RHEL/CentOS

```bash
# Install build tools
sudo dnf install rpm-build rpmdevtools

# Setup RPM build tree
rpmdev-setuptree

# Create source tarball
cd ..
tar czf ~/rpmbuild/SOURCES/xepher-0.3.0.tar.gz xepher/

# Copy spec file
cp xepher/packaging/rpm/weechat-xmpp.spec ~/rpmbuild/SPECS/xepher.spec

# Build package
cd ~/rpmbuild/SPECS
rpmbuild -bb xepher.spec

# Install
sudo dnf install ~/rpmbuild/RPMS/*/xepher-*.rpm
```

#### Arch Linux

```bash
# Copy PKGBUILD to a build directory
cp packaging/arch/PKGBUILD /tmp/build/
cd /tmp/build

# Build package
makepkg -sf

# Install
sudo pacman -U xepher-*.pkg.tar.zst
```

#### FreeBSD

Port skeleton: `packaging/freebsd/`. All dependencies are in the official ports tree.

```sh
# Install dependencies (or use install-deps.sh on FreeBSD)
sudo pkg install gmake git bison flex libstrophe libxml2 lmdb \
    libsignal-protocol-c libomemo-c libfmt gpgme curl weechat

# Copy into a ports tree and build
sudo cp -R packaging/freebsd /usr/ports/net-im/xepher
cd /usr/ports/net-im/xepher
make makesum
make package

# Or build a .pkg from the repo without the ports framework
sh packaging/scripts/build-freebsd-inside.sh 0.8.1 ./packaging/build
```

#### OpenBSD

Port skeleton: `packaging/openbsd/`. All dependencies are in the official ports tree.

```sh
# Install dependencies (or use install-deps.sh on OpenBSD)
doas pkg_add gmake git bison flex libstrophe libxml lmdb \
    libsignal-protocol-c libomemo-c fmt gpgme curl weechat

# Copy into a ports tree and build
doas cp packaging/openbsd/Makefile /usr/ports/net/xepher/
doas cp packaging/openbsd/pkg/* /usr/ports/net/xepher/pkg/
cd /usr/ports/net/xepher
make package

# Or use the helper script from the repo root on OpenBSD
sh packaging/scripts/build-openbsd-inside.sh 0.8.1 ./packaging/build
```

## Updating Package Versions

When releasing a new version, update these files:

1. **Debian**: `packaging/debian/changelog`
   ```bash
   dch -v 0.2.1-1 "New release description"
   ```

2. **RPM**: `packaging/rpm/weechat-xmpp.spec`

3. **Arch**: `packaging/arch/PKGBUILD`
   - Update `pkgver=` field
   - Update `pkgrel=` (reset to 1 for new version)
   - Update checksums if needed

4. **FreeBSD**: `packaging/freebsd/Makefile`
   - Update `PORTVERSION=`
   - Run `make makesum` in the ports tree

5. **OpenBSD**: `packaging/openbsd/Makefile`
   - Update `V=` and `GH_TAGNAME=`

## Package Contents

All packages install:
- Plugin binary: `*/lib/weechat/plugins/xmpp.so` (`/usr/lib` on Linux, `/usr/local/lib` on BSD)
- Documentation: `*/share/doc/xepher/`
- License: `*/share/licenses/xepher/` (Arch, FreeBSD) or `*/share/doc/xepher/` (Debian/RPM, OpenBSD)

## Dependencies

### Runtime Dependencies
- weechat (>= 3.0)
- libstrophe
- libxml2
- lmdb
- libsignal-protocol-c
- libomemo-c
- gpgme
- libfmt
- libcurl
- openssl

### Build Dependencies
- clang/clang++ (>= 14) with C++23 support
- cmake (>= 3.22) and ninja (Unix Makefiles fallback if ninja is absent)
- git
- bison
- flex
- Development headers for all runtime dependencies

Packages build via the thin `makefile` wrapper: `make PACKAGE_BUILD=1 weechat-xmpp`.
On FreeBSD/OpenBSD use `gmake` instead of `make`.

## Testing Packages

After building, test the package:

```bash
# Install package
sudo <package-manager> install packaging/build/<package-file>

# Load WeeChat
weechat

# In WeeChat, load plugin
/plugin load xmpp

# Verify it loaded
/plugin list

# Try basic functionality
/xmpp add <account> <jid>
```

## Troubleshooting

### Missing Dependencies

If build fails due to missing dependencies:
```bash
# Debian/Ubuntu
sudo apt-get build-dep .

# Fedora/RHEL
sudo dnf builddep packaging/rpm/weechat-xmpp.spec
# Arch
makepkg -s  # automatically installs makedepends
```

### Library Compatibility

Package names and versions vary by distribution. If a dependency is unavailable:

1. Check the distribution's package search:
   - Debian/Ubuntu: https://packages.debian.org / https://packages.ubuntu.com
   - Fedora: https://packages.fedoraproject.org
   - Arch: https://archlinux.org/packages

2. Update the package name in the appropriate packaging file

### Submodule Issues

The build process requires git submodules:
```bash
git submodule update --init --recursive
```

This is automated in the build scripts.

## Distribution Repository Submission

### AUR (Arch User Repository)

```bash
# Clone AUR repository
git clone ssh://aur@aur.archlinux.org/xepher.git

# Copy PKGBUILD
cp packaging/arch/PKGBUILD xepher/

# Update .SRCINFO
cd xepher
makepkg --printsrcinfo > .SRCINFO

# Commit and push
git add PKGBUILD .SRCINFO
git commit -m "Update to version 0.3.0"
git push
```

### Debian/Ubuntu PPA

See: https://help.launchpad.net/Packaging/PPA

### Fedora/EPEL

See: https://docs.fedoraproject.org/en-US/package-maintainers/

## Contact

For packaging issues, contact:
- Maintainer: Emiel Kollof <emiel@kollof.nl>
- Repository: https://github.com/ekollof/xepher
