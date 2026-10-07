# Common compiler settings applied through the pf_options interface target.
add_library(pf_options INTERFACE)

if(MSVC)
    target_compile_options(pf_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
    if(PF_WARNINGS_AS_ERRORS)
        target_compile_options(pf_options INTERFACE /WX)
    endif()
    target_compile_definitions(pf_options INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(pf_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
        -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference -Wformat=2)
    if(PF_WARNINGS_AS_ERRORS)
        target_compile_options(pf_options INTERFACE -Werror)
    endif()
endif()

if(WIN32)
    # Windows 10 is the minimum supported OS.
    target_compile_definitions(pf_options INTERFACE
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

target_compile_definitions(pf_options INTERFACE
    PF_VERSION_STRING="${PROJECT_VERSION}"
    PF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PF_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    PF_BUILD_NUMBER=${PF_BUILD_NUMBER})
if(PF_DEVELOPMENT_MODE)
    target_compile_definitions(pf_options INTERFACE PF_DEVELOPMENT_MODE=1)
endif()

if(MINGW)
    # Static runtime so binaries run under Wine/Windows without extra DLLs.
    target_link_options(pf_options INTERFACE -static -static-libgcc -static-libstdc++)
endif()
