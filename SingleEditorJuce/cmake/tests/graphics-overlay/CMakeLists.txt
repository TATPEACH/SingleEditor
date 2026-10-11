cmake_minimum_required(VERSION 3.24)
project(SingleEditorGraphicsOverlayTest NONE)
add_library(juce_graphics INTERFACE)
target_sources(juce_graphics INTERFACE
    "${JUCE_GRAPHICS_TEST_ROOT}/juce_graphics.cpp"
    "${CMAKE_CURRENT_LIST_FILE}")
include(../../SingleEditorJuceGraphics.cmake)
single_editor_configure_juce_graphics()
get_target_property(actual_sources juce_graphics INTERFACE_SOURCES)
file(WRITE "${CMAKE_BINARY_DIR}/selected-sources.txt" "${actual_sources}")
