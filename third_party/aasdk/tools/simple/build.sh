#!/bin/bash
set -e

BUILD_TYPE=${1:-Release}
BUILD_DIR=build-${BUILD_TYPE,,}

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE ..
make -j$(nproc)

cd ..
echo "Build complete. Binary is in $BUILD_DIR/aasdk_simple_headunit"
