#!/bin/sh
# Runs INSIDE a Fedora distrobox container.
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only.

set -e

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"

echo "=== [Fedora] Building xepher ${VERSION} ==="

# Install build dependencies
sudo dnf install -y \
    rpm-build rpmdevtools \
    gcc-c++ git bison flex \
    libstrophe-devel \
    libxml2-devel \
    lmdb-devel \
    libsignal-protocol-c-devel \
    libomemo-c-devel \
    gpgme-devel \
    fmt-devel \
    libcurl-devel \
    openssl-devel \
    weechat-devel

# Setup RPM build tree
rpmdev-setuptree

# Copy source (submodules already present in /project) to a writable temp dir
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

cp -a /project/. "${BUILD_DIR}/xepher-${VERSION}"

# Create source tarball from the copy (exclude .git to keep it clean)
cd "${BUILD_DIR}"
tar czf "${HOME}/rpmbuild/SOURCES/xepher-${VERSION}.tar.gz" \
    --exclude="xepher-${VERSION}/.git" \
    "xepher-${VERSION}/"

# Copy spec file and patch version
cp /project/packaging/rpm/weechat-xmpp.spec "${HOME}/rpmbuild/SPECS/xepher.spec"
sed -i "s/^Version:.*/Version:        ${VERSION}/" "${HOME}/rpmbuild/SPECS/xepher.spec"

# Build binary RPM
rpmbuild -bb "${HOME}/rpmbuild/SPECS/xepher.spec"

# Copy results out
mkdir -p "${OUTPUT_DIR}"
find "${HOME}/rpmbuild/RPMS" -name '*.rpm' -exec cp {} "${OUTPUT_DIR}/" \;

echo "=== [Fedora] Done. Packages in ${OUTPUT_DIR} ==="
ls -lh "${OUTPUT_DIR}"/*.rpm
