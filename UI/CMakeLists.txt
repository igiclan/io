# Shared Qt helpers for the desktop applications (only built when Qt 6 is available).
if(PF_HAVE_QT)
    add_library(pf_ui_kit STATIC Kit/UiKit.cpp)
    target_include_directories(pf_ui_kit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pf_ui_kit PUBLIC Qt6::Widgets pf_shared)
    set_target_properties(pf_ui_kit PROPERTIES AUTOMOC ON)
endif()
