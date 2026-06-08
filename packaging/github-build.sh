#!/bin/sh
# github-build.sh — Build xepher packages for all distros sequentially using Docker.
# Intended for GitHub Actions (ubuntu-latest) and any host with Docker.
#
# Usage:
#   ./packaging/github-build.sh <version> [--debian] [--fedora] [--arch] [--alpine] [--void]
#
# Each distro runs in a fresh container via the existing build-*-inside.sh scripts.
# Builds are sequential (no parallelism) to limit CI memory/CPU pressure.
#
# Output: packaging/build/*.deb, *.rpm, *.pkg.tar.zst, *.apk, *.xbps

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${PROJECT_DIR}/packaging/build"

VERSION=""
BUILD_DEBIAN=0
BUILD_FEDORA=0
BUILD_ARCH=0
BUILD_ALPINE=0
BUILD_VOID=0

for arg in "$@"; do
    case "$arg" in
        --debian) BUILD_DEBIAN=1 ;;
        --fedora) BUILD_FEDORA=1 ;;
        --arch)   BUILD_ARCH=1 ;;
        --alpine) BUILD_ALPINE=1 ;;
        --void)   BUILD_VOID=1 ;;
        [0-9]*)   VERSION="$arg" ;;
        *)        echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

if [ -z "${VERSION}" ]; then
    echo "Usage: $0 <version> [--debian] [--fedora] [--arch] [--alpine] [--void]" >&2
    exit 1
fi

if [ $((BUILD_DEBIAN + BUILD_FEDORA + BUILD_ARCH + BUILD_ALPINE + BUILD_VOID)) -eq 0 ]; then
    BUILD_DEBIAN=1
    BUILD_FEDORA=1
    BUILD_ARCH=1
    BUILD_ALPINE=1
    BUILD_VOID=1
fi

mkdir -p "${OUTPUT_DIR}"

docker_run() {
    IMAGE="$1"
    shift
    docker run --rm \
        -v "${PROJECT_DIR}:/project:ro" \
        -v "${OUTPUT_DIR}:/output" \
        "${IMAGE}" \
        "$@"
}

build_debian() {
    echo ""
    echo "============================================================"
    echo " Debian package build (docker: debian:stable)"
    echo "============================================================"
    docker_run debian:stable \
        sh /project/packaging/scripts/build-deb-inside.sh "${VERSION}" /output
}

build_fedora() {
    echo ""
    echo "============================================================"
    echo " Fedora/RPM package build (docker: fedora:latest)"
    echo "============================================================"
    docker_run fedora:latest \
        sh /project/packaging/scripts/build-rpm-inside.sh "${VERSION}" /output
}

build_arch() {
    echo ""
    echo "============================================================"
    echo " Arch Linux package build (docker: archlinux:latest)"
    echo "============================================================"
    docker_run archlinux:latest \
        sh /project/packaging/scripts/docker-arch-wrapper.sh "${VERSION}" /output
}

build_alpine() {
    echo ""
    echo "============================================================"
    echo " Alpine Linux package build (docker: alpine:edge)"
    echo "============================================================"
    docker_run alpine:edge \
        sh /project/packaging/scripts/build-alpine-inside.sh "${VERSION}" /output
}

build_void() {
    echo ""
    echo "============================================================"
    echo " Void Linux package build (docker: void-linux latest-full)"
    echo "============================================================"
    docker_run ghcr.io/void-linux/void-linux:latest-full-x86_64 \
        sh /project/packaging/scripts/build-void-inside.sh "${VERSION}" /output
}

echo "Building xepher ${VERSION} packages (sequential Docker builds)"
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
if [ "${BUILD_ALPINE}" -eq 1 ]; then
    build_alpine || { echo "ERROR: Alpine build failed"; FAILED="${FAILED} alpine"; }
fi
if [ "${BUILD_VOID}" -eq 1 ]; then
    build_void || { echo "ERROR: Void build failed"; FAILED="${FAILED} void"; }
fi

echo ""
echo "============================================================"
echo " Build summary"
echo "============================================================"
if [ -n "${FAILED}" ]; then
    echo "FAILED:${FAILED}"
    exit 1
fi

echo "All builds succeeded."
echo ""
ls -lh "${OUTPUT_DIR}/" | grep -E '\.(deb|rpm|zst|apk|xbps)$' || echo "(no packages found in output dir)"