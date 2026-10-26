add_library(ed_ui STATIC ui.cpp)
target_link_libraries(ed_ui PUBLIC ed_dna ed_core imgui)
