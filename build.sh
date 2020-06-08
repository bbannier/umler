#!/bin/bash

LLVM_BRANCH=${LLVM_BRANCH:-master}

# get llvm & clang deps
git clone --branch ${LLVM_BRANCH} --single-branch --depth=1 https://github.com/llvm/llvm-project.git llvm-project

# add ourselves to the build
git clone . llvm-project/clang-tools-extra/umler
echo 'add_subdirectory(umler)' >> llvm-project/clang-tools-extra/CMakeLists.txt

# build the tool
mkdir llvm-project/build
pushd llvm-project/build
cmake ../llvm
make -j`nproc` umler
