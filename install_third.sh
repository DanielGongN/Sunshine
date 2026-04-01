# 核心工具链 (Essential Toolchain)
pacman -S mingw-w64-x86_64-toolchain \
          mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-ninja --noconfirm

# 基础开发库 (Core Libraries)
pacman -S mingw-w64-x86_64-boost \
          mingw-w64-x86_64-opus \
          mingw-w64-x86_64-minhook \
          mingw-w64-x86_64-libvpl \
          mingw-w64-x86_64-openssl \
          mingw-w64-x86_64-curl --noconfirm

# 多媒体与串流依赖 (Multimedia & Streaming)
pacman -S mingw-w64-x86_64-ffmpeg --noconfirm

# 前端与文档工具 (WebUI & Documentation)

pacman -S mingw-w64-x86_64-nodejs \
          mingw-w64-x86_64-doxygen --noconfirm


# 可选的性能分析工具 (Optional Profiling Tools)
pacman -S --noconfirm mingw-w64-x86_64-libvpl

# 可选的图形化工具 (Optional Graphical Tools)
pacman -S --noconfirm mingw-w64-x86_64-graphviz