find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(OpenSSL REQUIRED)

# Minimum libstrophe: 0.12.0
#   - 0.9+:  conn flags + keepalive
#   - 0.10+: xmpp_stanza_new_from_string / add_child_ex
#   - 0.11+: xmpp_ctx_set_verbosity
#   - 0.12+: XMPP_CONN_FLAG_DISABLE_SM (we disable libstrophe SM; use XEP-0198 ourselves)
# Slackware 15 stock packages are often ~0.8.x — too old; build/install libstrophe
# from source or a newer SlackBuild.
set(XEPHER_LIBSTROPHE_MIN_VERSION "0.12.0")
pkg_check_modules(STROPH REQUIRED IMPORTED_TARGET libstrophe)
if(STROPH_VERSION)
    if(STROPH_VERSION VERSION_LESS "${XEPHER_LIBSTROPHE_MIN_VERSION}")
        message(FATAL_ERROR
            "libstrophe ${STROPH_VERSION} is too old (need >= ${XEPHER_LIBSTROPHE_MIN_VERSION}).\n"
            "  Found: ${STROPH_LIBRARIES} (include: ${STROPH_INCLUDE_DIRS})\n"
            "  Xepher needs a modern libstrophe for connection flags, keepalive,\n"
            "  stream-management control, stanza builders, and TLS certfail hooks.\n"
            "  Upgrade the system package or build libstrophe from:\n"
            "    https://github.com/strophe/libstrophe/releases")
    endif()
    message(STATUS "libstrophe: ${STROPH_VERSION} (minimum ${XEPHER_LIBSTROPHE_MIN_VERSION})")
else()
    message(WARNING
        "libstrophe.pc has no Version field; cannot enforce "
        ">= ${XEPHER_LIBSTROPHE_MIN_VERSION}. Relying on symbol probes.")
