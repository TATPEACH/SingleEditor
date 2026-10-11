# One explicitly requested HRTF bank. NOTICE remains independently inspectable.
set(SINGLE_EDITOR_AMBISONIC_RESOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Ambisonics/SADIE_006_DFC_ACN_SN3D_1OA_3OA_48k.sehrir")
set(SINGLE_EDITOR_AMBISONIC_NOTICE "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Ambisonics/NOTICE.md")
set(SINGLE_EDITOR_AMBISONIC_PACK "${CMAKE_CURRENT_BINARY_DIR}/owner-packs/ambisonics/binaural-v1.sepack")
set(ambisonic_pack_content "${CMAKE_CURRENT_BINARY_DIR}/owner-packs/ambisonics/content")
file(MAKE_DIRECTORY "${ambisonic_pack_content}")
file(SHA256 "${SINGLE_EDITOR_AMBISONIC_RESOURCE}" AMBISONIC_DATA_SHA256)
file(SIZE "${SINGLE_EDITOR_AMBISONIC_RESOURCE}" AMBISONIC_DATA_BYTES)
file(SHA256 "${SINGLE_EDITOR_AMBISONIC_NOTICE}" AMBISONIC_NOTICE_SHA256)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SINGLE_EDITOR_AMBISONIC_RESOURCE}" "${SINGLE_EDITOR_AMBISONIC_NOTICE}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/AmbisonicPackManifest.json.in"
    "${ambisonic_pack_content}/manifest.json" @ONLY NEWLINE_STYLE LF)
file(SHA256 "${ambisonic_pack_content}/manifest.json" AMBISONIC_MANIFEST_SHA256)
file(SIZE "${ambisonic_pack_content}/manifest.json" AMBISONIC_MANIFEST_BYTES)
execute_process(COMMAND "${SINGLE_EDITOR_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/BuildWindowShellTrayPack.mjs"
    "${ambisonic_pack_content}/manifest.json" "${SINGLE_EDITOR_AMBISONIC_RESOURCE}"
    "${SINGLE_EDITOR_AMBISONIC_PACK}" binaural.sehrir RESULT_VARIABLE ambisonic_pack_result)
if(NOT ambisonic_pack_result EQUAL 0)
    message(FATAL_ERROR "Could not generate the Ambisonics owner resource pack")
endif()
file(SHA256 "${SINGLE_EDITOR_AMBISONIC_PACK}" AMBISONIC_PACK_SHA256)
file(SIZE "${SINGLE_EDITOR_AMBISONIC_PACK}" AMBISONIC_PACK_BYTES)
if(AMBISONIC_PACK_BYTES GREATER 65536 OR AMBISONIC_DATA_BYTES GREATER 61440 OR AMBISONIC_MANIFEST_BYTES GREATER 4096)
    message(FATAL_ERROR "Ambisonics pack exceeds its reviewed source/loader budget")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/AmbisonicPack.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/AmbisonicPack.h" @ONLY)

function(single_editor_copy_ambisonic_resources target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/resources/ambisonics"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SINGLE_EDITOR_AMBISONIC_PACK}"
            "$<TARGET_FILE_DIR:${target_name}>/resources/ambisonics/binaural-v1.sepack"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>/Ambisonics"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SINGLE_EDITOR_AMBISONIC_NOTICE}"
            "$<TARGET_FILE_DIR:${target_name}>/Ambisonics/NOTICE.md")
endfunction()

function(single_editor_bind_ambisonic_resources)
    get_property(local_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS local_targets)
        get_target_property(sources ${target} SOURCES)
        if("Source/Audio/AmbisonicBinauralFilterBank.cpp" IN_LIST sources)
            target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
            target_link_libraries(${target} PRIVATE juce::juce_cryptography)
        endif()
    endforeach()
endfunction()
