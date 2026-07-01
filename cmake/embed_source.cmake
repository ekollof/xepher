# POST_BUILD helper: tar tracked sources into xmpp.so .source ELF section.

if(NOT PLUGIN OR NOT OBJCOPY OR NOT GIT_EXECUTABLE)
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" ls-files
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE tracked_files
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT tracked_files)
    return()
endif()

if(NOT BUILD_DIR)
    set(BUILD_DIR "${CMAKE_BINARY_DIR}")
endif()

set(tarball "${BUILD_DIR}/xmpp.source.tar.gz")
set(file_list "${BUILD_DIR}/xmpp.source.files.txt")

string(REPLACE "\n" ";" tracked_list "${tracked_files}")
set(existing_files "")
foreach(path IN LISTS tracked_list)
    if(EXISTS "${SOURCE_DIR}/${path}")
        list(APPEND existing_files "${path}")
    endif()
endforeach()

if(NOT existing_files)
    return()
endif()

file(WRITE "${file_list}" "")
foreach(path IN LISTS existing_files)
    file(APPEND "${file_list}" "${path}\n")
endforeach()

execute_process(
    COMMAND tar cz -T "${file_list}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_FILE "${tarball}"
    RESULT_VARIABLE tar_result
)

if(tar_result EQUAL 0)
    execute_process(
        COMMAND "${OBJCOPY}" --add-section ".source=${tarball}" "${PLUGIN}"
        RESULT_VARIABLE objcopy_result
    )
endif()