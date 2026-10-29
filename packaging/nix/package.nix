{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  bison,
  flex,
  weechat-unwrapped,
  libstrophe,
  libxml2,
  lmdb,
  libsignal-protocol-c,
  libomemo-c,
  gpgme,
  libgcrypt,
  fmt,
  curl,
  openssl,
  src,
}:

stdenv.mkDerivation {
  pname = "xepher";
  version = "1.1.0";

  inherit src;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    bison
    flex
  ];

  buildInputs = [
    weechat-unwrapped
    libstrophe
    libxml2
    lmdb
    libsignal-protocol-c
    libomemo-c
    gpgme
    libgcrypt
    fmt
    curl
    openssl
  ];

  # The makefile keeps CC/CXX from the environment and drives CMake.
  buildPhase = ''
    runHook preBuild
    make PACKAGE_BUILD=1 weechat-xmpp
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm644 xmpp.so "$out/lib/weechat/plugins/xmpp.so"
    install -Dm644 scripts/icat.py "$out/share/weechat/python/icat.py"
    install -Dm644 scripts/feed_compose.py "$out/share/weechat/python/feed_compose.py"
    install -Dm644 README.md "$out/share/doc/xepher/README.md"
    install -Dm644 LICENSE "$out/share/licenses/xepher/LICENSE"
    runHook postInstall
  '';

  meta = {
    description = "WeeChat plugin for XMPP";
    homepage = "https://ekollof.github.io/xepher/";
    license = lib.licenses.mpl20;
    platforms = lib.platforms.linux;
  };
}
