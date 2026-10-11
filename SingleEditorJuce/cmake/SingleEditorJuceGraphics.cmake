# A build-local JUCE8.0.12 overlay; never edit a shared/downloaded dependency.
option(SINGLE_EDITOR_SKIP_EXTRA_OUTPUTLESS_ADAPTERS
    "Avoid creating unused additional DirectX devices at window startup" ON)

function(single_editor_configure_juce_graphics)
    if(NOT WIN32 OR NOT SINGLE_EDITOR_SKIP_EXTRA_OUTPUTLESS_ADAPTERS)
        return()
    endif()

    get_target_property(graphics_sources juce_graphics INTERFACE_SOURCES)
    set(amalgamations)
    foreach(source IN LISTS graphics_sources)
        if(source MATCHES "/juce_graphics\\.cpp$")
            list(APPEND amalgamations "${source}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES amalgamations)
    list(LENGTH amalgamations count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "Single Editor expects exactly one JUCE graphics amalgamation")
    endif()
    list(GET amalgamations 0 amalgamation)
    get_filename_component(graphics_root "${amalgamation}" DIRECTORY)
    set(directx "${graphics_root}/native/juce_DirectX_windows.cpp")
    # Git checkouts and the official archive can differ only in line endings.
    file(READ "${amalgamation}" amalgamation_text)
    file(READ "${directx}" directx_text)
    string(REPLACE "\r\n" "\n" amalgamation_text "${amalgamation_text}")
    string(REPLACE "\r\n" "\n" directx_text "${directx_text}")
    string(SHA256 amalgamation_hash "${amalgamation_text}")
    string(SHA256 directx_hash "${directx_text}")
    if(NOT amalgamation_hash STREQUAL "f0c7dc88e9ac26606016776529a00f9b5f67862a5ce1bd68eb66affddd956b28"
       OR NOT directx_hash STREQUAL "a50c6e07aab945762edd8a3b723c040539a31712c8877a3bf61c76a3e44d3dfc")
        message(FATAL_ERROR
            "JUCE graphics source differs from the reviewed 8.0.12 patch base. "
            "Review/update the overlay, or explicitly configure "
            "-DSINGLE_EDITOR_SKIP_EXTRA_OUTPUTLESS_ADAPTERS=OFF to use upstream behavior.")
    endif()

    set(anchor "        if (const auto adapter = DxgiAdapter::create (d2dFactory, dxgiAdapter))")
    set(insertion [=[        // Retain the first usable default and every display adapter. Additional
        // outputless adapters cannot match a window monitor. Unknown enumeration
        // failures retain the original device creation/fallback path.
        if (! adapterArray.isEmpty() && dxgiAdapter != nullptr)
        {
            ComSmartPtr<IDXGIOutput> firstOutput;

            if (dxgiAdapter->EnumOutputs (0, firstOutput.resetAndGetPointerAddress()) == DXGI_ERROR_NOT_FOUND)
                continue;
        }

]=])
    string(REPLACE "${anchor}" "${insertion}${anchor}" directx_text "${directx_text}")
    set(overlay "${CMAKE_CURRENT_BINARY_DIR}/single-editor-juce-graphics")
    file(MAKE_DIRECTORY "${overlay}")
    set(patched_directx "${overlay}/juce_DirectX_windows.cpp")
    set(patched_amalgamation "${overlay}/juce_graphics.cpp")
    string(REPLACE "#include \"native/juce_DirectX_windows.cpp\""
        "#include \"${patched_directx}\"" amalgamation_text "${amalgamation_text}")
    foreach(part directx amalgamation)
        set(previous "")
        if(EXISTS "${patched_${part}}")
            file(READ "${patched_${part}}" previous)
        endif()
        if(NOT previous STREQUAL "${${part}_text}")
            file(WRITE "${patched_${part}}" "${${part}_text}")
        endif()
    endforeach()
    # Only the copied amalgamation needs the original directory for relative
    # includes. All other headers/sources still come directly from JUCE.
    set_source_files_properties("${patched_amalgamation}" PROPERTIES
        INCLUDE_DIRECTORIES "${graphics_root}")
    set(mapped_sources)
    foreach(source IN LISTS graphics_sources)
        if(source STREQUAL amalgamation)
            list(APPEND mapped_sources "${patched_amalgamation}")
        else()
            list(APPEND mapped_sources "${source}")
        endif()
    endforeach()
    set_property(TARGET juce_graphics PROPERTY INTERFACE_SOURCES "${mapped_sources}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${directx}" "${amalgamation}")
    message(STATUS "Single Editor: using reviewed build-local DirectX adapter overlay")
endfunction()
