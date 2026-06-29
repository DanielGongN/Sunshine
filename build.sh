#!/bin/bash
set -e  # 遇到错误立即退出

# 👈 ✨ 针对 MINGW64 环境的强制编码清洗
export LANG=zh_CN.UTF-8
export LC_ALL=zh_CN.UTF-8
export OUTPUT_CHARSET=utf-8

# 强制将 Windows 宿主工具的输出流在控制台中转换为 UTF-8
if [ -n "$COMSPEC" ]; then
    export PYTHONIOENCODING=utf-8
fi

rm -rf build

# 0. 创建 build 目录（如果不存在）
mkdir -p build
cd build

# 1. 清除旧的 CMake 缓存
rm -f CMakeCache.txt

# 2. 重新配置，显式指定链接器搜索路径   -DSUNSHINE_BUILD_ASSETS=OFF \
cmake -G "Ninja" \
  -DCMAKE_EXE_LINKER_FLAGS="-LE:/WorkComp/msys64/mingw64/lib" \
  -DSUNSHINE_ENABLE_TRAY=OFF ..

# 3. 再次运行编译
ninja