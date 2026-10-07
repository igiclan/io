#!/usr/bin/env bash
# Builds the Windows x64 dependency sysroot used by the MinGW cross build:
#   OpenSSL 3 (static) and libusb-1.0 (static), plus links to header-only libraries
#   (asio, nlohmann/json, doctest) installed on the host.
# Usage: Tools/ci/setup-mingw-deps.sh [/opt/pf-mingw]
set -euo pipefail
PREFIX=${1:-/opt/pf-mingw}
WORK=$(mktemp -d)
OPENSSL_VER=3.0.15
LIBUSB_VER=1.0.27
mkdir -p "$PREFIX/include"
cd "$WORK"
curl -fsSL -o openssl.tgz "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"
curl -fsSL -o libusb.tbz "https://github.com/libusb/libusb/releases/download/v${LIBUSB_VER}/libusb-${LIBUSB_VER}.tar.bz2"
mkdir ossl usb
tar xzf openssl.tgz -C ossl --strip-components=1
tar xjf libusb.tbz -C usb --strip-components=1
(cd usb && ./configure --host=x86_64-w64-mingw32 --prefix="$PREFIX" --enable-static --disable-shared \
    CC=x86_64-w64-mingw32-gcc-posix && make -j"$(nproc)" && make install)
(cd ossl && ./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- no-shared no-tests \
    --prefix="$PREFIX" --libdir=lib && make -j"$(nproc)" build_libs && make install_dev)
for h in asio asio.hpp nlohmann doctest; do
    [ -e "$PREFIX/include/$h" ] || ln -s "/usr/include/$h" "$PREFIX/include/$h"
done
rm -rf "$WORK"
echo "MinGW sysroot ready in $PREFIX"
