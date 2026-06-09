# ci-helpers.sh — small helpers shared by container build scripts (local + GitHub Actions).
# POSIX sh — compatible with OpenBSD pdksh (oksh).
# Source with:  . /project/packaging/scripts/ci-helpers.sh

xepher_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}