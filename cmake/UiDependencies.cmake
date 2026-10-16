# Third-party code for the new UI (ADR 0001), fetched at configure time.
#
#   noodles  node-graph renderer: the author's fork, branch wasm-port, pinned
#            by commit. The pin is shared with the Meta-Noodles research repo
#            (cmake/NoodlesDependency.cmake there); move them together.
#            -DNOODLES_SOURCE_DIR=<checkout> builds a local checkout instead.
#   imgui    Dear ImGui, docking branch, pinned by tag.
#   glfw     the system package when found (3.3 or newer), else fetched.

include(FetchContent)

set(NOODLES_GIT_REPOSITORY "https://github.com/nyue/noodles.git"
    CACHE STRING "noodles git remote (the wasm-port fork)")
set(NOODLES_GIT_TAG "13e27edb3e6ab951eac9df43702d7b5d0187e92e"
    CACHE STRING "noodles commit; keep in step with the research pin")

# noodles guards its tests on BUILD_TESTING, and still calls
# FetchContent_Populate itself (CMP0169) with old-style URL fetches (CMP0135).
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
# noodles defaults to a shared library; the UI links it statically.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(NOODLES_SOURCE_DIR)
  add_subdirectory(${NOODLES_SOURCE_DIR} ${CMAKE_BINARY_DIR}/noodles-build)
  set(NOODLES_CHECKOUT "${NOODLES_SOURCE_DIR}")
else()
  FetchContent_Declare(noodles
    GIT_REPOSITORY ${NOODLES_GIT_REPOSITORY}
    GIT_TAG ${NOODLES_GIT_TAG}   # a sha, so no GIT_SHALLOW
    GIT_PROGRESS TRUE)
  FetchContent_MakeAvailable(noodles)
  set(NOODLES_CHECKOUT "${noodles_SOURCE_DIR}")
endif()
set(NOODLES_ASSETS_DIR "${NOODLES_CHECKOUT}/assets" CACHE PATH
    "noodles' shaders/ and fonts/")

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9-docking
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)
endif()

find_package(OpenGL REQUIRED)

add_library(dx_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(dx_imgui PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(dx_imgui PUBLIC glfw OpenGL::GL)
