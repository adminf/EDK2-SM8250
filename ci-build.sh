#!/bin/bash
set -e
sudo apt update
sudo apt install -y build-essential uuid-dev iasl git nasm gcc-aarch64-linux-gnu bc abootimg python3-distutils
curdir="$PWD"
cd ..
git clone https://github.com/tianocore/edk2.git --branch edk2-stable202302 --recursive
git clone https://github.com/tianocore/edk2-platforms.git
cd "$curdir"
./firstrun.sh
./build.sh
