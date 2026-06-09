#!/bin/sh
# Runs INSIDE a Debian/Ubuntu distrobox container.
# POSIX sh — compatible with OpenBSD pdksh (oksh).
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only. We copy it to a writable
# build dir so debuild can create its temp files and output alongside the source.

set -e

. /project/packaging/scripts/prepare-source-tree.sh
. /project/packaging/scripts/install-build-deps.sh

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"
DEPS_STAMP=/opt/xepher-build/debian-deps.stamp

echo "=== [Debian] Building xepher ${VERSION} ==="

xepher_install_debian_deps() {
    xepher_as_root apt-get update -qq
    xepher_as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y \
        debhelper devscripts build-essential \
        clang git bison flex \
        libstrophe-dev \
        libxml2-dev \
        liblmdb-dev \
        libsignal-protocol-c-dev \
        libomemo-c-dev \
        libgcrypt20-dev \
        libgpgme-dev \
        libfmt-dev \
        libcurl4-openssl-dev \
        libssl-dev \
        weechat-dev
}

xepher_install_build_deps_once xepher_install_debian_deps

# Copy the full project source (with submodules already present) to a
# writable build directory. Use the Debian-required source dir name.
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

SRC_DIR="${BUILD_DIR}/xepher-${VERSION}"
prepare_source_tree "${SRC_DIR}"

# Place the debian/ packaging dir at the source root (from /project, read-only mount)
cp -r /project/packaging/debian "${SRC_DIR}/debian"

cd "${SRC_DIR}"

# Build binary-only package, no signing
debuild -us -uc -b

# Copy results out (debuild places .deb one level up, i.e. in $BUILD_DIR)
mkdir -p "${OUTPUT_DIR}"
cp "${BUILD_DIR}"/*.deb "${OUTPUT_DIR}/"

echo "=== [Debian] Done. Packages in ${OUTPUT_DIR} ==="
ls -lh "${OUTPUT_DIR}"/*.deb
