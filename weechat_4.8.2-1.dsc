Format: 1.0
Source: weechat
Binary: weechat, weechat-curses, weechat-headless, weechat-core, weechat-plugins, weechat-python, weechat-perl, weechat-ruby, weechat-lua, weechat-tcl, weechat-guile, weechat-php, weechat-doc, weechat-dev
Architecture: any all
Version: 4.8.2-1
Maintainer: Emmanuel Bouthenot <kolter@debian.org>
Homepage: https://weechat.org/
Standards-Version: 4.7.2
Vcs-Browser: https://salsa.debian.org/kolter/weechat
Vcs-Git: https://salsa.debian.org/kolter/weechat.git
Build-Depends: asciidoctor (>= 1.5.4), ruby-pygments.rb, debhelper (>= 12), cmake, pkgconf, libncurses-dev, gem2deb, libperl-dev, python3-dev, libaspell-dev, liblua5.3-dev, tcl8.6-dev, guile-3.0-dev, php-dev, libphp-embed, libargon2-dev, libsodium-dev, libxml2-dev, libcurl4-gnutls-dev, libgcrypt20-dev, libgnutls28-dev, libzstd-dev, zlib1g-dev, libcjson-dev
Package-List:
 weechat deb net optional arch=all
 weechat-core deb net optional arch=any
 weechat-curses deb net optional arch=any
 weechat-dev deb devel optional arch=any
 weechat-doc deb doc optional arch=all
 weechat-guile deb net optional arch=any
 weechat-headless deb net optional arch=any
 weechat-lua deb net optional arch=any
 weechat-perl deb net optional arch=any
 weechat-php deb net optional arch=any
 weechat-plugins deb net optional arch=any
 weechat-python deb net optional arch=any
 weechat-ruby deb net optional arch=any
 weechat-tcl deb net optional arch=any
Checksums-Sha1:
 1924c07e81ed7fc9f5beb332ff2adfc31ca8e4cb 5482840 weechat_4.8.2-1.tar.gz
Checksums-Sha256:
 4496b9a7e38c4e64263ac5b557a09d6ad74a0edba1336f1d5353313f6862e557 5482840 weechat_4.8.2-1.tar.gz
Files:
 21636effc81cf27fafa74f24735f26af 5482840 weechat_4.8.2-1.tar.gz
