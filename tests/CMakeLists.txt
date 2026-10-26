# Engine-free tests for src/dna (no Panda3D needed). ctest, or: dna_tests <samples dir> <tmp dir>
add_executable(dna_tests dna_tests.cpp)
target_link_libraries(dna_tests PRIVATE ed_dna)
add_test(NAME dna_formats COMMAND dna_tests ${CMAKE_CURRENT_SOURCE_DIR}/samples ${CMAKE_CURRENT_BINARY_DIR}/tmp)
