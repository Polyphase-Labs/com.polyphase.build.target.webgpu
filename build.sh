#!/bin/bash
# Native Addon Build Script for Linux/macOS
# Run this from the root of your addon folder (where package.json is)
#
# Usage: ./build.sh [config]
#   config - Optional. "Debug", "Release", or "Both" (default: Both)
#
# Requirements:
#   - g++ or clang++ installed

ADDON_NAME="com.polyphase.build.target.webgpu"
BUILD_CONFIG="${1:-Both}"

echo ""
echo "========================================"
echo " Building Native Addon: $ADDON_NAME"
echo " Configuration: $BUILD_CONFIG"
echo "========================================"
echo ""

if [ ! -d "Source" ]; then
    echo "ERROR: Source directory not found!"
    exit 1
fi

if command -v g++ &> /dev/null; then
    CXX="g++"
elif command -v clang++ &> /dev/null; then
    CXX="clang++"
else
    echo "ERROR: No C++ compiler found!"
    exit 1
fi

SOURCES=$(find Source -name "*.cpp" -type f)
BUILD_FAILED=0

if [[ "$BUILD_CONFIG" == "Release" ]] || [[ "$BUILD_CONFIG" == "Both" ]]; then
    echo "Building Release configuration..."
    mkdir -p "build/Linux/x64/Release"
    if $CXX -shared -fPIC -O2 -std=c++17 -ISource \
        -DOCTAVE_PLUGIN_EXPORT -DNDEBUG -DPLATFORM_LINUX=1 \
        -o "build/Linux/x64/Release/lib${ADDON_NAME}.so" $SOURCES; then
        echo "Release build succeeded"
        command -v sha256sum &> /dev/null && sha256sum "build/Linux/x64/Release/lib${ADDON_NAME}.so" > "build/Linux/x64/Release/${ADDON_NAME}-Linux-x64-Release.sha256"
    else
        echo "Release build FAILED!"
        BUILD_FAILED=1
    fi
fi

if [[ "$BUILD_CONFIG" == "Debug" ]] || [[ "$BUILD_CONFIG" == "Both" ]]; then
    echo "Building Debug configuration..."
    mkdir -p "build/Linux/x64/Debug"
    if $CXX -shared -fPIC -O0 -g -std=c++17 -ISource \
        -DOCTAVE_PLUGIN_EXPORT -D_DEBUG -DPLATFORM_LINUX=1 \
        -o "build/Linux/x64/Debug/lib${ADDON_NAME}.so" $SOURCES; then
        echo "Debug build succeeded"
        command -v sha256sum &> /dev/null && sha256sum "build/Linux/x64/Debug/lib${ADDON_NAME}.so" > "build/Linux/x64/Debug/${ADDON_NAME}-Linux-x64-Debug.sha256"
    else
        echo "Debug build FAILED!"
        BUILD_FAILED=1
    fi
fi

echo ""
if [ $BUILD_FAILED -eq 1 ]; then
    echo "BUILD COMPLETED WITH ERRORS"
else
    echo "Build Succeeded!"
fi
echo "Output: build/Linux/x64/[Debug|Release]/lib${ADDON_NAME}.so"

exit $BUILD_FAILED
