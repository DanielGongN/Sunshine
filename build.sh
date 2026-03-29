cd build
# 1. 清除旧的 CMake 缓存
rm CMakeCache.txt

# 2. 重新配置，显式指定链接器搜索路径
cmake -G "Ninja" \
  -DCMAKE_EXE_LINKER_FLAGS="-LE:/WorkComp/msys64/mingw64/lib" \
  -DSUNSHINE_BUILD_ASSETS=OFF ..

# 3. 再次运行编译
ninja