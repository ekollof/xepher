find_library(XEPHER_TOOLS_LMDB NAMES lmdb REQUIRED)

function(xepher_add_tool target)
    add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${target}.cpp")
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE
        -O2
        -Wall
        -Wextra
        -Wpedantic
    )
    target_link_libraries(${target} PRIVATE ${XEPHER_TOOLS_LMDB})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endfunction()

xepher_add_tool(dump_mam_db)
xepher_add_tool(dump_omemo_db)

add_custom_target(xepher_tools DEPENDS dump_mam_db dump_omemo_db)