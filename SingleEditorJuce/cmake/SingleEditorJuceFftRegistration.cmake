# Register JUCE's compiled FFT providers only when an FFT is first requested.
# Provider algorithms, priority ordering and shared JUCE files remain unchanged.
option(SINGLE_EDITOR_LAZY_FFT_REGISTRATION "Register FFT engines on first FFT request" ON)

function(single_editor_configure_juce_fft_registration)
    if(NOT SINGLE_EDITOR_LAZY_FFT_REGISTRATION)
        return()
    endif()
    get_target_property(dsp_sources juce_dsp INTERFACE_SOURCES)
    set(amalgamations)
    foreach(source IN LISTS dsp_sources)
        if(source MATCHES "/juce_dsp\\.cpp$")
            list(APPEND amalgamations "${source}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES amalgamations)
    list(LENGTH amalgamations count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "Single Editor expects exactly one JUCE DSP amalgamation")
    endif()
    list(GET amalgamations 0 amalgamation)
    get_filename_component(dsp_root "${amalgamation}" DIRECTORY)
    set(fft "${dsp_root}/frequency/juce_FFT.cpp")
    foreach(part amalgamation fft)
        file(READ "${${part}}" ${part}_text)
        string(REPLACE "\r\n" "\n" ${part}_text "${${part}_text}")
        string(SHA256 ${part}_hash "${${part}_text}")
    endforeach()
    if(NOT amalgamation_hash STREQUAL "a37fef63955cc304f3f244fbb129457112d8ffc4ad9f7e634554116e8aa59820"
       OR NOT fft_hash STREQUAL "1c00fedab3380dd35e4cbb0abce72c5e0d60ca82a04b0d103eb386840679f702")
        message(FATAL_ERROR "JUCE DSP differs from the reviewed 8.0.12 FFT registration base. Review the overlay or explicitly set SINGLE_EDITOR_LAZY_FFT_REGISTRATION=OFF.")
    endif()

    string(REPLACE "        for (auto* engine : getEngines())"
        "        ensureEnginesRegistered();\n\n        for (auto* engine : getEngines())" fft_text "${fft_text}")
    string(REPLACE "    static Array<Engine*>& getEngines()"
        "    static void ensureEnginesRegistered();\n\n    static Array<Engine*>& getEngines()" fft_text "${fft_text}")
    foreach(declaration
        "FFT::EngineImpl<FFTFallback> fftFallback;"
        "FFT::EngineImpl<AppleFFT> appleFFT;"
        "FFT::EngineImpl<FFTWImpl> fftwEngine;"
        "FFT::EngineImpl<IntelFFT> fftwEngine;"
        "FFT::EngineImpl<IntelPerformancePrimitivesFFT> intelPerformancePrimitivesFFT;")
        string(REPLACE "${declaration}" "" fft_text "${fft_text}")
    endforeach()
    set(registration [=[void FFT::Engine::ensureEnginesRegistered()
{
    // One initialization guard covers registration and sorting of the entire
    // compiled provider set. Concurrent first callers cannot enumerate it early.
    static const bool registered = []
    {
        static FFT::EngineImpl<FFTFallback> fftFallback;
       #if (JUCE_MAC || JUCE_IOS) && JUCE_USE_VDSP_FRAMEWORK
        static FFT::EngineImpl<AppleFFT> appleFFT;
       #endif
       #if JUCE_DSP_USE_SHARED_FFTW || JUCE_DSP_USE_STATIC_FFTW
        static FFT::EngineImpl<FFTWImpl> fftwEngine;
       #endif
       #if JUCE_DSP_USE_INTEL_MKL
        static FFT::EngineImpl<IntelFFT> fftwEngine;
       #endif
       #if _IPP_SEQUENTIAL_STATIC || _IPP_SEQUENTIAL_DYNAMIC || _IPP_PARALLEL_STATIC || _IPP_PARALLEL_DYNAMIC
        static FFT::EngineImpl<IntelPerformancePrimitivesFFT> intelPerformancePrimitivesFFT;
       #endif
        return true;
    }();
    ignoreUnused (registered);
}

FFT::FFT (int order)]=])
    string(REPLACE "FFT::FFT (int order)" "${registration}" fft_text "${fft_text}")
    set(overlay "${CMAKE_CURRENT_BINARY_DIR}/single-editor-juce-dsp")
    file(MAKE_DIRECTORY "${overlay}")
    set(patched_fft "${overlay}/juce_FFT.cpp")
    set(patched_amalgamation "${overlay}/juce_dsp.cpp")
    string(REPLACE "#include \"frequency/juce_FFT.cpp\"" "#include \"${patched_fft}\"" amalgamation_text "${amalgamation_text}")
    foreach(part fft amalgamation)
        set(previous "")
        if(EXISTS "${patched_${part}}")
            file(READ "${patched_${part}}" previous)
        endif()
        if(NOT previous STREQUAL "${${part}_text}")
            file(WRITE "${patched_${part}}" "${${part}_text}")
        endif()
    endforeach()
    set_source_files_properties("${patched_amalgamation}" PROPERTIES INCLUDE_DIRECTORIES "${dsp_root}")
    set(mapped_sources)
    foreach(source IN LISTS dsp_sources)
        if(source STREQUAL amalgamation)
            list(APPEND mapped_sources "${patched_amalgamation}")
        else()
            list(APPEND mapped_sources "${source}")
        endif()
    endforeach()
    set_property(TARGET juce_dsp PROPERTY INTERFACE_SOURCES "${mapped_sources}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fft}" "${amalgamation}")
    message(STATUS "Single Editor: build-local FFT provider registration on first FFT request")
endfunction()
