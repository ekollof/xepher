#!/bin/sh
# Native FreeBSD binary package build. Run on a FreeBSD host with build deps installed.
# POSIX sh — compatible with OpenBSD pdksh (oksh).
#
# Usage: packaging/scripts/build-freebsd-inside.sh [version] [output_dir]
#
# Produces: xepher-<version>.pkg in output_dir (via pkg create).

set -e

VERSION="${1:-0.8.1}"
OUTPUT_DIR="${2:-/output}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "${SCRIPT_DIR}/../.." && pwd)

case "$(uname -s)" in
FreeBSD) ;;
*)
    echo "build-freebsd-inside.sh must run on FreeBSD" >&2
    exit 1
    ;;
esac

echo "=== [FreeBSD] Building xepher ${VERSION} ==="

BUILD_DIR=$(mktemp -d)
trap 'rm -rf "${BUILD_DIR}"' EXIT

SRC="${BUILD_DIR}/src"
DESTDIR="${BUILD_DIR}/destdir"
PLIST="${BUILD_DIR}/plist"

cp -a "${PROJECT_DIR}/." "${SRC}/"
make -C "${SRC}" clean
make -C "${SRC}" deps/diff/libdiff.a
make -C "${SRC}" PACKAGE_BUILD=1 weechat-xmpp

install -d "${DESTDIR}/usr/local/lib/weechat/plugins"
install -d "${DESTDIR}/usr/local/share/doc/xepher"
install -d "${DESTDIR}/usr/local/share/licenses/xepher"
install -m 755 "${SRC}/xmpp.so" "${DESTDIR}/usr/local/lib/weechat/plugins/xmpp.so"
install -m 644 "${SRC}/README.md" "${DESTDIR}/usr/local/share/doc/xepher/README.md"
install -m 644 "${SRC}/LICENSE" "${DESTDIR}/usr/local/share/licenses/xepher/LICENSE"

cat > "${PLIST}" << 'EOF'
/usr/local/lib/weechat/plugins/xmpp.so
/usr/local/share/doc/xepher/README.md
/usr/local/share/licenses/xepher/LICENSE
EOF

mkdir -p "${OUTPUT_DIR}"
pkg create -r "${DESTDIR}" -p "${PLIST}" -o "${OUTPUT_DIR}" -n "xepher-${VERSION}"

echo "=== [FreeBSD] Done. Packages in ${OUTPUT_DIR} ==="
ls -lh "${OUTPUT_DIR}/"xepher-*.pkg 2>/dev/null || ls -lh "${OUTPUT_DIR}/"