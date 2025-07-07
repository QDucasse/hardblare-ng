#!/bin/bash

cmake -G Ninja \
  -S runtimes \
  -B build-aarch64-libunwind \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DLLVM_ENABLE_RUNTIMES="libunwind" \
  -DCMAKE_C_COMPILER=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/clang \
  -DCMAKE_ASM_COMPILER=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/clang \
  -DCMAKE_LINKER=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/ld.lld \
  -DCMAKE_AR=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/llvm-ar \
  -DCMAKE_RANLIB=/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/bin/llvm-ranlib \
  -DCMAKE_C_FLAGS="--target=aarch64-linux-musl -mcpu=cortex-a53" \
  -DCMAKE_CXX_FLAGS="--target=aarch64-linux-musl -mcpu=cortex-a53" \
  -DCMAKE_EXE_LINKER_FLAGS="--target=aarch64-linux-musl -L/home/aurora/qtests/llvm/hardblare-ng/build-aarch64/build-aarch64-compiler-rt-builtins/lib/linux -lclang_rt.builtins-aarch64" \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-musl \
  -DCMAKE_ASM_COMPILER_TARGET=aarch64-linux-musl \
  -DCMAKE_SYSROOT=/home/aurora/instr-musl/sysroot \
  -DCMAKE_C_COMPILER_WORKS=TRUE \
  -DLIBUNWIND_ENABLE_STATIC=ON \
  -DLIBUNWIND_ENABLE_SHARED=OFF \
  -DLIBUNWIND_USE_COMPILER_RT=ON
