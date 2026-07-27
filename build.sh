# Clean and build script
rm -rf CMakeCache.txt CMakeFiles/ build/
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/NDS.cmake ..
make
