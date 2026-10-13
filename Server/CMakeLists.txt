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

add_executable(parsflash-server App/ServerMain.cpp)
target_link_libraries(parsflash-server PRIVATE pf_server_core)

if(PF_HAVE_QT)
    # GUI: Qt 6 Widgets front end over the engine (no engine logic lives here).
    add_library(pf_server_ui STATIC UI/Strings.cpp UI/ServerWindow.cpp)
    target_include_directories(pf_server_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pf_server_ui PUBLIC pf_server_core pf_ui_kit Qt6::Widgets)
    set_target_properties(pf_server_ui PROPERTIES AUTOMOC ON)
    add_executable(parsflash-server-gui WIN32 App/ServerGuiMain.cpp)
    target_link_libraries(parsflash-server-gui PRIVATE pf_server_ui)
    set_target_properties(parsflash-server-gui PROPERTIES AUTOMOC ON)
endif()
