# One typed owner pack. Node is build tooling only, as in the release evidence
# tools. The two stored ZIP entries have no host-dependent timestamp extras.
find_program(SINGLE_EDITOR_NODE_EXECUTABLE NAMES node REQUIRED)
set(SINGLE_EDITOR_TRAY_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png")
set(SINGLE_EDITOR_TRAY_PACK "${CMAKE_CURRENT_BINARY_DIR}/owner-packs/window-shell/tray-v1.sepack")
set(tray_pack_content "${CMAKE_CURRENT_BINARY_DIR}/owner-packs/window-shell/content")
file(MAKE_DIRECTORY "${tray_pack_content}")
file(SHA256 "${SINGLE_EDITOR_TRAY_ICON_SOURCE}" TRAY_IMAGE_SHA256)
file(SIZE "${SINGLE_EDITOR_TRAY_ICON_SOURCE}" TRAY_IMAGE_BYTES)
configure_file("${SINGLE_EDITOR_TRAY_ICON_SOURCE}" "${tray_pack_content}/tray-icon.png" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/WindowShellTrayManifest.json.in"
    "${tray_pack_content}/manifest.json" @ONLY NEWLINE_STYLE LF)
file(SHA256 "${tray_pack_content}/manifest.json" TRAY_MANIFEST_SHA256)
file(SIZE "${tray_pack_content}/manifest.json" TRAY_MANIFEST_BYTES)
set(tray_pack_builder "${CMAKE_CURRENT_LIST_DIR}/BuildWindowShellTrayPack.mjs")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tray_pack_builder}")
execute_process(COMMAND "${SINGLE_EDITOR_NODE_EXECUTABLE}" "${tray_pack_builder}"
    "${tray_pack_content}/manifest.json" "${tray_pack_content}/tray-icon.png" "${SINGLE_EDITOR_TRAY_PACK}"
    RESULT_VARIABLE tray_pack_result)
if(NOT tray_pack_result EQUAL 0)
    message(FATAL_ERROR "Could not generate the window-shell tray resource pack")
endif()
file(SHA256 "${SINGLE_EDITOR_TRAY_PACK}" TRAY_PACK_SHA256)
file(SIZE "${SINGLE_EDITOR_TRAY_PACK}" TRAY_PACK_BYTES)
if(TRAY_PACK_BYTES GREATER 524288 OR TRAY_IMAGE_BYTES GREATER 524288 OR TRAY_MANIFEST_BYTES GREATER 4096)
    message(FATAL_ERROR "Window-shell tray pack exceeds its bounded loader; review asset changes")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/WindowShellTrayPack.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/WindowShellTrayPack.h" @ONLY)

function(single_editor_copy_window_shell_resources target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/resources/window-shell"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SINGLE_EDITOR_TRAY_PACK}"
            "$<TARGET_FILE_DIR:${target_name}>/resources/window-shell/tray-v1.sepack")
endfunction()
