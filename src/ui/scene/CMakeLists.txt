# Scene files from the executive's DxuiScene module (ADR 0003). No GL, so
# scene_check runs headless.

add_library(dx_scene STATIC SceneFile.cpp)
target_include_directories(dx_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)
target_compile_features(dx_scene PUBLIC cxx_std_17)

add_executable(scene_check scene_check.cpp)
target_link_libraries(scene_check PRIVATE dx_scene)
