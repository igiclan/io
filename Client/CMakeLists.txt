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
