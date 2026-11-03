#!/bin/sh
# Builds rb_waveform_core/data/wave-bridge.dylib. Needs only OpenSSL 3 headers (no linking):
# brew install openssl@3, or set OPENSSL_INCLUDE.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../../rb_waveform_core/data/wave-bridge.dylib"
include=${OPENSSL_INCLUDE:-$(brew --prefix openssl@3 2>/dev/null)/include}
version=$(sed -n 's/^version *= *"\(.*\)"/\1/p' "$here/../../pyproject.toml")
clang++ -std=c++17 -O2 -Wall -Wextra -fvisibility=hidden -fexceptions \
    -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -dynamiclib \
    -DWAVE_BRIDGE_VERSION="\"$version\"" -I"$include" \
    -o "$out" "$here/wave_bridge.cpp"
codesign --force --sign - "$out"
echo "$out"
