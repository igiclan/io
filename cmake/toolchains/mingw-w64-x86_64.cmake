# Cross-compile Windows x64 binaries from Linux with MinGW-w64 (posix threads).
# Used for CI verification of all Windows user-mode code; binaries run under Wine.
# Production Windows builds use MSVC (see Docs/BUILD.md).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(PF_MINGW_SYSROOT "/opt/pf-mingw" CACHE PATH "Sysroot with cross-built OpenSSL/libusb and header-only deps")
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 ${PF_MINGW_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(OPENSSL_ROOT_DIR ${PF_MINGW_SYSROOT})
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(ENV{PKG_CONFIG_LIBDIR} "${PF_MINGW_SYSROOT}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

set(CMAKE_CROSSCOMPILING_EMULATOR wine)
