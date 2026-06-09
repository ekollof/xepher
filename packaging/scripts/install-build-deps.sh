# install-build-deps.sh — one-time dependency bootstrap inside persistent build containers.
# POSIX sh — compatible with OpenBSD pdksh (oksh).
# Sourced by build-*-inside.sh scripts.  Uses a stamp under /opt/xepher-build so it
# survives across runs and is visible whether the build runs as user or via sudo.
#
# Usage:
#   DEPS_STAMP=/opt/xepher-build/debian-deps.stamp
#   . /project/packaging/scripts/install-build-deps.sh
#   xepher_install_build_deps_once xepher_install_<distro>_deps

. /project/packaging/scripts/ci-helpers.sh

xepher_install_build_deps_once() {
    INSTALLER="$1"
    if [ -z "${DEPS_STAMP}" ] || [ -z "${INSTALLER}" ]; then
        echo "xepher_install_build_deps_once: missing DEPS_STAMP or installer" >&2
        return 1
    fi
    if [ -f "${DEPS_STAMP}" ]; then
        echo ">>> Build dependencies already installed (${DEPS_STAMP}) — skipping"
        return 0
    fi
    echo ">>> Installing build dependencies (first run in this container)..."
    "${INSTALLER}"
    xepher_as_root mkdir -p "$(dirname "${DEPS_STAMP}")"
    xepher_as_root touch "${DEPS_STAMP}"
    xepher_as_root chmod 644 "${DEPS_STAMP}"
}