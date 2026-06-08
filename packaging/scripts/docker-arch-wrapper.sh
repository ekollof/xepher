#!/bin/sh
# Runs INSIDE an Arch Linux docker container as root.
# Creates a non-root builder account (makepkg refuses root) and delegates to
# build-arch-inside.sh.  Used by packaging/github-build.sh.

set -e

VERSION="${1:?version required}"
OUTPUT_DIR="${2:-/output}"

pacman -Sy --needed --noconfirm base-devel git sudo

if ! id builder >/dev/null 2>&1; then
    useradd -m -G wheel builder
fi
if ! grep -q '^%wheel ALL=(ALL) NOPASSWD: ALL' /etc/sudoers 2>/dev/null; then
    echo '%wheel ALL=(ALL) NOPASSWD: ALL' >> /etc/sudoers
fi

su builder -c "sh /project/packaging/scripts/build-arch-inside.sh ${VERSION} ${OUTPUT_DIR}"