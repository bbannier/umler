#!/bin/bash

LLVM_BRANCH=${LLVM_BRANCH:-master}

# get llvm & clang deps
git clone --branch ${LLVM_BRANCH} --single-branch --depth=1 http://llvm.org/git/llvm llvm
git clone --branch ${LLVM_BRANCH} --single-branch --depth=1 http://llvm.org/git/clang llvm/tools/clang
git clone --branch ${LLVM_BRANCH} --single-branch --depth=1 http://llvm.org/git/clang-tools-extra llvm/tools/clang/tools/extra

# add ourselves to the build
git clone . llvm/tools/clang/tools/extra/umler
echo 'add_subdirectory(umler)' >> llvm/tools/clang/tools/extra/CMakeLists.txt

# build the tool
mkdir llvm/build
pushd llvm/build
cmake ..
make -j2 umler
