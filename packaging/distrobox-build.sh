#!/bin/sh
# distrobox-build.sh — Build xepher packages for Debian, Fedora, Arch, Alpine, and Void Linux
# using persistent distrobox containers (packages stay installed between runs).
#
# Usage:
#   ./packaging/distrobox-build.sh [version] [--debian] [--fedora] [--arch] [--alpine] [--void]
#                                   [--teardown] [--recreate]
#
#   version    : package version to build (default: 0.3.0)
#   --debian   : build only the Debian package
#   --fedora   : build only the Fedora/RPM package
#   --arch     : build only the Arch package
#   --alpine   : build only the Alpine package
#   --void     : build only the Void Linux package
#   --teardown : remove build containers after a successful run (old ephemeral behaviour)
#   --recreate : remove and recreate build containers before building (refresh base image/deps)
#   (no flags  : build all five)
#
# Containers are kept by default:
#   xepher-build-debian, xepher-build-fedora, xepher-build-arch,
#   xepher-build-alpine, xepher-build-void
#
# Prerequisites: distrobox + docker (or podman) installed and accessible.
#
# For CI / hosts without distrobox, use packaging/github-build.sh instead (plain Docker).
#
# Output: packaging/build/*.deb, *.rpm, *.pkg.tar.zst, *.apk, *.xbps

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VERSION="0.3.0"
OUTPUT_DIR="${PROJECT_DIR}/packaging/build"
REPO_URL="https://github.com/ekollof/xepher.git"

BUILD_DEBIAN=0
BUILD_FEDORA=0
BUILD_ARCH=0
BUILD_ALPINE=0
BUILD_VOID=0
TEARDOWN=0
RECREATE=0

for arg in "$@"; do
    case "$arg" in
        --debian)   BUILD_DEBIAN=1 ;;
        --fedora)   BUILD_FEDORA=1 ;;
        --arch)     BUILD_ARCH=1 ;;
        --alpine)   BUILD_ALPINE=1 ;;
        --void)     BUILD_VOID=1 ;;
        --teardown) TEARDOWN=1 ;;
        --recreate) RECREATE=1 ;;
        [0-9]*)     VERSION="$arg" ;;
        *)          echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

if [ $((BUILD_DEBIAN + BUILD_FEDORA + BUILD_ARCH + BUILD_ALPINE + BUILD_VOID)) -eq 0 ]; then
    BUILD_DEBIAN=1
    BUILD_FEDORA=1
    BUILD_ARCH=1
    BUILD_ALPINE=1
    BUILD_VOID=1
fi

mkdir -p "${OUTPUT_DIR}"

# ── helpers ──────────────────────────────────────────────────────────────────

container_exists() {
    distrobox list 2>/dev/null | grep -qE "(^|[[:space:]])${1}([[:space:]]|$)"
}

teardown_container() {
    CONTAINER="$1"
    if container_exists "${CONTAINER}"; then
        echo ">>> Removing container: ${CONTAINER}"
        distrobox rm --force "${CONTAINER}" 2>/dev/null || true
    fi
}

# Create a persistent distrobox if missing.  Volumes are fixed at create time.
ensure_container() {
    CONTAINER="$1"
    IMAGE="$2"

    if [ "${RECREATE}" -eq 1 ]; then
        teardown_container "${CONTAINER}"
    fi

    if container_exists "${CONTAINER}"; then
        echo ">>> Reusing persistent container: ${CONTAINER}"
        return 0
    fi

    echo ">>> Creating persistent container: ${CONTAINER} (${IMAGE})"
    distrobox create \
        --name "${CONTAINER}" \
        --image "${IMAGE}" \
        --yes \
        --volume "${PROJECT_DIR}:/project:ro" \
        --volume "${OUTPUT_DIR}:/output"
}

run_in_container() {
    distrobox enter --name "$1" -- "${@:2}"
}

# Alpine/Void package scripts mutate system accounts/repos and need root.
run_in_container_privileged() {
    distrobox enter --name "$1" -- sudo sh "${@:2}"
}

maybe_teardown() {
    CONTAINER="$1"
    if [ "${TEARDOWN}" -eq 1 ]; then
        teardown_container "${CONTAINER}"
    fi
}

# ── Debian ────────────────────────────────────────────────────────────────────

build_debian() {
    CONTAINER="xepher-build-debian"
    echo ""
    echo "============================================================"
    echo " Debian package build (container: ${CONTAINER})"
    echo "============================================================"

    ensure_container "${CONTAINER}" debian:stable
    run_in_container "${CONTAINER}" \
        sh /project/packaging/scripts/build-deb-inside.sh \
            "${VERSION}" /output "${REPO_URL}"
    maybe_teardown "${CONTAINER}"
}

# ── Fedora ────────────────────────────────────────────────────────────────────

build_fedora() {
    CONTAINER="xepher-build-fedora"
    echo ""
    echo "============================================================"
    echo " Fedora/RPM package build (container: ${CONTAINER})"
    echo "============================================================"

    ensure_container "${CONTAINER}" fedora:latest
    run_in_container "${CONTAINER}" \
        sh /project/packaging/scripts/build-rpm-inside.sh \
            "${VERSION}" /output
    maybe_teardown "${CONTAINER}"
}

# ── Arch ──────────────────────────────────────────────────────────────────────

build_arch() {
    CONTAINER="xepher-build-arch"
    echo ""
    echo "============================================================"
    echo " Arch Linux package build (container: ${CONTAINER})"
    echo "============================================================"

    ensure_container "${CONTAINER}" archlinux:latest
    run_in_container "${CONTAINER}" \
        sh /project/packaging/scripts/build-arch-inside.sh \
            "${VERSION}" /output
    maybe_teardown "${CONTAINER}"
}

# ── Alpine ────────────────────────────────────────────────────────────────────

build_alpine() {
    CONTAINER="xepher-build-alpine"
    echo ""
    echo "============================================================"
    echo " Alpine Linux package build (container: ${CONTAINER})"
    echo "============================================================"

    ensure_container "${CONTAINER}" alpine:edge
    run_in_container_privileged "${CONTAINER}" \
        /project/packaging/scripts/build-alpine-inside.sh \
            "${VERSION}" /output
    maybe_teardown "${CONTAINER}"
}

# ── Void ──────────────────────────────────────────────────────────────────────

build_void() {
    CONTAINER="xepher-build-void"
    echo ""
    echo "============================================================"
    echo " Void Linux package build (container: ${CONTAINER})"
    echo "============================================================"

    ensure_container "${CONTAINER}" ghcr.io/void-linux/void-linux:latest-full-x86_64
    run_in_container_privileged "${CONTAINER}" \
        /project/packaging/scripts/build-void-inside.sh \
            "${VERSION}" /output
    maybe_teardown "${CONTAINER}"
}

# ── main ──────────────────────────────────────────────────────────────────────

echo "Building xepher ${VERSION} packages"
echo "Output directory: ${OUTPUT_DIR}"
if [ "${TEARDOWN}" -eq 1 ]; then
    echo "Mode: ephemeral (containers removed after build)"
elif [ "${RECREATE}" -eq 1 ]; then
    echo "Mode: recreate persistent containers, then build"
else
    echo "Mode: persistent containers (reuse installed packages)"
fi
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
else
    echo "All builds succeeded."
    echo ""
    ls -lh "${OUTPUT_DIR}/" | grep -E '\.(deb|rpm|zst|apk|xbps)$' || echo "(no packages found in output dir)"
fi