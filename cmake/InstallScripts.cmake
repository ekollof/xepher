# Install companion scripts separately from the compiled plugin. Relative links
# keep autoload entries valid when the WeeChat data directory is moved.
if(NOT DEFINED XEPHER_SCRIPT_SOURCE_DIR OR NOT DEFINED XEPHER_WEECHAT_HOME)
    message(FATAL_ERROR "Script source and WeeChat data directory are required")
endif()
file(MAKE_DIRECTORY "${XEPHER_WEECHAT_HOME}/python/autoload")
foreach(script IN ITEMS icat.py feed_compose.py)
    set(destination "${XEPHER_WEECHAT_HOME}/python/${script}")
    file(COPY_FILE "${XEPHER_SCRIPT_SOURCE_DIR}/${script}" "${destination}.new")
    file(RENAME "${destination}.new" "${destination}")
    file(CHMOD "${destination}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    file(REMOVE "${XEPHER_WEECHAT_HOME}/python/autoload/${script}")
    file(CREATE_LINK "../${script}" "${XEPHER_WEECHAT_HOME}/python/autoload/${script}"
        SYMBOLIC RESULT link_result)
    if(NOT link_result STREQUAL "0")
        message(FATAL_ERROR "Could not enable autoload for ${script}: ${link_result}")
    endif()
    message(STATUS "Installed ${script} with autoload in ${XEPHER_WEECHAT_HOME}")
endforeach()
