# DXLink: the C library an application uses to drive dxexec over its socket
# protocol. The new UI links it. x11.c (Xt main-loop glue) and os2.c are left
# out; the UI runs its own event loop.

add_library(dx_dxl STATIC
    close.c conn.c connect.c dict.c execute.c exit.c handlers.c load.c
    open.c query.c save.c send.c sequencer.c socket.c value.c windows.c
    object.c)
target_include_directories(dx_dxl
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${DX_GENERATED_INCLUDE})
target_compile_definitions(dx_dxl PRIVATE HAVE_CONFIG_H ${DX_ARCH} _GNU_SOURCE)
set_target_properties(dx_dxl PROPERTIES POSITION_INDEPENDENT_CODE ON)
