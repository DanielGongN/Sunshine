
echo off
chcp 65001 >nul

@echo off
setlocal EnableDelayedExpansion

set MSYS2_ROOT=E:\WorkComp\msys32
set MSYS2_MINGW64=%MSYS2_ROOT%\mingw64
set MSYS2_USR=%MSYS2_ROOT%\usr\bin

REM ── 安装 MSYS2 mingw-w64-x86_64 依赖（幂等，已装则跳过）────────────────
if not exist "%MSYS2_MINGW64%\bin\pkgconf.exe" (
    echo [INFO] Installing required MSYS2 mingw-w64-x86_64 packages via pacman ...
    "%MSYS2_USR%\bash.exe" -lc "pacman -S --noconfirm --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf mingw-w64-x86_64-openssl mingw-w64-x86_64-curl mingw-w64-x86_64-opus mingw-w64-x86_64-miniupnpc mingw-w64-x86_64-boost mingw-w64-x86_64-cppwinrt"
    if errorlevel 1 (
        echo [ERROR] pacman install failed.
        exit /b 1
    )
)

REM ── 确保 MSYS2 mingw64/bin 在本次 PATH 最前 ────────────────────────────
set GRAPHVIZ_ROOT=E:\WorkComp\Graphviz
set PATH=%GRAPHVIZ_ROOT%\bin;%MSYS2_MINGW64%\bin;%MSYS2_USR%;%PATH%

REM ── 清理旧缓存（避免工具链切换残留）───────────────────────────────────
if exist build\CMakeCache.txt del /f /q build\CMakeCache.txt
if exist build\CMakeFiles rmdir /s /q build\CMakeFiles

REM ── CMake 配置（MSYS2 mingw-w64-x86_64 工具链）─────────────────────────
set VCPKG_MINGW=E:/work/c++/vcpkg/installed/x64-mingw-static

cmake -B build -G Ninja -S . ^
    -DCMAKE_C_COMPILER="%MSYS2_MINGW64%/bin/gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%MSYS2_MINGW64%/bin/g++.exe" ^
    -DCMAKE_PREFIX_PATH="%MSYS2_MINGW64%;%VCPKG_MINGW%" ^
    -DPKG_CONFIG_EXECUTABLE="%MSYS2_MINGW64%/bin/pkgconf.exe" ^
    -DOPENSSL_ROOT_DIR="%MSYS2_MINGW64%" ^
    -DMINHOOK_LIBRARY="%VCPKG_MINGW%/lib/minhook.x64.a" ^
    -DMINHOOK_INCLUDE_DIR="%VCPKG_MINGW%/include" ^
    -DDOXYGEN_DOT_EXECUTABLE="%GRAPHVIZ_ROOT%/bin/dot.exe"

if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

REM ── 编译 ─────────────────────────────────────────────────────────────────
cmake --build build --parallel