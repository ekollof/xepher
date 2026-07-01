macro(xepher_add_tests plugin_target)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")

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

    enable_testing()

    add_test(
        NAME doctest
        COMMAND "${CMAKE_SOURCE_DIR}/tests/run" -sm
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
    )
    set_tests_properties(doctest PROPERTIES
        TIMEOUT 120
        LABELS "doctest"
    )

    add_custom_target(xepher_test
        COMMAND ${CMAKE_CTEST_COMMAND} --force-new-ctest-process --output-on-failure -R doctest
        DEPENDS xepher_tests_run xepher_plugin_cov
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Running doctests (CTest)"
        VERBATIM
    )

    find_program(XEPHER_GCOVR NAMES gcovr)
    if(XEPHER_GCOVR)
        add_custom_target(xepher_coverage
            COMMAND ${XEPHER_GCOVR} --txt -s --merge-mode-functions=separate
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS xepher_tests_run xepher_plugin_cov
            COMMENT "Generating gcovr coverage report"
            VERBATIM
        )
    endif()

    endif() # Debug build only
endmacro()