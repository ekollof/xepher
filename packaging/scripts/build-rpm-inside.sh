#!/bin/sh
# Runs INSIDE a Fedora docker container as root.
# POSIX sh — compatible with OpenBSD pdksh (oksh).
# Called by packaging/distrobox-build.sh — do not invoke directly.
#
# /project is the host repo mounted read-only.

set -e

. /project/packaging/scripts/prepare-source-tree.sh
. /project/packaging/scripts/install-build-deps.sh

VERSION="${1:-0.3.0}"
OUTPUT_DIR="${2:-/output}"
DEPS_STAMP=/opt/xepher-build/fedora-deps.stamp

echo "=== [Fedora] Building xepher ${VERSION} ==="

xepher_install_fedora_deps() {
    xepher_as_root dnf install -y \
        rpm-build rpmdevtools \
        clang git bison flex \
        libstrophe-devel \
        libxml2-devel \
        lmdb-devel \
        libsignal-protocol-c-devel \
        libomemo-c-devel \
        protobuf-c \
        protobuf-c-devel \
        gpgme-devel \
        libgcrypt-devel \
        fmt-devel \
        libcurl-devel \
        openssl-devel \
        weechat-devel
}

xepher_install_build_deps_once xepher_install_fedora_deps

# Setup RPM build tree
rpmdev-setuptree

# Copy source (submodules already present in /project) to a writable temp dir
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

prepare_source_tree "${BUILD_DIR}/xepher-${VERSION}"

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
