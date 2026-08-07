find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(STROPH REQUIRED IMPORTED_TARGET libstrophe)
# sexp/driver.hh includes <strophe.h>; pkg-config -I is required on BSD where
# headers live under /usr/local/include (not a default compiler search path).
# LibXml2: required for strophe_compat.hh fallback when libstrophe < 0.10.
target_link_libraries(xepher_sexp PRIVATE PkgConfig::STROPH LibXml2::LibXml2)

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

add_library(Xepher::deps ALIAS xepher_deps)
# libstrophe 0.10+ provides xmpp_stanza_new_from_string / xmpp_stanza_add_child_ex.
# Older packages (some Slackware builds) need compile-time shims in strophe_compat.hh.
include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES ${STROPH_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${STROPH_LIBRARIES})
check_symbol_exists(xmpp_stanza_new_from_string "strophe.h" XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING)
check_symbol_exists(xmpp_stanza_add_child_ex "strophe.h" XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

if(NOT XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING)
    message(STATUS "libstrophe: xmpp_stanza_new_from_string missing — using libxml2 fallback")
endif()
if(NOT XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX)
    message(STATUS "libstrophe: xmpp_stanza_add_child_ex missing — using add_child shim")
endif()

# Config header for strophe_compat.hh (0/1 macros).
set(_xepher_strophe_new_from_string 0)
set(_xepher_strophe_add_child_ex 0)
if(XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING)
    set(_xepher_strophe_new_from_string 1)
endif()
if(XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX)
    set(_xepher_strophe_add_child_ex 1)
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/strophe_compat_config.hh.in"
    "${CMAKE_BINARY_DIR}/generated/strophe_compat_config.hh"
    @ONLY
)
target_include_directories(xepher_deps INTERFACE "${CMAKE_BINARY_DIR}/generated")
# sexp also needs the generated config + strophe for driver.cpp
target_include_directories(xepher_sexp PRIVATE "${CMAKE_BINARY_DIR}/generated" "${CMAKE_SOURCE_DIR}/src")
