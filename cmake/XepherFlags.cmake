function(xepher_apply_common_compile_options target)
    target_compile_options(${target} PRIVATE
        -fno-omit-frame-pointer
        -fPIC
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -Wall
        -Wextra
        -pedantic
        -Werror
        -Wno-missing-field-initializers
        -Wno-variadic-macros
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            -Wno-gnu-zero-variadic-macro-arguments
            # RPM %optflags inject GCC-only -specs=...; Clang treats them as unused.
            -Wno-unused-command-line-argument
        )
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(${target} PRIVATE -D_XOPEN_SOURCE=700)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(${target} PRIVATE DEBUG)
    else()
        target_compile_definitions(${target} PRIVATE NDEBUG)
    endif()

    target_compile_definitions(${target} PRIVATE FMT_DEPRECATED_HEAVY_CORE)
endfunction()

function(xepher_apply_plugin_link_options target)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${target} PRIVATE -Wl,--as-needed)
    endif()

    if(XEPHER_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_libraries(${target} PRIVATE asan rt)
        endif()
    endif()
endfunction()

function(xepher_apply_git_commit target)
    find_package(Git QUIET)
    set(git_ref "")
    if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --abbrev=6 --always --dirty
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE git_ref
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    target_compile_definitions(${target} PRIVATE "GIT_COMMIT=${git_ref}")
endfunction()

function(xepher_add_source_embed target)
    if(XEPHER_PACKAGE_BUILD)
        return()
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()

    find_program(XEPHER_OBJCOPY NAMES objcopy llvm-objcopy)
    if(NOT XEPHER_OBJCOPY)
        return()
    endif()

    find_package(Git QUIET)
    if(NOT GIT_FOUND OR NOT EXISTS "${CMAKE_SOURCE_DIR}/.git")
        return()
    endif()

    add_custom_command(
        TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DPLUGIN="$<TARGET_FILE:${target}>"
            -DOBJCOPY="${XEPHER_OBJCOPY}"
            -DSOURCE_DIR="${CMAKE_SOURCE_DIR}"
            -DBUILD_DIR="${CMAKE_BINARY_DIR}"
            -DGIT_EXECUTABLE="${GIT_EXECUTABLE}"
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_source.cmake"
        COMMENT "Embedding .source section into ${target}"
        VERBATIM
    )
endfunction()