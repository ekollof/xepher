#!/bin/sh
# Runs INSIDE an Arch Linux distrobox container.
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only. We use a local source tarball
# so makepkg doesn't need to clone from GitHub (avoids tag/network dependency).

set -e

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"

echo "=== [Arch] Building xepher ${VERSION} ==="

# Install base-devel and git if not present
sudo pacman -Sy --needed --noconfirm base-devel git

# Create a writable build directory
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

# Copy source (submodules already present) into a versioned directory and
# create a local tarball for makepkg to use as the source.
cp -a /project/. "${BUILD_DIR}/xepher-${VERSION}"

# Remove .git dirs to avoid makepkg VCS-source confusion
find "${BUILD_DIR}/xepher-${VERSION}" -name '.git' -exec rm -rf {} + 2>/dev/null || true

cd "${BUILD_DIR}"
tar czf "xepher-${VERSION}.tar.gz" "xepher-${VERSION}/"

# Write a PKGBUILD that uses the local tarball rather than cloning
cat > "${BUILD_DIR}/PKGBUILD" << PKGBUILD_EOF
# Maintainer: Emiel Kollof <emiel@kollof.nl>
pkgname=xepher
pkgver=${VERSION}
pkgrel=1
pkgdesc="Xepher — WeeChat plugin for XMPP/Jabber protocol"
arch=('x86_64' 'aarch64')
url="https://github.com/ekollof/xepher"
license=('MPL2')
depends=(
    'weechat'
    'libstrophe'
    'libxml2'
    'lmdb'
    'libsignal-protocol-c'
    'libomemo-c'
    'gpgme'
    'fmt'
    'curl'
    'openssl'
)
makedepends=(
    'gcc'
    'git'
    'bison'
    'flex'
)
source=("xepher-\${pkgver}.tar.gz")
sha256sums=('SKIP')

build() {
    cd "\${srcdir}/xepher-\${pkgver}"
    make weechat-xmpp
}

package() {
    cd "\${srcdir}/xepher-\${pkgver}"
    install -Dm755 xmpp.so "\${pkgdir}/usr/lib/weechat/plugins/xmpp.so"
    install -Dm644 LICENSE "\${pkgdir}/usr/share/licenses/\${pkgname}/LICENSE"
    install -Dm644 README.md "\${pkgdir}/usr/share/doc/\${pkgname}/README.md"
}
PKGBUILD_EOF

# makepkg must not run as root; distrobox runs as the regular host user — OK.
# -s  : install missing makedepends
# -f  : force rebuild
# --noconfirm : no prompts
makepkg -sf --noconfirm

# Copy results out
mkdir -p "${OUTPUT_DIR}"
cp "${BUILD_DIR}"/*.pkg.tar.zst "${OUTPUT_DIR}/"

echo "=== [Arch] Done. Packages in ${OUTPUT_DIR} ==="
ls -lh "${OUTPUT_DIR}"/*.pkg.tar.zst
