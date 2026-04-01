# 1. 清除旧的 CMake 缓存
# 如果存在 CMakeCache.txt 文件，删除它以确保新的配置生效
if [ -f build/CMakeCache.txt ]; then
    echo "Removing old CMake cache..."
    rm build/CMakeCache.txt
fi

# 2. 重新配置，显式指定链接器搜索路径
cmake -B build -G Ninja -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON。

# 3. 再次运行编译
ninja -C build
