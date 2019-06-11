#!/usr/bin/env bash

rm -rf build && mkdir build && cd build

cmake .. -G Xcode -DCMAKE_INSTALL_PREFIX=release -DCMAKE_TOOLCHAIN_FILE=../ios.toolchain.cmake -DPLATFORM=OS64COMBINED
cmake --build . --config Release --target install

tar -zcvf libwebsocket.tar.gz -C release ./lib ./include