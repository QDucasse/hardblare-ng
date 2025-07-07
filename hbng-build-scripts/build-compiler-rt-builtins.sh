#!/bin/bash

cmake -G Ninja \
  -S compiler-rt/lib/builtins \
  -B ../build-aarch64-compiler-rt-builtins \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-musl \
  -DCMAKE_ASM_COMPILER_TARGET=aarch64-linux-musl \
  -DCMAKE_C_COMPILER=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/clang \
  -DCMAKE_ASM_COMPILER=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/clang \
  -DCMAKE_ASM_FLAGS="-mcpu=cortex-a53" \
  -DCMAKE_C_FLAGS="-mcpu=cortex-a53" \
  -DCMAKE_SYSROOT=/home/aurora/instr-musl/sysroot \
  -DCMAKE_AR=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/llvm-ar \
  -DCMAKE_RANLIB=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/llvm-ranlib \
  -DCMAKE_C_COMPILER_WORKS=1 \
  -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCOMPILER_RT_USE_BUILTINS_LIBRARY=TRUE \
  -DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=FALSE
