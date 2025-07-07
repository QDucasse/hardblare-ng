#!/bin/bash

cmake -G Ninja \
  -S llvm \
  -B build-x86 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_LLD=OFF \