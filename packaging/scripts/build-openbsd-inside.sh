#!/bin/sh
# Native OpenBSD package build via the port skeleton. Run on OpenBSD with ports(7) deps.
#
# Usage: packaging/scripts/build-openbsd-inside.sh [version] [output_dir]
#
# Copies packaging/openbsd into a temporary ports tree and runs make package.

set -e

VERSION="${1:-0.8.1}"
OUTPUT_DIR="${2:-/output}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)

case "$(uname -s)" in
OpenBSD) ;;
*)
    echo "build-openbsd-inside.sh must run on OpenBSD" >&2
    exit 1
    ;;
esac

echo "=== [OpenBSD] Building xepher ${VERSION} ==="

BUILD_DIR=$(mktemp -d)
trap 'rm -rf "${BUILD_DIR}"' EXIT

PORT_DIR="${BUILD_DIR}/ports/net/xepher"
mkdir -p "${PORT_DIR}/pkg"
cp "${PROJECT_DIR}/packaging/openbsd/Makefile" "${PORT_DIR}/"
cp "${PROJECT_DIR}/packaging/openbsd/pkg/"* "${PORT_DIR}/pkg/"

# Allow overriding the version baked into the skeleton Makefile.
if [ "${VERSION}" != "0.8.1" ]; then
    sed -i "s/^V =.*/V =		${VERSION}/" "${PORT_DIR}/Makefile"
fi

export PORTSDIR="${BUILD_DIR}/ports"
cd "${PORT_DIR}"
make package

ARCH=$(machine -a)
PKG_GLOB="${PORTSDIR}/packages/${ARCH}/all/xepher-${VERSION}*.tgz"
mkdir -p "${OUTPUT_DIR}"
cp ${PKG_GLOB} "${OUTPUT_DIR}/"

echo "=== [OpenBSD] Done. Packages in ${OUTPUT_DIR} ==="
ls -lh "${OUTPUT_DIR}/"xepher-*.tgz