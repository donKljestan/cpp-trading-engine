#!/bin/bash

set -e 

echo "Clean old build directory..."
rm -rf build

echo "Configuring CMake..."
cmake -B build -S .

echo "Compiling..."
cmake --build build

echo "Build successfully!"
echo ""
echo "Starting..."
echo ""
./build/minisdk_demo

echo ""
echo "Finish!"
