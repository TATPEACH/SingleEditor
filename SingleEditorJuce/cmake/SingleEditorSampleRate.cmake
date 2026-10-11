# D107 playback backend. Kept behind PlaybackRateSource so the later M4/M5
# converter evaluation does not alter Document, readers, edits or saved PCM.
function(single_editor_configure_samplerate)
    # Upstream 0.2.2 predates CMake 4; scope its legacy policy floor here.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.15)
    if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
    set(LIBSAMPLERATE_ENABLE_SINC_FAST_CONVERTER OFF CACHE BOOL "" FORCE)
    set(LIBSAMPLERATE_ENABLE_SINC_MEDIUM_CONVERTER ON CACHE BOOL "" FORCE)
    set(LIBSAMPLERATE_ENABLE_SINC_BEST_CONVERTER OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(single_editor_samplerate
        URL https://github.com/libsndfile/libsamplerate/archive/refs/tags/0.2.2.tar.gz
        URL_HASH SHA256=16e881487f184250deb4fcb60432d7556ab12cb58caea71ef23960aec6c0405a
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(single_editor_samplerate)
    # Binary distributions must carry the upstream BSD notice.
    configure_file("${single_editor_samplerate_SOURCE_DIR}/COPYING"
        "${CMAKE_BINARY_DIR}/licenses/libsamplerate-0.2.2.txt" COPYONLY)
endfunction()
single_editor_configure_samplerate()

function(single_editor_link_samplerate target)
    target_link_libraries(${target} PRIVATE SampleRate::samplerate)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/licenses/libsamplerate-0.2.2.txt"
            "$<TARGET_FILE_DIR:${target}>/licenses/libsamplerate-0.2.2.txt")
endfunction()
