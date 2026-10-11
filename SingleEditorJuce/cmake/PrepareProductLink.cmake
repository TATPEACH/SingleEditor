# Remove only the generated private PDB immediately before a Release link.
# Published candidates and the immutable same-link archives are outside this path.
if(NOT CONFIGURATION STREQUAL "Release")
    return()
endif()
if(NOT PRODUCT_TARGET MATCHES "^(SingleEditor|SingleEditorCLI)$"
        OR NOT EXISTS "${BUILD_ROOT}/CMakeCache.txt")
    message(FATAL_ERROR "Expected a configured product build")
endif()
get_filename_component(expected_pdb
    "${BUILD_ROOT}/link-evidence/${PRODUCT_TARGET}/Release/${PRODUCT_TARGET}.pdb" ABSOLUTE)
get_filename_component(actual_pdb "${PRIVATE_PDB}" ABSOLUTE)
if(NOT actual_pdb STREQUAL expected_pdb)
    message(FATAL_ERROR "Refusing a PDB outside the configured private product output")
endif()
file(REMOVE "${actual_pdb}")
if(EXISTS "${actual_pdb}")
    message(FATAL_ERROR "Could not prepare a fresh private Release PDB")
endif()
