# prepare-source-tree.sh — shared helper for distrobox/docker build scripts.
# POSIX sh — compatible with OpenBSD pdksh (oksh).
#
# Copies the read-only /project mount into a writable destination and removes
# host build artifacts so each container compiles fresh.  Pre-builds deps/diff
# while .git is still available — RPM/Arch/Alpine tarballs omit .git, but the
# packaging flow seeds libdiff.a before stripping VCS metadata.
#
# Source with:  . /project/packaging/scripts/prepare-source-tree.sh

prepare_source_tree() {
    DEST="$1"
    if [ -z "$DEST" ]; then
        echo "prepare_source_tree: missing destination directory" >&2
        return 1
    fi

    mkdir -p "$DEST"
    cp -a /project/. "$DEST/"

    if [ -d "$DEST/.git" ]; then
        # Docker/distrobox copies are often owned by a different uid than the
        # container user — mark the tree safe before submodule/git makefile steps.
        git config --global --add safe.directory "$DEST"
        git -C "$DEST" submodule update --init --recursive
    fi

    if [ -f "$DEST/makefile" ] || [ -f "$DEST/Makefile" ]; then
        make -C "$DEST" clean
        # Seed libdiff.a before tarballs drop .git (see makefile seed-libdiff).
        make -C "$DEST" seed-libdiff
    else
        rm -f "$DEST/xmpp.so"
        rm -rf "$DEST/obj" "$DEST/build"
    fi
}