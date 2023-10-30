#!/usr/bin/env bash
git submodule update --init --recursive
cd SPS-Variants/APS-AllInOne
mkdir build
cd build
cmake ..
make -j32
