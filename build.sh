#!/bin/bash
set -e  # 遇到错误立即退出

# 强制将输出编码设为 UTF-8
export LANG=zh_CN.UTF-8
export LC_ALL=zh_CN.UTF-8

# 不需要手动设 PATH，npm_wrapper.cmd 会在 cmd 上下文中设置正确的 Node.js 路径

# 1. 强��删掉之前的 build 残留
rm -rf build

# 2. 重新创建并进入 build 目录
mkdir build && cd build

# 3. 手动绑定 UCRT64 的编译器路径进行配置
cmake -G "Ninja" \
  -DCMAKE_C_COMPILER="E:/WorkComp/msys64/ucrt64/bin/gcc.exe" \
  -DCMAKE_CXX_COMPILER="E:/WorkComp/msys64/ucrt64/bin/g++.exe" \
  -DNPM="E:/work/c++/Sunshine/npm_wrapper.cmd" \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS=OFF \
  ..

# 4. 编译
ninja
