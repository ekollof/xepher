# Packaging Guide for Xepher

This directory contains packaging infrastructure for building distribution packages.

## Supported Distributions

- **Debian/Ubuntu** (.deb packages)
- **Fedora/RHEL/CentOS** (.rpm packages)
- **Arch Linux** (.pkg.tar.zst packages)

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
tar czf ~/rpmbuild/SOURCES/xepher-0.2.0.tar.gz xepher/

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

## Package Contents

All packages install:
- Plugin binary: `/usr/lib/weechat/plugins/xmpp.so`
- Documentation: `/usr/share/doc/xepher/`
- License: `/usr/share/licenses/xepher/` (Arch) or `/usr/share/doc/xepher/` (Debian/RPM)

## Dependencies

### Runtime Dependencies
- weechat (>= 1.0)
- libstrophe
- libxml2
- lmdb
- libsignal-protocol-c
- gpgme
- libfmt
- libcurl
- openssl

### Build Dependencies
- g++ (>= 12) or clang++ with C++23 support
- git
- bison
- flex
- Development headers for all runtime dependencies

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
git commit -m "Update to version 0.2.0"
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
