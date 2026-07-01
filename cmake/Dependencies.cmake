find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(STROPH REQUIRED IMPORTED_TARGET libstrophe)
pkg_check_modules(GPGME REQUIRED IMPORTED_TARGET gpgme)
pkg_check_modules(OMEMO REQUIRED IMPORTED_TARGET libomemo-c)
pkg_check_modules(SIGNAL REQUIRED IMPORTED_TARGET libsignal-protocol-c)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

find_library(XEPHER_LMDB NAMES lmdb REQUIRED)
find_library(XEPHER_GCRYPT NAMES gcrypt REQUIRED)
find_library(XEPHER_FMT NAMES fmt REQUIRED)

add_library(xepher_deps INTERFACE)
target_link_libraries(xepher_deps INTERFACE
    Xepher::sexp
    "${XEPHER_LIBDIFF_A}"
    PkgConfig::STROPH
    PkgConfig::GPGME
    PkgConfig::OMEMO
    PkgConfig::SIGNAL
    PkgConfig::CURL
    OpenSSL::SSL
    OpenSSL::Crypto
    LibXml2::LibXml2
    Threads::Threads
    ${XEPHER_LMDB}
    ${XEPHER_GCRYPT}
    ${XEPHER_FMT}
)

target_include_directories(xepher_deps INTERFACE
    "${CMAKE_SOURCE_DIR}/deps/lmdbxx"
    "${CMAKE_SOURCE_DIR}/deps"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}"
)

if(EXISTS "${CMAKE_SOURCE_DIR}/libstrophe")
    target_include_directories(xepher_deps INTERFACE "${CMAKE_SOURCE_DIR}/libstrophe")
endif()

add_dependencies(xepher_deps xepher_libdiff)
add_library(Xepher::deps ALIAS xepher_deps)