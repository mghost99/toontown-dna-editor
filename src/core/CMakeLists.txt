add_library(ed_core STATIC log.cpp resconfig.cpp)
target_include_directories(ed_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
