# Build deps/diff via its existing autotools makefile (same as legacy root makefile).

set(XEPHER_LIBDIFF_A "${CMAKE_SOURCE_DIR}/deps/diff/libdiff.a")
set(XEPHER_LIBDIFF_DIR "${CMAKE_SOURCE_DIR}/deps/diff")

find_program(XEPHER_GNU_MAKE NAMES gmake make REQUIRED)

# Nested autotools make must not inherit CMake's jobserver MAKEFLAGS (breaks on
# FreeBSD gmake: "argument 'observer-auth=fifo:…' to option '-j' must be a
# positive number"). Match makefile seed-libdiff: clear MAKEFLAGS/MFLAGS, -j1.
add_custom_command(
    OUTPUT "${XEPHER_LIBDIFF_A}"
    COMMAND ${CMAKE_COMMAND} -E echo "HAVE___PROGNAME=1" > configure.local
    COMMAND ${CMAKE_COMMAND} -E env
        --unset=MAKEFLAGS --unset=MFLAGS
        "CC=${CMAKE_C_COMPILER}"
        sh ./configure
    COMMAND ${CMAKE_COMMAND} -E env
        --unset=MAKEFLAGS --unset=MFLAGS
        ${XEPHER_GNU_MAKE} -j1
        "CC=${CMAKE_C_COMPILER}" "CFLAGS=-fPIC"
    WORKING_DIRECTORY "${XEPHER_LIBDIFF_DIR}"
    COMMENT "Building vendored libdiff"
    VERBATIM
)

add_custom_target(xepher_libdiff DEPENDS "${XEPHER_LIBDIFF_A}")