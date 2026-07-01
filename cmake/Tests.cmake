function(xepher_add_tests plugin_target)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()

    file(GLOB_RECURSE XEPHER_TEST_PLUGIN_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
    )

    add_library(xepher_plugin_cov MODULE ${XEPHER_TEST_PLUGIN_SOURCES})
    target_link_libraries(xepher_plugin_cov PRIVATE Xepher::deps)
    xepher_apply_common_compile_options(xepher_plugin_cov)
    xepher_apply_plugin_link_options(xepher_plugin_cov)
    xepher_apply_git_commit(xepher_plugin_cov)

    target_compile_options(xepher_plugin_cov PRIVATE --coverage)
    target_link_options(xepher_plugin_cov PRIVATE --coverage)

    set_target_properties(xepher_plugin_cov PROPERTIES
        OUTPUT_NAME "xmpp.cov"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
        PREFIX ""
    )

    add_executable(xepher_tests_run "${CMAKE_SOURCE_DIR}/tests/main.cc")
    target_include_directories(xepher_tests_run PRIVATE
        "${CMAKE_SOURCE_DIR}/tests"
        "${CMAKE_SOURCE_DIR}/deps"
        "${CMAKE_SOURCE_DIR}/deps/doctest"
        "${CMAKE_SOURCE_DIR}/src"
    )
    xepher_apply_common_compile_options(xepher_tests_run)
    target_compile_options(xepher_tests_run PRIVATE --coverage)
    target_link_options(xepher_tests_run PRIVATE --coverage)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(xepher_tests_run PRIVATE
            -Wl,--allow-shlib-undefined
            "-Wl,-rpath,${CMAKE_SOURCE_DIR}/tests"
        )
    else()
        target_link_options(xepher_tests_run PRIVATE
            -Wl,-undefined,dynamic_lookup
            "-Wl,-rpath,${CMAKE_SOURCE_DIR}/tests"
        )
    endif()

    target_link_libraries(xepher_tests_run PRIVATE Xepher::deps)
    # MODULE output has no lib prefix; link by full path (legacy test.mk).
    target_link_options(xepher_tests_run PRIVATE
        "$<TARGET_FILE:xepher_plugin_cov>"
    )

    set_target_properties(xepher_tests_run PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
        OUTPUT_NAME "run"
    )

    add_dependencies(xepher_tests_run xepher_plugin_cov)

    find_program(XEPHER_TIMEOUT NAMES timeout)
    if(XEPHER_TIMEOUT)
        add_custom_target(xepher_test
            COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}/tests"
                ${XEPHER_TIMEOUT} --kill-after=5 120
                "${CMAKE_SOURCE_DIR}/tests/run" -sm
            DEPENDS xepher_tests_run
            COMMENT "Running doctests"
            VERBATIM
        )
    else()
        add_custom_target(xepher_test
            COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_SOURCE_DIR}/tests"
                "${CMAKE_SOURCE_DIR}/tests/run" -sm
            DEPENDS xepher_tests_run
            COMMENT "Running doctests"
            VERBATIM
        )
    endif()
endfunction()