# .net visual program reader for the new UI. Standard library only, so it
# builds and tests without the GUI dependencies.

add_library(dx_netfile STATIC NetFile.cpp)
target_include_directories(dx_netfile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dx_netfile PUBLIC cxx_std_17)

# net_check FILE... parses each file; --dump FILE prints nodes and links.
add_executable(net_check net_check.cpp)
target_link_libraries(net_check PRIVATE dx_netfile)
