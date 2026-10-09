# Doctests (Debug only).
#
# Day-to-day: link tests against the normal plugin (xmpp.so) — no second full
# compile with --coverage. Coverage builds are EXCLUDE_FROM_ALL and only built
# for `make coverage` / the xepher_coverage target.

macro(xepher_add_tests plugin_target)
    # Do not use return() inside a macro — it returns from the including file.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")

    # ── Fast path: tests + normal plugin ─────────────────────────────────────
    add_executable(xepher_tests_run "${CMAKE_SOURCE_DIR}/tests/main.cc")
    target_include_directories(xepher_tests_run PRIVATE
        "${CMAKE_SOURCE_DIR}/tests"
        "${CMAKE_SOURCE_DIR}/deps"
        "${CMAKE_SOURCE_DIR}/deps/doctest"
        "${CMAKE_SOURCE_DIR}/src"
    )
    xepher_apply_common_compile_options(xepher_tests_run)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(xepher_tests_run PRIVATE
            -Wl,--allow-shlib-undefined
            "-Wl,-rpath,${CMAKE_SOURCE_DIR}"
        )
    else()
        target_link_options(xepher_tests_run PRIVATE
            -Wl,-undefined,dynamic_lookup
            "-Wl,-rpath,${CMAKE_SOURCE_DIR}"
        )
    endif()

    target_link_libraries(xepher_tests_run PRIVATE Xepher::deps)
    # MODULE output has no lib prefix; link by full path (legacy test.mk).
    target_link_options(xepher_tests_run PRIVATE
        "$<TARGET_FILE:${plugin_target}>"
    )

    set_target_properties(xepher_tests_run PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
        OUTPUT_NAME "run"
    )

    add_dependencies(xepher_tests_run ${plugin_target})

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
        DEPENDS xepher_tests_run ${plugin_target}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Running doctests (CTest)"
        VERBATIM
    )

    # Exercise the actual buffer callbacks, privacy modifiers and editor lifecycle.
    find_program(XEPHER_WEECHAT_HEADLESS NAMES weechat-headless)
    if(XEPHER_WEECHAT_HEADLESS)
        add_library(xepher_form_editor_smoke MODULE EXCLUDE_FROM_ALL
            "${CMAKE_SOURCE_DIR}/tests/form_editor_smoke.cc")
        target_include_directories(xepher_form_editor_smoke PRIVATE "${CMAKE_SOURCE_DIR}/src")
        xepher_apply_common_compile_options(xepher_form_editor_smoke)
        target_link_libraries(xepher_form_editor_smoke PRIVATE Xepher::deps)
        target_link_options(xepher_form_editor_smoke PRIVATE "$<TARGET_FILE:${plugin_target}>")
        set_target_properties(xepher_form_editor_smoke PROPERTIES PREFIX "" OUTPUT_NAME "formprobe")
        add_dependencies(xepher_form_editor_smoke ${plugin_target})
        add_dependencies(xepher_test xepher_form_editor_smoke)
        add_test(NAME doctest_form_editor
            COMMAND "${XEPHER_WEECHAT_HEADLESS}" --stdout -a -t -P buflist -r
                "/plugin load $<TARGET_FILE:${plugin_target}>;/plugin load $<TARGET_FILE:xepher_form_editor_smoke>;/plugin unload formprobe;/plugin unload xmpp;/quit")
        set_tests_properties(doctest_form_editor PROPERTIES
            TIMEOUT 15 LABELS "doctest"
            SKIP_REGULAR_EXPRESSION "FORM_SMOKE_SKIP"
            PASS_REGULAR_EXPRESSION "FORM_SMOKE_PASS"
            FAIL_REGULAR_EXPRESSION "FORM_SMOKE_FAIL")
    endif()

    find_package(Python3 QUIET COMPONENTS Interpreter)
    find_program(XEPHER_OPENSSL_CLI NAMES openssl)
    if(Python3_Interpreter_FOUND AND XEPHER_WEECHAT_HEADLESS AND XEPHER_OPENSSL_CLI)
        add_test(NAME doctest_registration_tls
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_registration_tls.py"
                --plugin "$<TARGET_FILE:${plugin_target}>"
                --weechat "${XEPHER_WEECHAT_HEADLESS}" --openssl "${XEPHER_OPENSSL_CLI}" -v)
        set_tests_properties(doctest_registration_tls PROPERTIES
            TIMEOUT 45 LABELS "doctest" RUN_SERIAL TRUE
            SKIP_REGULAR_EXPRESSION "OK \\(skipped=2\\)")
    endif()

    # ── Coverage path (opt-in; not built by default) ─────────────────────────
    file(GLOB_RECURSE XEPHER_TEST_PLUGIN_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
    )

    add_library(xepher_plugin_cov MODULE EXCLUDE_FROM_ALL ${XEPHER_TEST_PLUGIN_SOURCES})
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

    add_executable(xepher_tests_run_cov EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tests/main.cc")
    target_include_directories(xepher_tests_run_cov PRIVATE
        "${CMAKE_SOURCE_DIR}/tests"
        "${CMAKE_SOURCE_DIR}/deps"
        "${CMAKE_SOURCE_DIR}/deps/doctest"
        "${CMAKE_SOURCE_DIR}/src"
    )
    xepher_apply_common_compile_options(xepher_tests_run_cov)
    target_compile_options(xepher_tests_run_cov PRIVATE --coverage)
    target_link_options(xepher_tests_run_cov PRIVATE --coverage)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(xepher_tests_run_cov PRIVATE
            -Wl,--allow-shlib-undefined
            "-Wl,-rpath,${CMAKE_SOURCE_DIR}/tests"
        )
    else()
        target_link_options(xepher_tests_run_cov PRIVATE
            -Wl,-undefined,dynamic_lookup
            "-Wl,-rpath,${CMAKE_SOURCE_DIR}/tests"
        )
    endif()

    target_link_libraries(xepher_tests_run_cov PRIVATE Xepher::deps)
    target_link_options(xepher_tests_run_cov PRIVATE
        "$<TARGET_FILE:xepher_plugin_cov>"
    )

    set_target_properties(xepher_tests_run_cov PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/tests"
        OUTPUT_NAME "run.cov"
    )

    add_dependencies(xepher_tests_run_cov xepher_plugin_cov)

    find_program(XEPHER_GCOVR NAMES gcovr)
    if(XEPHER_GCOVR)
        add_custom_target(xepher_coverage
            COMMAND "${CMAKE_SOURCE_DIR}/tests/run.cov" -sm
            COMMAND ${XEPHER_GCOVR} --txt -s --merge-mode-functions=separate
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS xepher_tests_run_cov xepher_plugin_cov
            COMMENT "Running coverage-instrumented doctests + gcovr"
            VERBATIM
        )
    else()
        add_custom_target(xepher_coverage
            COMMAND "${CMAKE_SOURCE_DIR}/tests/run.cov" -sm
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS xepher_tests_run_cov xepher_plugin_cov
            COMMENT "Running coverage-instrumented doctests (gcovr not found)"
            VERBATIM
        )
    endif()

    endif() # Debug
endmacro()
