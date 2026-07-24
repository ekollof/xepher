#!/bin/sh
# Runs INSIDE an Alpine Linux docker container as root.
# POSIX sh — compatible with OpenBSD pdksh (oksh).
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only.
# Produces: xepher-<version>-r0.apk in $OUTPUT_DIR

set -e

. /project/packaging/scripts/prepare-source-tree.sh
. /project/packaging/scripts/install-build-deps.sh

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"
DEPS_STAMP=/opt/xepher-build/alpine-deps.stamp

echo "=== [Alpine] Building xepher ${VERSION} ==="

xepher_install_alpine_deps() {
    # Enable community repo (needed for libomemo-c, libsignal-protocol-c, weechat-dev)
    if ! grep -q 'alpine/edge/community' /etc/apk/repositories 2>/dev/null; then
        echo "https://dl-cdn.alpinelinux.org/alpine/edge/community" >> /etc/apk/repositories
    fi
    apk update
    apk add --no-cache \
        alpine-sdk \
        git bison flex flex-dev make pkgconf \
        clang cmake ninja \
        expat-dev \
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

    # abuild must run as a non-root user — idempotent for persistent containers
    if ! id builder >/dev/null 2>&1; then
        adduser -D -G abuild builder
    fi
    if ! grep -q '^builder ALL=(ALL) NOPASSWD: ALL' /etc/sudoers 2>/dev/null; then
        echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
    fi
    # abuild < 3.18: ~/.abuild/  — abuild ≥ 3.18 (edge): ~/.config/abuild/
    if [ ! -f /home/builder/.abuild/abuild.conf ] \
        && [ ! -f /home/builder/.config/abuild/abuild.conf ]; then
        su builder -c 'abuild-keygen -a -n'
    fi
    # Install any generated public keys into the apk trust store.
    found_pub=0
    for keydir in /home/builder/.abuild /home/builder/.config/abuild; do
        for pub in "${keydir}"/*.pub; do
            [ -f "${pub}" ] || continue
            cp "${pub}" /etc/apk/keys/
            found_pub=1
        done
    done
    if [ "${found_pub}" -eq 0 ]; then
        echo "ERROR: no abuild public key under ~/.abuild or ~/.config/abuild" >&2
        exit 1
    fi
}

xepher_install_build_deps_once xepher_install_alpine_deps

# Create a writable build directory owned by builder
BUILD_DIR=$(mktemp -d)
chown builder:abuild "${BUILD_DIR}"

# Copy source and strip host build artifacts (host xmpp.so is glibc, not musl).
prepare_source_tree "${BUILD_DIR}/xepher-${VERSION}"
find "${BUILD_DIR}/xepher-${VERSION}" -name '.git' -exec rm -rf {} + 2>/dev/null || true
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
    clang
    cmake
    ninja
    expat-dev
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
    # Tarball has no .git; libdiff.a is pre-seeded in prepare_source_tree.
    rm -rf obj build
    rm -f xmpp.so
    make PACKAGE_BUILD=1 weechat-xmpp
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
