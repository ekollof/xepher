#!/bin/sh
# distrobox-build.sh — Build xepher packages for Debian, Fedora, and Arch Linux
# using isolated distrobox containers. Containers are torn down after building.
#
# Usage:
#   ./packaging/distrobox-build.sh [version] [--debian] [--fedora] [--arch]
#
#   version   : package version to build (default: 0.3.0)
#   --debian  : build only the Debian package
#   --fedora  : build only the Fedora/RPM package
#   --arch    : build only the Arch package
#   (no flags : build all three)
#
# Prerequisites: distrobox + docker (or podman) installed and accessible.
#
# Output: packaging/build/*.deb, *.rpm, *.pkg.tar.zst

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VERSION="${1:-0.3.0}"
OUTPUT_DIR="${PROJECT_DIR}/packaging/build"
REPO_URL="https://github.com/ekollof/xepher.git"

# Strip version arg if present (starts with digit)
case "$1" in
    [0-9]*) shift ;;
esac

# Parse target flags; if none given, build all
BUILD_DEBIAN=0
BUILD_FEDORA=0
BUILD_ARCH=0
if [ $# -eq 0 ]; then
    BUILD_DEBIAN=1
    BUILD_FEDORA=1
    BUILD_ARCH=1
fi
for arg in "$@"; do
    case "$arg" in
        --debian) BUILD_DEBIAN=1 ;;
        --fedora) BUILD_FEDORA=1 ;;
        --arch)   BUILD_ARCH=1   ;;
        *)        echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

mkdir -p "${OUTPUT_DIR}"

# ── helpers ──────────────────────────────────────────────────────────────────

container_exists() {
    distrobox list 2>/dev/null | grep -q "^.*${1}"
}

run_in_container() {
    CONTAINER="$1"
    shift
    distrobox enter "${CONTAINER}" -- "$@"
}

teardown() {
    CONTAINER="$1"
    echo ">>> Tearing down container: ${CONTAINER}"
    distrobox rm --force "${CONTAINER}" 2>/dev/null || true
}

# ── Debian ────────────────────────────────────────────────────────────────────

build_debian() {
    CONTAINER="xepher-build-debian"
    echo ""
    echo "============================================================"
    echo " Debian package build (container: ${CONTAINER})"
    echo "============================================================"

    distrobox create \
        --name "${CONTAINER}" \
        --image debian:stable \
        --yes \
        --volume "${PROJECT_DIR}:/project:ro" \
        --volume "${OUTPUT_DIR}:/output"

    run_in_container "${CONTAINER}" \
        sh /project/packaging/scripts/build-deb-inside.sh \
            "${VERSION}" /output "${REPO_URL}"

    teardown "${CONTAINER}"
}

# ── Fedora ────────────────────────────────────────────────────────────────────

build_fedora() {
    CONTAINER="xepher-build-fedora"
    echo ""
    echo "============================================================"
    echo " Fedora/RPM package build (container: ${CONTAINER})"
    echo "============================================================"

    distrobox create \
        --name "${CONTAINER}" \
        --image fedora:latest \
        --yes \
        --volume "${PROJECT_DIR}:/project:ro" \
        --volume "${OUTPUT_DIR}:/output"

    run_in_container "${CONTAINER}" \
        sh /project/packaging/scripts/build-rpm-inside.sh \
            "${VERSION}" /output "${REPO_URL}"

    teardown "${CONTAINER}"
}

# ── Arch ──────────────────────────────────────────────────────────────────────

build_arch() {
    CONTAINER="xepher-build-arch"
    echo ""
    echo "============================================================"
    echo " Arch Linux package build (container: ${CONTAINER})"
    echo "============================================================"

    distrobox create \
        --name "${CONTAINER}" \
        --image archlinux:latest \
        --yes \
        --volume "${PROJECT_DIR}:/project:ro" \
        --volume "${OUTPUT_DIR}:/output"

    run_in_container "${CONTAINER}" \
        sh /project/packaging/scripts/build-arch-inside.sh \
            "${VERSION}" /output

    teardown "${CONTAINER}"
}

# ── main ──────────────────────────────────────────────────────────────────────

echo "Building xepher ${VERSION} packages"
echo "Output directory: ${OUTPUT_DIR}"
echo ""

FAILED=""

if [ "${BUILD_DEBIAN}" -eq 1 ]; then
    build_debian || { echo "ERROR: Debian build failed"; FAILED="${FAILED} debian"; }
fi

if [ "${BUILD_FEDORA}" -eq 1 ]; then
    build_fedora || { echo "ERROR: Fedora build failed"; FAILED="${FAILED} fedora"; }
fi

if [ "${BUILD_ARCH}" -eq 1 ]; then
    build_arch || { echo "ERROR: Arch build failed"; FAILED="${FAILED} arch"; }
fi

echo ""
echo "============================================================"
echo " Build summary"
echo "============================================================"
if [ -n "${FAILED}" ]; then
    echo "FAILED:${FAILED}"
    exit 1
else
    echo "All builds succeeded."
    echo ""
    ls -lh "${OUTPUT_DIR}/" | grep -E '\.(deb|rpm|zst)$' || echo "(no packages found in output dir)"
fi
