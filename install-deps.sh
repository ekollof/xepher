#!/bin/sh
# Install dependencies for weechat-xmpp.
# POSIX sh — compatible with OpenBSD pdksh (oksh).

set -e

# Detect OS and distro
if [ -f /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    OS=$ID
else
    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
fi

echo "Detected OS: $OS"

# Common packages across distros
# Runtime deps: libstrophe, libxml2, lmdb, libomemo-c (libsignal-protocol-c), gpgme, libfmt, libcurl, libssl/libcrypto (OpenSSL)
# Build deps: clang/clang++, cmake (>= 3.22), ninja, bison, flex, git

case "$OS" in
    ubuntu|debian|linuxmint|pop)
        echo "Installing dependencies for Debian/Ubuntu-based system..."
        sudo apt-get update
        sudo apt-get install -y \
            build-essential \
            clang \
            cmake \
            ninja-build \
            git \
            bison \
            flex \
            libstrophe-dev \
            libxml2-dev \
            liblmdb-dev \
            libsignal-protocol-c-dev \
            libgpgme-dev \
            libfmt-dev \
            libcurl4-openssl-dev \
            libssl-dev \
            weechat \
            weechat-dev
        ;;
    
    fedora|rhel|centos|rocky|almalinux)
        echo "Installing dependencies for Fedora/RHEL-based system..."
        sudo dnf install -y \
            clang \
            cmake \
            ninja-build \
            git \
            bison \
            flex \
            libstrophe-devel \
            libxml2-devel \
            lmdb-devel \
            libsignal-protocol-c-devel \
            gpgme-devel \
            fmt-devel \
            libcurl-devel \
            openssl-devel \
            weechat \
            weechat-devel
        ;;
    
    arch|manjaro)
        echo "Installing dependencies for Arch-based system..."
        sudo pacman -Sy --needed --noconfirm \
            clang \
            cmake \
            ninja \
            git \
            bison \
            flex \
            libstrophe \
            libxml2 \
            lmdb \
            libsignal-protocol-c \
            gpgme \
            fmt \
            curl \
            openssl \
            weechat
        ;;
    
    opensuse*|suse)
        echo "Installing dependencies for openSUSE..."
        sudo zypper install -y \
            clang \
            git \
            bison \
            flex \
            libstrophe-devel \
            libxml2-devel \
            lmdb-devel \
            libsignal-protocol-c-devel \
            gpgme-devel \
            fmt-devel \
            libcurl-devel \
            libopenssl-devel \
            weechat \
            weechat-devel
        ;;
    
    void)
        echo "Installing dependencies for Void Linux..."
        sudo xbps-install -Sy \
            clang \
            cmake \
            ninja \
            git \
            bison \
            flex \
            libstrophe-devel \
            libxml2-devel \
            lmdb-devel \
            libsignal-protocol-c-devel \
            gpgme-devel \
            fmt-devel \
            libcurl-devel \
            openssl-devel \
            weechat
        ;;
    
    alpine)
        echo "Installing dependencies for Alpine Linux..."
        sudo apk add --no-cache \
            clang \
            cmake \
            ninja \
            git \
            bison \
            flex \
            libstrophe-dev \
            libxml2-dev \
            lmdb-dev \
            libsignal-protocol-c-dev \
            gpgme-dev \
            fmt-dev \
            curl-dev \
            openssl-dev \
            weechat
        ;;
    
    gentoo)
        echo "Installing dependencies for Gentoo..."
        sudo emerge --ask=n \
            llvm-core/clang \
            dev-vcs/git \
            sys-devel/bison \
            sys-devel/flex \
            net-libs/libstrophe \
            dev-libs/libxml2 \
            dev-db/lmdb \
            net-libs/libsignal-protocol-c \
            app-crypt/gpgme \
            dev-libs/libfmt \
            net-misc/curl \
            dev-libs/openssl \
            net-im/weechat
        ;;
    
    freebsd)
        echo "Installing dependencies for FreeBSD..."
        sudo pkg install -y \
            llvm \
            cmake \
            ninja \
            gmake \
            git \
            bison \
            flex \
            libstrophe \
            libxml2 \
            lmdb \
            libsignal-protocol-c \
            libomemo-c \
            gpgme \
            libfmt \
            curl \
            openssl \
            weechat
        ;;

    openbsd)
        echo "Installing dependencies for OpenBSD..."
        doas pkg_add \
            cmake \
            ninja \
            gmake \
            git \
            bison \
            flex \
            libstrophe \
            libxml \
            lmdb \
            libsignal-protocol-c \
            libomemo-c \
            fmt \
            gpgme \
            curl \
            weechat
        ;;

    netbsd)
        echo "Installing dependencies for NetBSD..."
        sudo pkgin install \
            clang \
            gmake \
            git \
            bison \
            flex \
            libstrophe \
            libxml2 \
            lmdb \
            gpgme \
            libfmt \
            curl \
            openssl \
            weechat
        echo "Note: libsignal-protocol-c may need to be built from source on NetBSD."
        ;;

    darwin)
        echo "Installing dependencies for macOS (Homebrew)..."
        brew install \
            llvm \
            cmake \
            ninja \
            make \
            bison \
            flex \
            libstrophe \
            libxml2 \
            lmdb \
            gpgme \
            fmt \
            curl \
            openssl \
            weechat
        echo ""
        echo "Note: libsignal-protocol-c and libomemo-c are not in Homebrew core."
        echo "      Build them from source:"
        echo "        https://github.com/signalapp/libsignal-protocol-c"
        echo "        https://github.com/gkdr/libomemo-c"
        echo ""
        echo "After building and installing both libraries, run:"
        echo "  gmake (or: make -f makefile)"
        ;;

    *)
        echo "Unknown or unsupported distribution: $OS"
        echo ""
        echo "Please install the following packages manually:"
        echo "  Runtime: libstrophe, libxml2, lmdb, libsignal-protocol-c, gpgme, libfmt, libcurl, openssl, weechat"
        echo "  Build: clang/clang++ (>= 14), cmake (>= 3.22), ninja, bison, flex, git"
        exit 1
        ;;
esac

echo ""
echo "Dependencies installed successfully!"
echo ""
echo "Next steps:"
echo "  1. Initialize git submodules: git submodule update --init --recursive"
echo "  2. Build the plugin: make"
echo "  3. Install to WeeChat: make install"
