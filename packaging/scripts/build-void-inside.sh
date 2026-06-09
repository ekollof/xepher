#!/bin/sh
# Runs INSIDE a Void Linux docker container as root.
# POSIX sh — compatible with OpenBSD pdksh (oksh).
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only.
# Produces: xepher-<version>_1.<arch>.xbps in $OUTPUT_DIR

set -e

. /project/packaging/scripts/prepare-source-tree.sh
. /project/packaging/scripts/install-build-deps.sh

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"
DEPS_STAMP=/opt/xepher-build/void-deps.stamp

echo "=== [Void] Building xepher ${VERSION} ==="

xepher_install_void_deps() {
    xbps-install -Syu --yes xbps
    xbps-install -Syu --yes
    xbps-install -y \
        base-devel \
        git bison flex make pkg-config \
        clang \
        libstrophe-devel \
        libxml2-devel \
        lmdb-devel \
        libsignal-protocol-c-devel \
        libomemo-c-devel \
        libgcrypt-devel \
        gpgme-devel \
        fmt-devel \
        libcurl-devel \
        openssl-devel \
        weechat-devel
}

xepher_install_build_deps_once xepher_install_void_deps

# Create a writable build directory
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

# Copy source and strip host build artifacts before compiling in-container.
prepare_source_tree "${BUILD_DIR}/src"

# Build xmpp.so
cd "${BUILD_DIR}/src"
make PACKAGE_BUILD=1 weechat-xmpp

# Assemble fake destdir for xbps-create
DESTDIR="${BUILD_DIR}/destdir"
ARCH=$(xbps-uhelper arch)

install -Dm755 xmpp.so    "${DESTDIR}/usr/lib/weechat/plugins/xmpp.so"
install -Dm644 LICENSE     "${DESTDIR}/usr/share/licenses/xepher/LICENSE"
install -Dm644 README.md   "${DESTDIR}/usr/share/doc/xepher/README.md"

# Write the xbps-create manifest
PKGVER="${VERSION}_1"
mkdir -p "${BUILD_DIR}/pkg"
cat > "${BUILD_DIR}/pkg/props.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict/>
</plist>
EOF

# xbps-create packs a destdir into a .xbps package
cd "${BUILD_DIR}"
xbps-create \
    --architecture "${ARCH}" \
    --pkgver "xepher-${PKGVER}" \
    --desc "Xepher — WeeChat plugin for XMPP/Jabber protocol" \
    --homepage "https://github.com/ekollof/xepher" \
    --license "MPL-2.0" \
    --dependencies "weechat>=3.0 libstrophe libxml2 lmdb libsignal-protocol-c libomemo-c gpgme fmt libcurl openssl libgcrypt" \
    "${DESTDIR}"

# Copy results out
mkdir -p "${OUTPUT_DIR}"
cp "${BUILD_DIR}/xepher-${PKGVER}.${ARCH}.xbps" "${OUTPUT_DIR}/"

echo "=== [Void] Done. Packages in ${OUTPUT_DIR} ==="
ls -lh "${OUTPUT_DIR}/"*.xbps
