#!/bin/sh
# Runs INSIDE a Void Linux docker container as root.
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only.
# Produces: xepher-<version>_1.<arch>.xbps in $OUTPUT_DIR

set -e

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"

echo "=== [Void] Building xepher ${VERSION} ==="

# Sync and install build deps (xbps must be updated first)
xbps-install -Syu --yes xbps
xbps-install -Syu --yes
xbps-install -y \
    base-devel \
    git bison flex make pkg-config \
    gcc \
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

# Create a writable build directory
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

# Copy source (submodules already present in /project)
cp -a /project/. "${BUILD_DIR}/src"

# Build xmpp.so
cd "${BUILD_DIR}/src"
make weechat-xmpp

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
