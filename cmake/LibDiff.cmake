# Build deps/diff via its existing autotools makefile (same as legacy root makefile).

set(XEPHER_LIBDIFF_A "${CMAKE_SOURCE_DIR}/deps/diff/libdiff.a")
set(XEPHER_LIBDIFF_DIR "${CMAKE_SOURCE_DIR}/deps/diff")

find_program(XEPHER_GNU_MAKE NAMES gmake make REQUIRED)

add_custom_command(
    OUTPUT "${XEPHER_LIBDIFF_A}"
    COMMAND ${CMAKE_COMMAND} -E echo "HAVE___PROGNAME=1" > configure.local
    COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" ./configure
    COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS
        ${XEPHER_GNU_MAKE} CC=${CMAKE_C_COMPILER} CFLAGS=-fPIC
    WORKING_DIRECTORY "${XEPHER_LIBDIFF_DIR}"
    COMMENT "Building vendored libdiff"
    VERBATIM
)

add_custom_target(xepher_libdiff DEPENDS "${XEPHER_LIBDIFF_A}")