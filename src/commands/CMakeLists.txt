add_library(ed_commands STATIC commands.cpp)
target_link_libraries(ed_commands PUBLIC ed_dna)