endif()
# sexp/driver.hh includes <strophe.h>; pkg-config -I is required on BSD where
# headers live under /usr/local/include (not a default compiler search path).
# LibXml2: still used by strophe_compat.hh if a rare partial install lacks
# xmpp_stanza_new_from_string despite a modern version string.
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
# libstrophe feature probes — belt-and-suspenders after the version floor.
# Incomplete/custom builds can still miss symbols; strophe_compat.hh shims those.
include(CheckSymbolExists)
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${STROPH_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${STROPH_LIBRARIES})
check_symbol_exists(xmpp_stanza_new_from_string "strophe.h" XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING)
check_symbol_exists(xmpp_stanza_add_child_ex "strophe.h" XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX)
check_symbol_exists(xmpp_ctx_set_verbosity "strophe.h" XEPHER_HAVE_XMPP_CTX_SET_VERBOSITY)
check_symbol_exists(xmpp_conn_get_flags "strophe.h" XEPHER_HAVE_XMPP_CONN_GET_FLAGS)
check_symbol_exists(xmpp_conn_set_flags "strophe.h" XEPHER_HAVE_XMPP_CONN_SET_FLAGS)
check_symbol_exists(xmpp_conn_set_keepalive "strophe.h" XEPHER_HAVE_XMPP_CONN_SET_KEEPALIVE)
check_symbol_exists(xmpp_conn_disable_tls "strophe.h" XEPHER_HAVE_XMPP_CONN_DISABLE_TLS)
check_symbol_exists(xmpp_conn_set_certfail_handler "strophe.h" XEPHER_HAVE_XMPP_CONN_SET_CERTFAIL_HANDLER)
check_symbol_exists(xmpp_connect_raw "strophe.h" XEPHER_HAVE_XMPP_CONNECT_RAW)
# Flag macros appeared with the flags API (0.9+); probe DISABLE_SM separately
# (added later for stream-management disable).
check_c_source_compiles("
#include <strophe.h>
int main(void) { return (int)(XMPP_CONN_FLAG_DISABLE_TLS); }
" XEPHER_HAVE_XMPP_CONN_FLAG_MACROS)
check_c_source_compiles("
#include <strophe.h>
int main(void) { return (int)(XMPP_CONN_FLAG_DISABLE_SM); }
" XEPHER_HAVE_XMPP_CONN_FLAG_DISABLE_SM)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

function(_xepher_strophe_status have_var label)
    if(NOT ${have_var})
        message(STATUS "libstrophe: ${label} missing — using compat shim")
    endif()
endfunction()
_xepher_strophe_status(XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING "xmpp_stanza_new_from_string")
_xepher_strophe_status(XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX "xmpp_stanza_add_child_ex")
_xepher_strophe_status(XEPHER_HAVE_XMPP_CTX_SET_VERBOSITY "xmpp_ctx_set_verbosity")
_xepher_strophe_status(XEPHER_HAVE_XMPP_CONN_GET_FLAGS "xmpp_conn_get_flags")
_xepher_strophe_status(XEPHER_HAVE_XMPP_CONN_SET_FLAGS "xmpp_conn_set_flags")
_xepher_strophe_status(XEPHER_HAVE_XMPP_CONN_SET_KEEPALIVE "xmpp_conn_set_keepalive")
_xepher_strophe_status(XEPHER_HAVE_XMPP_CONN_SET_CERTFAIL_HANDLER "xmpp_conn_set_certfail_handler")
_xepher_strophe_status(XEPHER_HAVE_XMPP_CONNECT_RAW "xmpp_connect_raw")

# Config header for strophe_compat.hh (0/1 macros).
set(_xepher_strophe_new_from_string 0)
set(_xepher_strophe_add_child_ex 0)
set(_xepher_strophe_ctx_set_verbosity 0)
set(_xepher_strophe_conn_get_flags 0)
set(_xepher_strophe_conn_set_flags 0)
set(_xepher_strophe_conn_set_keepalive 0)
set(_xepher_strophe_conn_disable_tls 0)
set(_xepher_strophe_conn_set_certfail 0)
set(_xepher_strophe_connect_raw 0)
if(XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING)
    set(_xepher_strophe_new_from_string 1)
endif()
if(XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX)
    set(_xepher_strophe_add_child_ex 1)
endif()
if(XEPHER_HAVE_XMPP_CTX_SET_VERBOSITY)
    set(_xepher_strophe_ctx_set_verbosity 1)
endif()
if(XEPHER_HAVE_XMPP_CONN_GET_FLAGS)
    set(_xepher_strophe_conn_get_flags 1)
endif()
if(XEPHER_HAVE_XMPP_CONN_SET_FLAGS)
    set(_xepher_strophe_conn_set_flags 1)
endif()
if(XEPHER_HAVE_XMPP_CONN_SET_KEEPALIVE)
    set(_xepher_strophe_conn_set_keepalive 1)
endif()
if(XEPHER_HAVE_XMPP_CONN_DISABLE_TLS)
    set(_xepher_strophe_conn_disable_tls 1)
endif()
if(XEPHER_HAVE_XMPP_CONN_SET_CERTFAIL_HANDLER)
    set(_xepher_strophe_conn_set_certfail 1)
endif()
if(XEPHER_HAVE_XMPP_CONNECT_RAW)
    set(_xepher_strophe_connect_raw 1)
endif()
set(_xepher_define_conn_flag_macros 0)
set(_xepher_define_conn_flag_disable_sm 0)
if(NOT XEPHER_HAVE_XMPP_CONN_FLAG_MACROS)
    set(_xepher_define_conn_flag_macros 1)
endif()
if(NOT XEPHER_HAVE_XMPP_CONN_FLAG_DISABLE_SM)
    set(_xepher_define_conn_flag_disable_sm 1)
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/strophe_compat_config.hh.in"
    "${CMAKE_BINARY_DIR}/generated/strophe_compat_config.hh"
    @ONLY
)
target_include_directories(xepher_deps INTERFACE "${CMAKE_BINARY_DIR}/generated")
# sexp also needs the generated config + strophe for driver.cpp
target_include_directories(xepher_sexp PRIVATE "${CMAKE_BINARY_DIR}/generated" "${CMAKE_SOURCE_DIR}/src")
