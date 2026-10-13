# pf_client_core: Qt-free client engine (server links, sessions, virtual USB bridge).
add_library(pf_client_core STATIC
    VirtualUSB/VhciRecords.cpp
    VirtualUSB/WinVhci.cpp
    VirtualUSB/TestVhci.cpp
    Sessions/ClientSession.cpp
    Core/ClientEngine.cpp
)
target_include_directories(pf_client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_client_core PUBLIC pf_shared)
if(WIN32)
    target_link_libraries(pf_client_core PUBLIC setupapi)
endif()

add_executable(parsflash-client App/ClientMain.cpp)
target_link_libraries(parsflash-client PRIVATE pf_client_core)

if(PF_HAVE_QT)
    # GUI: Qt 6 Widgets front end over the client engine.
    add_library(pf_client_ui STATIC UI/ClientStrings.cpp UI/ClientWindow.cpp)
    target_include_directories(pf_client_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pf_client_ui PUBLIC pf_client_core pf_ui_kit Qt6::Widgets)
    set_target_properties(pf_client_ui PROPERTIES AUTOMOC ON)
    add_executable(parsflash-client-gui WIN32 App/ClientGuiMain.cpp)
    target_link_libraries(parsflash-client-gui PRIVATE pf_client_ui)
    set_target_properties(parsflash-client-gui PROPERTIES AUTOMOC ON)
endif()
