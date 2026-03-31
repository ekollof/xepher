#!/bin/sh
# Runs INSIDE an Alpine Linux docker container as root.
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only.
# Produces: xepher-<version>-r0.apk in $OUTPUT_DIR

set -e

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"

echo "=== [Alpine] Building xepher ${VERSION} ==="

# Enable community repo (needed for libomemo-c, libsignal-protocol-c, weechat-dev)
echo "https://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories
apk update

# Install build tools and dependencies
apk add --no-cache \
    alpine-sdk \
    git bison flex make pkgconf \
    g++ \
    libstrophe-dev \
    libxml2-dev \
    lmdb-dev \
    libsignal-protocol-c-dev \
    libomemo-c-dev \
    libgcrypt-dev \
    gpgme-dev \
    fmt-dev \
    curl-dev \
    openssl-dev \
    weechat-dev

# abuild must run as a non-root user — create a dedicated build user
adduser -D -G abuild builder
# Grant passwordless sudo to builder (for apk calls inside abuild)
echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

# Set up abuild signing key for the builder user and trust it system-wide
su builder -c 'abuild-keygen -a -n'
# Copy the public key into /etc/apk/keys so abuild can update the local repo index
cp /home/builder/.abuild/*.pub /etc/apk/keys/

# Create a writable build directory owned by builder
BUILD_DIR=$(mktemp -d)
chown builder:abuild "${BUILD_DIR}"

# Copy source into a versioned directory; remove pre-built artifacts so the
# container compiles from scratch (host xmpp.so links glibc, not musl).
cp -a /project/. "${BUILD_DIR}/xepher-${VERSION}"
find "${BUILD_DIR}/xepher-${VERSION}" -name '.git' -exec rm -rf {} + 2>/dev/null || true
rm -f "${BUILD_DIR}/xepher-${VERSION}/xmpp.so" "${BUILD_DIR}/xepher-${VERSION}"/*.o
chown -R builder:abuild "${BUILD_DIR}"

# Create local source tarball
cd "${BUILD_DIR}"
tar czf "xepher-${VERSION}.tar.gz" "xepher-${VERSION}/"
chown builder:abuild "xepher-${VERSION}.tar.gz"

# Compute sha256 of the local tarball for the APKBUILD
SHA256=$(sha256sum "${BUILD_DIR}/xepher-${VERSION}.tar.gz" | awk '{print $1}')

# Write APKBUILD using the local tarball
mkdir -p "${BUILD_DIR}/apkbuild"
cp "${BUILD_DIR}/xepher-${VERSION}.tar.gz" "${BUILD_DIR}/apkbuild/"
chown -R builder:abuild "${BUILD_DIR}/apkbuild"

cat > "${BUILD_DIR}/apkbuild/APKBUILD" << APKBUILD_EOF
# Maintainer: Emiel Kollof <emiel@kollof.nl>
pkgname=xepher
pkgver=${VERSION}
pkgrel=0
pkgdesc="Xepher — WeeChat plugin for XMPP/Jabber protocol"
url="https://github.com/ekollof/xepher"
arch="x86_64 aarch64"
license="MPL-2.0"
depends="
    weechat
    libstrophe
    libxml2
    lmdb
    libsignal-protocol-c
    libomemo-c
    gpgme
    fmt
    curl
    openssl
    libgcrypt
"
makedepends="
    g++
    bison
    flex
    make
    pkgconf
    git
    libstrophe-dev
    libxml2-dev
    lmdb-dev
    libsignal-protocol-c-dev
    libomemo-c-dev
    libgcrypt-dev
    gpgme-dev
    fmt-dev
    curl-dev
    openssl-dev
    weechat-dev
"
source="\${pkgname}-\${pkgver}.tar.gz"
sha256sums="${SHA256}  xepher-${VERSION}.tar.gz"

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
APKBUILD_EOF

# Set REPODEST and build as builder user
export REPODEST="${BUILD_DIR}/repo"
su builder -c "
    export REPODEST='${BUILD_DIR}/repo'
    cd '${BUILD_DIR}/apkbuild'
    abuild -r
"

# Copy .apk files to output (exclude doc/dbg sub-packages)
mkdir -p "${OUTPUT_DIR}"
find "${REPODEST}" -name '*.apk' ! -name '*-doc-*' ! -name '*-dbg-*' \
    -exec cp {} "${OUTPUT_DIR}/" \;
# Fix ownership so host user can read them
chmod a+r "${OUTPUT_DIR}"/*.apk 2>/dev/null || true

echo "=== [Alpine] Done. Packages in ${OUTPUT_DIR} ==="
ls -lh "${OUTPUT_DIR}"/*.apk
