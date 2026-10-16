cmake_minimum_required(VERSION 3.20)

project(OpenDX VERSION 4.4.4 LANGUAGES C)

# The sources predate C23 (bool keyword, no implicit declarations); pin the
# dialect GCC 11 uses today so a newer compiler cannot change it.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)

# compile_commands.json for clangd and other editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(DX_BUILD_EXEC "Build the executive (dxexec)" ON)
# Off by default: it downloads Dear ImGui, GLFW and noodles at configure time.
option(DX_BUILD_UI "Build the new user interface (dxui2)" OFF)

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(DX_ARCH linux)
else()
  message(FATAL_ERROR "OpenDX's CMake build supports Linux only so far "
                      "(see docs/diary); got ${CMAKE_SYSTEM_NAME}")
endif()

include(cmake/DXConfigChecks.cmake)

# Every target sees the generated dxconfig.h before the source tree's include/.
set(DX_GENERATED_INCLUDE ${CMAKE_BINARY_DIR}/include)
set(DX_CODEGEN ${CMAKE_SOURCE_DIR}/tools/codegen)

if(DX_BUILD_EXEC)
  add_subdirectory(src/exec)
endif()

if(DX_BUILD_UI)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  add_subdirectory(src/uipp/dxl)
  add_subdirectory(src/ui)
endif()
