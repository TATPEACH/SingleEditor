# Preserve the existing large-WAV fast path and keep optional ASWG key lookup out
# of process-wide initialization. Only this build's audio-format sources change.
option(SINGLE_EDITOR_LAZY_WAV_METADATA "Initialize ASWG WAV metadata keys only when used" ON)

function(single_editor_configure_juce_wav_metadata)
    get_target_property(format_sources juce_audio_formats INTERFACE_SOURCES)
    set(amalgamations)
    foreach(source IN LISTS format_sources)
        if(source MATCHES "/juce_audio_formats\\.cpp$")
            list(APPEND amalgamations "${source}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES amalgamations)
    list(LENGTH amalgamations count)
    if(NOT count EQUAL 1)
        if(NOT SINGLE_EDITOR_LAZY_WAV_METADATA)
            return()
        endif()
        message(FATAL_ERROR "Single Editor expects exactly one JUCE audio-format amalgamation")
    endif()
    list(GET amalgamations 0 amalgamation)
    get_filename_component(format_root "${amalgamation}" DIRECTORY)
    set(wav "${format_root}/codecs/juce_WavAudioFormat.cpp")
    foreach(part amalgamation wav)
        if(NOT SINGLE_EDITOR_LAZY_WAV_METADATA AND NOT EXISTS "${${part}}")
            return()
        endif()
        file(READ "${${part}}" ${part}_text)
        string(REPLACE "\r\n" "\n" ${part}_text "${${part}_text}")
        string(SHA256 ${part}_hash "${${part}_text}")
    endforeach()
    set(official_wav_hash "879ec0d03ac5b9d702cf5aa6e41fe1714efe98bd1b96e1d19b5f5945986af825")
    set(legacy_fast_wav_hash "7c68d165a0519ef7f7e61bb16b99bc2263cba6a2c5ce745f2fbefe8076db5126")
    if(NOT amalgamation_hash STREQUAL "d0f763ad4b368d0aa4cf5a9505e66bee99c95188a4c2e2c2bc4e11d4c06c7cbb"
       OR (NOT wav_hash STREQUAL official_wav_hash AND NOT wav_hash STREQUAL legacy_fast_wav_hash))
        if(NOT SINGLE_EDITOR_LAZY_WAV_METADATA)
            # Preserve the existing explicit fallback for unreviewed upstream.
            return()
        endif()
        message(FATAL_ERROR
            "JUCE WAV source differs from the reviewed 8.0.12 metadata patch base. "
            "Review the overlay or explicitly set SINGLE_EDITOR_LAZY_WAV_METADATA=OFF.")
    endif()

    if(wav_hash STREQUAL legacy_fast_wav_hash AND NOT SINGLE_EDITOR_LAZY_WAV_METADATA)
        return()
    endif()
    if(wav_hash STREQUAL official_wav_hash)
        # Move the exact historical FAST_WAV_OPEN change out of shared JUCE.
        # The complete resulting source must match the retained legacy base.
        string(REPLACE "namespace juce\n{\n"
            "namespace juce\n{\n\n#ifndef SINGLE_EDITOR_FAST_WAV_OPEN\n#define SINGLE_EDITOR_FAST_WAV_OPEN 0\n#endif\n"
            wav_text "${wav_text}")
        string(REPLACE "        int cueRegionIndex = 0;\n"
            "        int cueRegionIndex = 0;\n        bool formatChunkFound = false;\n"
            wav_text "${wav_text}")
        string(REPLACE "        auto startOfRIFFChunk = input->getPosition();\n"
            [=[        auto startOfRIFFChunk = input->getPosition();
        const auto shouldPreferEarlyExitAfterData = [] (uint64 riffLength) noexcept
        {
           #if SINGLE_EDITOR_FAST_WAV_OPEN
            constexpr uint64 largeWavEarlyExitThreshold = 64ull * 1024ull * 1024ull;
            return riffLength >= largeWavEarlyExitThreshold;
           #else
            juce::ignoreUnused (riffLength);
            return false;
           #endif
        } (len);
]=] wav_text "${wav_text}")
        string(REPLACE "                if (chunkType == chunkName (\"fmt \"))\n                {\n"
            "                if (chunkType == chunkName (\"fmt \"))\n                {\n                    formatChunkFound = true;\n"
            wav_text "${wav_text}")
        string(REPLACE "                    lengthInSamples = (bytesPerFrame > 0) ? (dataLength / bytesPerFrame) : 0;\n"
            [=[                    lengthInSamples = (bytesPerFrame > 0) ? (dataLength / bytesPerFrame) : 0;

                    if (shouldPreferEarlyExitAfterData
                        && formatChunkFound
                        && dataChunkStart > 0
                        && lengthInSamples > 0)
                    {
                        break;
                    }
]=] wav_text "${wav_text}")
        string(SHA256 fast_wav_hash "${wav_text}")
        if(NOT fast_wav_hash STREQUAL legacy_fast_wav_hash)
            message(FATAL_ERROR "Build-local FAST WAV source does not match the reviewed historical implementation")
        endif()
    endif()

    if(SINGLE_EDITOR_LAZY_WAV_METADATA)
    string(REPLACE "        static const std::unordered_set<String> aswgMetadataKeys\n        {"
        "        static const std::unordered_set<String>& getAswgMetadataKeys()\n        {\n            static const std::unordered_set<String> aswgMetadataKeys\n            {"
        wav_text "${wav_text}")
    string(REPLACE "            WavAudioFormat::aswgBillingCode\n        };"
        "            WavAudioFormat::aswgBillingCode\n            };\n            return aswgMetadataKeys;\n        }"
        wav_text "${wav_text}")
    string(REPLACE "aswgMetadataKeys.find (tag) != aswgMetadataKeys.end()"
        "getAswgMetadataKeys().count (tag) != 0" wav_text "${wav_text}")
    string(REPLACE "aswgMetadataKeys.find (pair.first) != aswgMetadataKeys.end()"
        "getAswgMetadataKeys().count (pair.first) != 0" wav_text "${wav_text}")
    string(REPLACE "WavFileHelpers::IXMLChunk::aswgMetadataKeys)"
        "WavFileHelpers::IXMLChunk::getAswgMetadataKeys())" wav_text "${wav_text}")
    endif()

    set(overlay "${CMAKE_CURRENT_BINARY_DIR}/single-editor-juce-formats")
    file(MAKE_DIRECTORY "${overlay}")
    set(patched_wav "${overlay}/juce_WavAudioFormat.cpp")
    set(patched_amalgamation "${overlay}/juce_audio_formats.cpp")
    string(REPLACE "#include \"codecs/juce_WavAudioFormat.cpp\""
        "#include \"${patched_wav}\"" amalgamation_text "${amalgamation_text}")
    foreach(part wav amalgamation)
        set(previous "")
        if(EXISTS "${patched_${part}}")
            file(READ "${patched_${part}}" previous)
        endif()
        if(NOT previous STREQUAL "${${part}_text}")
            file(WRITE "${patched_${part}}" "${${part}_text}")
        endif()
    endforeach()
    set_source_files_properties("${patched_amalgamation}" PROPERTIES INCLUDE_DIRECTORIES "${format_root}")
    set(mapped_sources)
    foreach(source IN LISTS format_sources)
        if(source STREQUAL amalgamation)
            list(APPEND mapped_sources "${patched_amalgamation}")
        else()
            list(APPEND mapped_sources "${source}")
        endif()
    endforeach()
    set_property(TARGET juce_audio_formats PROPERTY INTERFACE_SOURCES "${mapped_sources}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${wav}" "${amalgamation}")
    if(SINGLE_EDITOR_LAZY_WAV_METADATA)
        message(STATUS "Single Editor: build-local FAST WAV path and metadata keys initialized on first use")
    else()
        message(STATUS "Single Editor: build-local FAST WAV path; upstream eager metadata retained")
    endif()
endfunction()
