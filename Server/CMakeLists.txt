# pf_server_core: Qt-free server engine (USB backends, sessions, engine, REST API).
add_library(pf_server_core STATIC
    USB/LoopbackBackend.cpp
    USB/LibusbBackend.cpp
    USB/DeviceInventory.cpp
    Sessions/ServerSession.cpp
    Core/ServerEngine.cpp
    API/RestApi.cpp
)
target_include_directories(pf_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_server_core PUBLIC pf_shared)
if(PF_HAVE_LIBUSB)
    target_link_libraries(pf_server_core PUBLIC pf_libusb)
    target_compile_definitions(pf_server_core PUBLIC PF_HAVE_LIBUSB=1)
endif()
if(WIN32)
    target_link_libraries(pf_server_core PUBLIC setupapi cfgmgr32)
endif()
