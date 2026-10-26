add_library(imgui STATIC imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp
            imgui/imgui_demo.cpp imgui/backends/imgui_impl_opengl3.cpp imguizmo/ImGuizmo.cpp)
target_include_directories(imgui PUBLIC imgui imguizmo)
if(NOT WIN32)
  target_link_libraries(imgui PUBLIC dl)
endif()
