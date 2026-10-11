# D108: official pinned external headers, enabled per target only. No SDK
# implementation, registry scan or driver DLL is a startup dependency.
set(SINGLE_EDITOR_ASIO_SDK_ROOT "" CACHE PATH "Offline ASIO SDK 2.3.4 root (contains common/ and LICENSE.txt)")

function(single_editor_enable_asio target)
    if(NOT WIN32)
        return()
    endif()
    if(SINGLE_EDITOR_ASIO_SDK_ROOT)
        get_filename_component(sdk_root "${SINGLE_EDITOR_ASIO_SDK_ROOT}" ABSOLUTE)
    else()
        include(FetchContent)
        FetchContent_Declare(single_editor_asio_sdk
            URL https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
            URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
        FetchContent_MakeAvailable(single_editor_asio_sdk)
        set(sdk_root "${single_editor_asio_sdk_SOURCE_DIR}")
    endif()
    set(SINGLE_EDITOR_RESOLVED_ASIO_SDK_ROOT "${sdk_root}" CACHE INTERNAL
        "Actual ASIO SDK provider used by the configured product" FORCE)

    set(sdk_files common/asio.h common/asiosys.h common/iasiodrv.h LICENSE.txt)
    set(sdk_hashes
        93d2f80d49b4ac9c963514e7c1307ab5111d8104246192233745beb058534ea1
        f44a1ed0fd7c056ff047aeebc4c05a8c185f0585a24d8debc55c70107777dbef
        86ad4788452c74823653cf57d4bd557c48bb6542d7e6eb7fff80102c705a066d
        61e56814f24b32216884b2db0b4dcc317111a5ab354d54d19dd9fc13d71b1c7a)
    foreach(file expected IN ZIP_LISTS sdk_files sdk_hashes)
        if(NOT EXISTS "${sdk_root}/${file}")
            message(FATAL_ERROR "Missing pinned ASIO SDK file: ${sdk_root}/${file}")
        endif()
        file(SHA256 "${sdk_root}/${file}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "ASIO SDK 2.3.4 hash mismatch: ${file}; use the official pinned archive")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sdk_root}/${file}")
    endforeach()
    target_include_directories(${target} SYSTEM PRIVATE "${sdk_root}/common")
    target_compile_definitions(${target} PRIVATE JUCE_ASIO=1 JUCE_ASIO_USE_EXTERNAL_SDK=1)
    message(STATUS "${target}: JUCE ASIO with pinned external SDK 2.3.4")
endfunction()
