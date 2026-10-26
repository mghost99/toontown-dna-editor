add_library(ed_assets STATIC storage.cpp)
target_link_libraries(ed_assets PUBLIC ed_dna ed_core panda3d)
