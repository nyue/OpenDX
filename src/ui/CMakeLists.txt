# The new user interface (ADR 0001): Dear ImGui + noodles + GLFW, driving
# dxexec through DXLink.

include(${CMAKE_SOURCE_DIR}/cmake/UiDependencies.cmake)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/net/CMakeLists.txt)
  add_subdirectory(net)
endif()
add_subdirectory(scene)

add_library(dx_execclient STATIC exec/ExecClient.cpp exec/NetScript.cpp)
target_include_directories(dx_execclient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/exec)
find_package(Threads REQUIRED)
target_link_libraries(dx_execclient PRIVATE dx_dxl PUBLIC Threads::Threads)

add_executable(exec_check exec/exec_check.cpp)
target_link_libraries(exec_check PRIVATE dx_execclient)

# The application. GraphView renders with noodles (which loads GL through
# GLEW); ImGui uses its own loader.
add_executable(dxui2 app/main.cpp app/GraphView.cpp app/ImagePanel.cpp app/SceneView.cpp)
target_include_directories(dxui2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(dxui2 PRIVATE dx_execclient dx_netfile dx_scene dx_imgui noodles)
target_compile_definitions(dxui2 PRIVATE
  DXUI_NOODLES_ASSETS="${NOODLES_ASSETS_DIR}"
  DXUI_SOURCE_UI_MDF="${CMAKE_SOURCE_DIR}/src/uipp/ui/ui.mdf")

# The legacy UI's module definitions for UI-side nodes (Image, Transmitter,
# Input/Output, ...), where the legacy install puts them.
install(FILES ${CMAKE_SOURCE_DIR}/src/uipp/ui/ui.mdf DESTINATION dx/ui)

# Warnings on for the new code (not for fetched dependencies or DXLink).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(target dx_netfile net_check dx_scene scene_check dx_execclient exec_check dxui2)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endforeach()
endif()
