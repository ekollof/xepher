cmake_minimum_required(VERSION 3.22)

# Compiler launcher must be set before project() (ccache iterative dev workflow).
if(NOT CMAKE_CXX_COMPILER_LAUNCHER AND NOT CMAKE_C_COMPILER_LAUNCHER)
    find_program(XEPHER_CCACHE NAMES ccache)
    if(XEPHER_CCACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${XEPHER_CCACHE}")
        set(CMAKE_C_COMPILER_LAUNCHER "${XEPHER_CCACHE}")
    endif()
endif()

project(xepher
    VERSION 0.10.2
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XEPHER_PACKAGE_BUILD "Skip dev-only .source ELF embed (distribution builds)" OFF)
option(XEPHER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(XEPHER_BUILD_TOOLS "Build developer tools (dump_mam_db, dump_omemo_db)" ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(XEPHER_DWARF_FLAG "-g")
else()
    set(XEPHER_DWARF_FLAG "-gdwarf-4")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${XEPHER_DWARF_FLAG}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${XEPHER_DWARF_FLAG}")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Xepher expects Clang; CXX=${CMAKE_CXX_COMPILER_ID}")
endif()

if(CMAKE_CXX_COMPILER_LAUNCHER)
    message(STATUS "Using compiler launcher: ${CMAKE_CXX_COMPILER_LAUNCHER}")
endif()

include("${CMAKE_SOURCE_DIR}/cmake/XepherFlags.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/LibDiff.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/Sexp.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake")

file(GLOB_RECURSE XEPHER_PLUGIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_library(xepher_plugin MODULE ${XEPHER_PLUGIN_SOURCES})
add_library(Xepher::plugin ALIAS xepher_plugin)

target_link_libraries(xepher_plugin PRIVATE Xepher::deps)
xepher_apply_common_compile_options(xepher_plugin)
xepher_apply_plugin_link_options(xepher_plugin)
xepher_apply_git_commit(xepher_plugin)
xepher_add_source_embed(xepher_plugin)

set_target_properties(xepher_plugin PROPERTIES
    OUTPUT_NAME "xmpp"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_target(weechat-xmpp DEPENDS xepher_plugin)
add_custom_target(xmpp ALIAS xepher_plugin)

include("${CMAKE_SOURCE_DIR}/cmake/Tests.cmake")
xepher_add_tests(xepher_plugin)

# CTest reserves the "test" target when enable_testing() is active; use xepher_test.
if(TARGET xepher_test)
    # Doctests use xmpp.cov.so, but finish the plugin first (matches make DEBUG=1 ordering).
    add_dependencies(xepher_test weechat-xmpp)
    add_custom_target(xepher_dev DEPENDS weechat-xmpp xepher_test)
endif()

if(TARGET xepher_coverage)
    add_custom_target(coverage DEPENDS xepher_coverage)
endif()

if(XEPHER_BUILD_TOOLS)
    add_subdirectory(tools)
    add_custom_target(tools DEPENDS xepher_tools)
endif()

if(EXISTS "${CMAKE_BINARY_DIR}/compile_commands.json")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/compile_commands.json"
    )
endif()