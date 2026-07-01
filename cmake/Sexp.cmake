find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

# Generate into sexp/ (same paths as the legacy makefile; artifacts are gitignored).
set(SEXP_GEN_DIR "${CMAKE_SOURCE_DIR}/sexp")

BISON_TARGET(
    xepher_sexp_parser
    "${CMAKE_SOURCE_DIR}/sexp/parser.yy"
    "${SEXP_GEN_DIR}/parser.tab.cc"
    DEFINES_FILE "${SEXP_GEN_DIR}/parser.tab.hh"
    COMPILE_FLAGS "-t -d -v"
)

FLEX_TARGET(
    xepher_sexp_lexer
    "${CMAKE_SOURCE_DIR}/sexp/lexer.l"
    "${SEXP_GEN_DIR}/lexer.yy.cc"
    COMPILE_FLAGS "-d"
)

ADD_FLEX_BISON_DEPENDENCY(xepher_sexp_lexer xepher_sexp_parser)

add_library(xepher_sexp STATIC
    "${CMAKE_SOURCE_DIR}/sexp/driver.cpp"
    ${BISON_xepher_sexp_parser_OUTPUTS}
    ${FLEX_xepher_sexp_lexer_OUTPUTS}
)

set_target_properties(xepher_sexp PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(xepher_sexp
    PUBLIC
        "${SEXP_GEN_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(xepher_sexp PRIVATE FMT_DEPRECATED_HEAVY_CORE)
target_compile_options(xepher_sexp PRIVATE -fvisibility=default)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(xepher_sexp PRIVATE
        -Wno-unused-variable
        -Wno-unused-but-set-variable
    )
else()
    target_compile_options(xepher_sexp PRIVATE -Wno-unused-but-set-variable)
endif()

add_library(Xepher::sexp ALIAS xepher_sexp)