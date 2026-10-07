cmake_minimum_required(VERSION 3.24)

project(ParsFlashUsbRedirector
    VERSION 1.0.0
    DESCRIPTION "ParsFlash USB Redirector - USB over IP for mobile repair"
    HOMEPAGE_URL "https://parsflash.com"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PF_BUILD_TESTS "Build automated tests" ON)
option(PF_BUILD_GUI "Build Qt 6 GUI applications (auto-disabled if Qt 6 is missing)" ON)
option(PF_BUILD_TOOLS "Build benchmark and log tools" ON)
option(PF_WITH_LIBUSB "Build the libusb server backend" ON)
option(PF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PF_DEVELOPMENT_MODE "Development build: no license required" ON)
set(PF_BUILD_NUMBER "0" CACHE STRING "Build number stamped into binaries")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PfCompilerOptions)
include(PfDependencies)

enable_testing()

add_subdirectory(Shared)
add_subdirectory(Server)
add_subdirectory(Client)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Driver/Windows/pfdevcon/CMakeLists.txt")
    add_subdirectory(Driver/Windows/pfdevcon)
endif()
if(PF_BUILD_TOOLS)
    add_subdirectory(Tools)
endif()
if(PF_BUILD_TESTS)
    add_subdirectory(Tests)
endif()

message(STATUS "ParsFlash ${PROJECT_VERSION}: GUI=${PF_HAVE_QT} libusb=${PF_HAVE_LIBUSB} tests=${PF_BUILD_TESTS}")
