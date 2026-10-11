# The JUCE compilation clock is a declared source epoch in reproducible Release
# products. Actual build wall-clock times belong in the external build receipts.
function(single_editor_configure_reproducible_release)
    if(NOT MSVC)
        return()
    endif()

    if(NOT DEFINED SINGLE_EDITOR_SOURCE_DATE_EPOCH)
        execute_process(COMMAND git show -s --format=%ct HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE source_epoch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        set(SINGLE_EDITOR_SOURCE_DATE_EPOCH "${source_epoch}" CACHE STRING
            "Source epoch for reproducible Release compilation dates; required for source archives without Git")
    endif()
    if(NOT SINGLE_EDITOR_SOURCE_DATE_EPOCH MATCHES "^[0-9]+$")
        message(FATAL_ERROR "Set SINGLE_EDITOR_SOURCE_DATE_EPOCH to the source revision's Unix timestamp")
    endif()
    set(had_epoch FALSE)
    if(DEFINED ENV{SOURCE_DATE_EPOCH})
        set(had_epoch TRUE)
        set(previous_epoch "$ENV{SOURCE_DATE_EPOCH}")
    endif()
    set(ENV{SOURCE_DATE_EPOCH} "${SINGLE_EDITOR_SOURCE_DATE_EPOCH}")
    string(TIMESTAMP source_date "%b %d %Y" UTC)
    string(TIMESTAMP source_time "%H:%M:%S" UTC)
    if(had_epoch)
        set(ENV{SOURCE_DATE_EPOCH} "${previous_epoch}")
    else()
        unset(ENV{SOURCE_DATE_EPOCH})
    endif()

    get_target_property(core_sources juce_core INTERFACE_SOURCES)
    set(mapped_sources)
    set(replaced FALSE)
    foreach(source IN LISTS core_sources)
        if(source MATCHES "/juce_core_CompilationTime\\.cpp$")
            if(replaced)
                message(FATAL_ERROR "Expected one JUCE compilation clock source")
            endif()
            set(replaced TRUE)
            file(READ "${source}" original)
            string(REPLACE "\r\n" "\n" original "${original}")
            foreach(clock DATE TIME)
                string(REGEX MATCHALL "__${clock}__" occurrences "${original}")
                list(LENGTH occurrences count)
                if(NOT count EQUAL 1)
                    message(FATAL_ERROR "Review JUCE compilation clock source before applying source epoch")
                endif()
            endforeach()
            string(REPLACE "__DATE__" "\"${source_date}\"" replacement "${original}")
            string(REPLACE "__TIME__" "\"${source_time}\"" replacement "${replacement}")
            set(overlay "${CMAKE_CURRENT_BINARY_DIR}/generated/juce_core_CompilationTime.cpp")
            file(CONFIGURE OUTPUT "${overlay}" CONTENT "${replacement}" @ONLY)
            list(APPEND mapped_sources
                "$<IF:$<AND:$<CONFIG:Release>,$<BOOL:$<TARGET_PROPERTY:SINGLE_EDITOR_REPRODUCIBLE_PRODUCT>>>,${overlay},${source}>")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
        else()
            list(APPEND mapped_sources "${source}")
        endif()
    endforeach()
    if(NOT replaced)
        message(FATAL_ERROR "JUCE compilation clock source was not found")
    endif()
    set_property(TARGET juce_core PROPERTY INTERFACE_SOURCES "${mapped_sources}")
    foreach(product_target IN ITEMS SingleEditor SingleEditorCLI)
        set_property(TARGET ${product_target} PROPERTY SINGLE_EDITOR_REPRODUCIBLE_PRODUCT TRUE)
        # MSVC includes source paths in unnamed-namespace identities. The
        # existing build-local JUCE overlays must have the same logical path in
        # independent build directories; #line alone does not provide this.
        target_compile_options(${product_target} PRIVATE
            "$<$<CONFIG:Release>:/experimental:deterministic>"
            "$<$<CONFIG:Release>:/pathmap:${CMAKE_CURRENT_BINARY_DIR}=SingleEditorBuild>")
        # Keep linker evidence outside the shipping artifact directory. A map
        # records final groups and symbol origins; it is not a runtime catalog
        # or an estimate of optimized symbol sizes.
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/link-evidence")
        target_link_options(${product_target} PRIVATE "$<$<CONFIG:Release>:/Brepro>"
            "$<$<CONFIG:Release>:/MAP:${CMAKE_CURRENT_BINARY_DIR}/link-evidence/${product_target}.map>")
    endforeach()
    message(STATUS "Single Editor: Release source epoch ${SINGLE_EDITOR_SOURCE_DATE_EPOCH}; shared JUCE unchanged")
endfunction()
