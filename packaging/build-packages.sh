#!/bin/sh
# Build distribution packages for Xepher

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VERSION="${1:-0.2.0}"

echo "Building packages for Xepher version $VERSION"
echo "Project directory: $PROJECT_DIR"
echo ""

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
else
    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
fi

echo "Detected OS: $OS"
echo ""

build_deb() {
    echo "=== Building Debian/Ubuntu package ==="
    
    # Install build dependencies
    sudo apt-get update
    sudo apt-get install -y debhelper devscripts build-essential
    
    # Create build directory
    BUILD_DIR=$(mktemp -d)
    trap "rm -rf $BUILD_DIR" EXIT
    
    # Copy source
    cp -r "$PROJECT_DIR" "$BUILD_DIR/xepher-$VERSION"
    cd "$BUILD_DIR/xepher-$VERSION"
    
    # Copy debian packaging files
    cp -r "$PROJECT_DIR/packaging/debian" ./debian
    
    # Build package
    debuild -us -uc -b
    
    # Copy result
    mkdir -p "$PROJECT_DIR/packaging/build"
    cp "$BUILD_DIR"/*.deb "$PROJECT_DIR/packaging/build/"
    
    echo ""
    echo "Debian package built: $(ls $PROJECT_DIR/packaging/build/*.deb)"
}

build_rpm() {
    echo "=== Building RPM package ==="
    
    # Install build dependencies
    sudo dnf install -y rpm-build rpmdevtools
    
    # Setup RPM build tree
    rpmdev-setuptree
    
    # Create source tarball
    cd "$PROJECT_DIR/.."
    tar czf "$HOME/rpmbuild/SOURCES/xepher-$VERSION.tar.gz" \
        --transform "s|^xepher|xepher-$VERSION|" \
        xepher/
    
    # Copy spec file
    cp "$PROJECT_DIR/packaging/rpm/weechat-xmpp.spec" "$HOME/rpmbuild/SPECS/xepher.spec"
    
    # Build RPM
    cd "$HOME/rpmbuild/SPECS"
    rpmbuild -bb xepher.spec
    
    # Copy result
    mkdir -p "$PROJECT_DIR/packaging/build"
    cp "$HOME/rpmbuild/RPMS"/*/*.rpm "$PROJECT_DIR/packaging/build/"
    
    echo ""
    echo "RPM package built: $(ls $PROJECT_DIR/packaging/build/*.rpm)"
}

build_arch() {
    echo "=== Building Arch Linux package ==="
    
    # Install build dependencies
    sudo pacman -Sy --needed --noconfirm base-devel git
    
    # Create build directory
    BUILD_DIR=$(mktemp -d)
    trap "rm -rf $BUILD_DIR" EXIT
    
    # Copy PKGBUILD
    cp "$PROJECT_DIR/packaging/arch/PKGBUILD" "$BUILD_DIR/"
    cd "$BUILD_DIR"
    
    # Build package
    makepkg -sf
    
    # Copy result
    mkdir -p "$PROJECT_DIR/packaging/build"
    cp *.pkg.tar.zst "$PROJECT_DIR/packaging/build/"
    
    echo ""
    echo "Arch package built: $(ls $PROJECT_DIR/packaging/build/*.pkg.tar.zst)"
}

# Build based on OS
case "$OS" in
    ubuntu|debian|linuxmint|pop)
        build_deb
        ;;
    
    fedora|rhel|centos|rocky|almalinux)
        build_rpm
        ;;
    
    arch|manjaro)
        build_arch
        ;;
    
    *)
        echo "Package building not yet automated for $OS"
        echo ""
        echo "Available packaging files:"
        echo "  Debian/Ubuntu: packaging/debian/"
        echo "  RPM (Fedora/RHEL): packaging/rpm/weechat-xmpp.spec"
        echo "  Arch Linux: packaging/arch/PKGBUILD"
        exit 1
        ;;
esac

echo ""
echo "=== Package build complete ==="
echo "Output directory: $PROJECT_DIR/packaging/build/"
