# Build-only private link evidence. Keep disabled until the integrated product
# candidate has completed its independent rebuild and native acceptance gates.
option(SINGLE_EDITOR_ENABLE_LINK_EVIDENCE "Finalize Release products with private same-link contribution evidence" OFF)
option(SINGLE_EDITOR_ENABLE_PRIVATE_SYMBOLS "Retain private optimized Release function symbols for owner reconciliation" OFF)
function(single_editor_configure_link_evidence)
    if(SINGLE_EDITOR_ENABLE_PROVENANCE_PROBE
       AND (NOT SINGLE_EDITOR_ENABLE_LINK_EVIDENCE OR NOT SINGLE_EDITOR_ENABLE_PRIVATE_SYMBOLS))
        message(FATAL_ERROR "Provenance probe requires both link evidence and private symbols; it is not a normal release configuration")
    endif()
    if(NOT SINGLE_EDITOR_ENABLE_LINK_EVIDENCE)
        if(SINGLE_EDITOR_ENABLE_PRIVATE_SYMBOLS)
            message(FATAL_ERROR "Private symbols require finalized same-link evidence")
        endif()
        return()
    endif()
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Same-link evidence currently requires MSVC x64")
    endif()
    set(icf_option "/OPT:ICF")
    if(SINGLE_EDITOR_ENABLE_PROVENANCE_PROBE)
        # One future-build feasibility profile, never proof for F92 or another
        # existing image. JUCE module sources compile within the two products;
        # samplerate and the generated icon library are their compiled inputs.
        # RC-only targets and the separate DIA build tool do not add C/C++ code
        # to the products. Keep ordinary Release optimization and /OPT:REF.
        foreach(probe_target IN ITEMS SingleEditor SingleEditorCLI samplerate SingleEditorCoreActionIcons)
            set_property(TARGET ${probe_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
            target_compile_options(${probe_target} PRIVATE
                "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/GL->"
                "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/GF->")
        endforeach()
        set(icf_option "/OPT:NOICF")
        message(STATUS "Single Editor: diagnostic provenance probe; Release GL/ICF/GF disabled, current-image evidence unchanged")
    endif()
    set(SINGLE_EDITOR_DIA_ROOT "${CMAKE_VS_INSTALLATION_PATH}/DIA SDK" CACHE PATH "Installed Visual Studio DIA SDK")
    set(dia_dll "${SINGLE_EDITOR_DIA_ROOT}/bin/amd64/msdia140.dll")
    if(NOT EXISTS "${dia_dll}")
        message(FATAL_ERROR "Set SINGLE_EDITOR_DIA_ROOT to the installed DIA SDK")
    endif()
    set(release_tools "${CMAKE_CURRENT_SOURCE_DIR}/../tools/release")
    add_subdirectory("${release_tools}/dia" "${CMAKE_CURRENT_BINARY_DIR}/link-evidence/inspector")
    set(tool_identity "")
    foreach(tool IN ITEMS Finalize-ProductLink.mjs Finalize-LinkEvidence.mjs BuildArtifactEvidence.mjs
            dia/DiaSectionContributions.cpp dia/CMakeLists.txt)
        file(SHA256 "${release_tools}/${tool}" tool_hash)
        string(APPEND tool_identity "${tool}:${tool_hash}\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${release_tools}/${tool}")
    endforeach()
    string(SHA256 finalization_revision "${tool_identity}")
    # /Z7 adds build-only symbol records to each object; it does not select Debug,
    # disable optimization/LTCG or change runtime defines. The existing finalizer
    # keeps PDBs private and removes the shipping CodeView/PDB reference. This option
    # stays opt-in until the new output has its own release qualification.
    if(SINGLE_EDITOR_ENABLE_PRIVATE_SYMBOLS)
        foreach(symbol_target IN ITEMS SingleEditor SingleEditorCLI samplerate)
            target_compile_options(${symbol_target} PRIVATE "$<$<CONFIG:Release>:/Z7>")
        endforeach()
    endif()
    foreach(product_target IN ITEMS SingleEditor SingleEditorCLI)
        set(private_root "${CMAKE_CURRENT_BINARY_DIR}/link-evidence/${product_target}")
        file(MAKE_DIRECTORY "${private_root}/Release")
        set_target_properties(${product_target} PROPERTIES
            PDB_OUTPUT_DIRECTORY_RELEASE "${private_root}/Release"
            PDB_NAME_RELEASE "${product_target}")
        add_dependencies(${product_target} SingleEditorDiaInspector)
        if(SINGLE_EDITOR_ENABLE_PROVENANCE_PROBE)
            target_link_options(${product_target} PRIVATE "$<$<CONFIG:Release>:/LTCG:OFF>")
        endif()
        # Including the tool revision in this stable private debug name forces a
        # relink when finalization logic changes, including in Visual Studio.
        target_link_options(${product_target} PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>" "$<$<CONFIG:Release>:/OPT:REF>"
            "$<$<CONFIG:Release>:${icf_option}>" "$<$<CONFIG:Release>:/INCREMENTAL:NO>"
            "$<$<CONFIG:Release>:/PDBALTPATH:${product_target}-${finalization_revision}.pdb>")
        # A full MSVC relink still reuses the old PDB and increments its age.
        # That changes /Brepro image identity even when every object is unchanged.
        # Start each actual Release link with a fresh private PDB; the preceding
        # link and its exact symbols are already retained by the finalizer.
        add_custom_command(TARGET ${product_target} PRE_LINK
            COMMAND "${CMAKE_COMMAND}" "-DCONFIGURATION=$<CONFIG>"
                "-DBUILD_ROOT=${CMAKE_CURRENT_BINARY_DIR}" "-DPRODUCT_TARGET=${product_target}"
                "-DPRIVATE_PDB=$<TARGET_PDB_FILE:${product_target}>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareProductLink.cmake"
            VERBATIM COMMENT "Prepare fresh ${product_target} Release link symbols")
        add_custom_command(TARGET ${product_target} POST_BUILD
            COMMAND "${SINGLE_EDITOR_NODE_EXECUTABLE}" "${release_tools}/Finalize-ProductLink.mjs"
                "$<CONFIG>" "${product_target}" "$<TARGET_FILE:${product_target}>"
                "$<TARGET_PDB_FILE:${product_target}>" "$<TARGET_FILE:SingleEditorDiaInspector>"
                "${dia_dll}" "${private_root}/$<CONFIG>"
            VERBATIM COMMENT "Finalize ${product_target} with private same-link evidence")
    endforeach()
endfunction()
