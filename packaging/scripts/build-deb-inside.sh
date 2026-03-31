#!/bin/sh
# Runs INSIDE a Debian/Ubuntu distrobox container.
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only. We copy it to a writable
# build dir so debuild can create its temp files and output alongside the source.

set -e

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"

echo "=== [Debian] Building xepher ${VERSION} ==="

# Install build dependencies
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    debhelper devscripts build-essential \
    g++ git bison flex \
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

# Copy the full project source (with submodules already present) to a
# writable build directory. Use the Debian-required source dir name.
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

SRC_DIR="${BUILD_DIR}/xepher-${VERSION}"
cp -a /project/. "${SRC_DIR}"

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
