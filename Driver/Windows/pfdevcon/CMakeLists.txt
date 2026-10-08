# ParsFlash USB Redirector - pfdevcon (installs/removes the pfvhci virtual USB host controller)
# https://parsflash.com
#
# Included from the top-level project with add_subdirectory(); ${PROJECT_SOURCE_DIR}
# is the repository root. Windows (x64) only.

if(WIN32)
    add_executable(pfdevcon pfdevcon.cpp)

    target_compile_features(pfdevcon PRIVATE cxx_std_20)
    target_compile_definitions(pfdevcon PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(pfdevcon PRIVATE ${PROJECT_SOURCE_DIR}/Shared/Driver)
    target_link_libraries(pfdevcon PRIVATE setupapi newdev cfgmgr32)

    if(MSVC)
        target_compile_options(pfdevcon PRIVATE /W4 /permissive- /utf-8)
    elseif(MINGW)
        # wmain() entry point
        target_compile_options(pfdevcon PRIVATE -Wall -Wextra -municode)
        target_link_options(pfdevcon PRIVATE -municode)
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(WARNING "pfdevcon must be built for x64: UpdateDriverForPlugAndPlayDevices fails under WOW64")
    endif()
endif()
