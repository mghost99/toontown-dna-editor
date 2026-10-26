# DNA XML + text .dna (span-keeping parsers), compiled .pdna, document model, storage selection, catalog, save. No engine dependency.
add_library(ed_dna STATIC xml_span.cpp dna_document.cpp dna_text.cpp dna_binary.cpp)
target_include_directories(ed_dna PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
