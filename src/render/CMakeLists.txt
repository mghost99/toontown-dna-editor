add_library(ed_render STATIC scene.cpp views.cpp gizmo.cpp walker.cpp)
target_link_libraries(ed_render PUBLIC ed_assets ed_dna ed_core panda3d imgui)
