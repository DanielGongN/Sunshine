# Sunshine 项目综合说明文档

> **版本**: 基于 master 分支源码分析
> **许可证**: GPL-3.0-only
> **官方文档**: https://docs.lizardbyte.dev/projects/sunshine

---

## 目录

- [一、项目概述](#一项目概述)
- [二、架构总览](#二架构总览)
- [三、模块功能详解](#三模块功能详解)
  - [3.1 核心模块](#31-核心模块)
  - [3.2 协议与网络模块](#32-协议与网络模块)
  - [3.3 编码与媒体模块](#33-编码与媒体模块)
  - [3.4 平台抽象层](#34-平台抽象层)
  - [3.5 基础设施与工具库](#35-基础设施与工具库)
- [四、如何编译运行](#四如何编译运行)
  - [4.1 编译环境要求](#41-编译环境要求)
  - [4.2 Windows 构建步骤](#42-windows-构建步骤)
  - [4.3 Linux 构建步骤](#43-linux-构建步骤)
  - [4.4 macOS 构建步骤](#44-macos-构建步骤)
  - [4.5 Docker 构建](#45-docker-构建)
  - [4.6 运行方式](#46-运行方式)
- [五、第三方依赖清单](#五第三方依赖清单)
- [六、集成到自有产品的指南](#六集成到自有产品的指南)
  - [6.1 整体集成策略](#61-整体集成策略)
  - [6.2 模块化裁剪方案](#62-模块化裁剪方案)
  - [6.3 关键改造点](#63-关键改造点)
  - [6.4 开箱即用的最小集成方案](#64-开箱即用的最小集成方案)
- [七、注意事项](#七注意事项)
  - [7.1 许可证合规（最重要）](#71-许可证合规最重要)
  - [7.2 安全注意事项](#72-安全注意事项)
  - [7.3 性能注意事项](#73-性能注意事项)
  - [7.4 兼容性注意事项](#74-兼容性注意事项)
  - [7.5 维护注意事项](#75-维护注意事项)

---

## 一、项目概述

**Sunshine** 是一个开源的自托管游戏串流主机（Host），与 [Moonlight](https://moonlight-stream.org/) 客户端配合使用，实现低延迟的远程游戏/桌面串流。它本质上是 NVIDIA GameStream 协议的开源实现，但不依赖 NVIDIA GeForce Experience（GFE），支持 AMD、Intel、NVIDIA 三家 GPU 的硬件编码，也支持纯软件编码。

### 核心能力

| 能力 | 说明 |
|------|------|
| **视频编码** | H.264 / HEVC / AV1，支持 8bit/10bit、4:2:0/4:4:4、HDR |
| **硬件加速** | NVENC (NVIDIA)、QSV (Intel)、AMF (AMD)、VideoToolbox (macOS)、VAAPI (Linux) |
| **音频编码** | Opus 编码，支持立体声 / 5.1 / 7.1 环绕声 |
| **输入转发** | 键盘、鼠标、手柄（Xbox/DualShock/DualSense/Switch Pro）、触摸屏 |
| **屏幕捕获** | DXGI/WGC (Windows)、X11/Wayland/KMS/NvFBC (Linux)、AVFoundation (macOS) |
| **网络传输** | RTP over UDP + AES-GCM 加密 + Reed-Solomon FEC 纠错 |
| **Web 管理** | HTTPS Web UI 支持配置管理、客户端配对 |
| **跨平台** | Windows、Linux、macOS、FreeBSD（实验性） |

---

## 二、架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                        Moonlight Client                         │
│               (Android / iOS / PC / TV / ...)                   │
└───────────────────────────┬─────────────────────────────────────┘
                            │ GameStream Protocol (HTTPS + RTSP + RTP)
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Sunshine Host                              │
│                                                                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────────────┐ │
│  │ nvhttp   │ │  rtsp    │ │ stream   │ │   confighttp       │ │
│  │(配对/信息)│ │(信令协商) │ │(数据传输) │ │  (Web UI管理)      │ │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────────────────────┘ │
│       │            │            │                               │
│  ┌────▼────────────▼────────────▼──────────────────────┐        │
│  │              核心引擎层                              │        │
│  │  ┌───────┐ ┌───────┐ ┌───────┐ ┌────────┐          │        │
│  │  │ video │ │ audio │ │ input │ │ crypto │          │        │
│  │  │(编码)  │ │(编码)  │ │(注入)  │ │(加密)   │          │        │
│  └──┤───────┤─┤───────┤─┤───────┤─┤────────┤──────────┘        │
│     │       │ │       │ │       │ │        │                    │
│  ┌──▼───────▼─▼───────▼─▼───────▼─▼────────▼──────────┐        │
│  │              平台抽象层 (platf)                       │        │
│  │  display_t │ audio_control_t │ input_t │ misc       │        │
│  └──┬─────────┴─────────┬───────┴─────────┴────────────┘        │
│     │                   │                                       │
│  ┌──▼───────┐  ┌────────▼────────┐  ┌────────────────┐         │
│  │ Windows  │  │     Linux       │  │     macOS      │         │
│  │ DXGI/WGC │  │ X11/Wl/KMS/CUDA│  │  AVFoundation  │         │
│  │ WASAPI   │  │ PulseAudio      │  │  CoreAudio     │         │
│  │ ViGEm    │  │ inputtino       │  │  IOKit         │         │
│  └──────────┘  └─────────────────┘  └────────────────┘         │
│                                                                 │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 基础设施: thread_pool | task_pool | mail | config | log   │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 数据流概要

1. **配对阶段**: Moonlight 客户端通过 `nvhttp` 模块（HTTPS）发送 PIN 配对请求，`crypto` 模块验证并交换证书
2. **信令阶段**: 客户端通过 `rtsp` 模块协商视频分辨率/帧率/编码格式、音频通道数、加密密钥等参数
3. **串流阶段**: `stream` 模块启动视频/音频/控制三个 UDP 通道：
   - **视频通道**: 平台层捕获屏幕 → `video` 硬件/软件编码 → RTP 封包 + FEC → AES-GCM 加密 → UDP 发送
   - **音频通道**: 平台层捕获音频 → `audio` Opus 编码 → 加密 → UDP 发送
   - **控制通道**: 接收客户端输入 → `input` 解析 → 平台层注入键鼠/手柄事件
4. **管理面**: `confighttp` 提供 HTTPS Web UI，用户可通过浏览器管理配置、查看状态

---

## 三、模块功能详解

### 3.1 核心模块

#### `main.cpp` / `main.h` — 程序入口

程序主入口。负责：
- 解析命令行参数（`creds`、`help`、`version` 等子命令）
- 初始化配置、日志系统、显示设备、平台层、网络 HTTP 等核心子系统
- 注册 `SIGINT` / `SIGTERM` 信号处理器实现优雅关闭
- Windows 上创建隐藏窗口监听 `WM_ENDSESSION` 消息、修改 NVIDIA 控制面板设置
- 启动所有子服务（nvhttp、confighttp、RTSP、UPnP、系统托盘等）
- 进入主事件循环等待关闭信号

#### `config.cpp` / `config.h` — 配置管理

管理 Sunshine 的全部配置选项，从配置文件解析并填充结构体。核心配置子结构：

| 子结构 | 内容 |
|--------|------|
| `video_t` | NVENC/QSV/AMF/VT/VAAPI 各平台编码参数、显示设备配置、色彩空间、最大码率 |
| `audio_t` | 音频设备名称、虚拟设备配置 |
| `stream_t` | FEC 百分比、加密模式、超时设置 |
| `nvhttp_t` | 证书路径、服务名称、访问控制、密钥路径 |
| `input_t` | 按键映射、手柄类型（DS4/Switch/Xbox）、触控设置 |

#### `globals.cpp` / `globals.h` — 全局资源

定义进程级全局共享资源：
- `task_pool` — 全局线程池
- `display_cursor` — 光标显示标志
- `nvprefs_instance` — Windows NVIDIA 控制面板配置单例
- `mail` 命名空间 — 进程级邮件通信系统，提供命名通道：
  - `shutdown` / `broadcast_shutdown` — 关闭信号
  - `video_packets` / `audio_packets` — 媒体数据包
  - `switch_display` — 显示器切换
  - `touch_port` — 触摸端口
  - `idr` — IDR 帧请求
  - `gamepad_feedback` — 手柄反馈
  - `hdr` — HDR 状态切换

#### `entry_handler.cpp` / `entry_handler.h` — 入口处理

处理命令行子命令和应用程序生命周期管理：
- `args::creds` — 重置用户凭据
- `args::help` — 打印帮助信息
- `args::version` — 打印版本
- `launch_ui()` — 生成 Web UI HTTPS URL 并打开浏览器
- `lifetime::exit_sunshine()` — 通过 `SIGINT` 触发优雅退出

#### `process.cpp` / `process.h` — 进程管理

管理串流会话启动的应用程序生命周期：
- 支持预处理命令（`prep_cmds`）、分离进程（`detached`）、主进程命令（`cmd`）
- 可配置工作目录、输出重定向、提权运行
- 实现优雅终止（带超时的进程组退出）和强制终止
- 自动检测工作目录（解析命令路径或 PATH 查找）
- 从 JSON 文件加载应用列表

#### `logging.cpp` / `logging.h` — 日志系统

基于 Boost.Log 的分级异步日志：
- 6 个日志级别：`verbose(0)`、`debug(1)`、`info(2)`、`warning(3)`、`error(4)`、`fatal(5)`
- 异步 text_ostream sink 输出到文件和 stdout
- 集成 FFmpeg `av_log` 和 `display_device` 库日志回调

#### `system_tray.cpp` / `system_tray.h` — 系统托盘

跨平台系统托盘图标和通知：
- 菜单项：打开 Web UI、捐赠链接、重置显示设备、重启、退出
- 根据串流状态动态切换图标（空闲/播放中/暂停/锁定）
- 桌面通知（播放/暂停/停止状态变化时触发）

---

### 3.2 协议与网络模块

#### `nvhttp.cpp` / `nvhttp.h` — GameStream HTTP 协议

模拟 NVIDIA GameStream 协议的 HTTP/HTTPS 服务器：
- 版本伪装为 GFE 3.23.0.74，协议版本 7.1.431.-1
- 实现客户端 PIN 配对、服务器信息查询、应用列表、流会话启停等端点
- TLS 双向认证，管理已配对客户端证书链
- 是 Moonlight 客户端发现和连接 Sunshine 的入口

#### `rtsp.cpp` / `rtsp.h` — RTSP 协议

RTSP 信令服务器：
- 处理 `ANNOUNCE` / `SETUP` / `PLAY` 等 RTSP 消息
- 管理 `launch_session_t`：GCM 密钥/IV、视频分辨率/帧率、HDR/SOPS 设置、App ID 等
- 支持 AES-GCM 加密的 RTSP 消息
- 管理活跃会话计数

#### `stream.cpp` / `stream.h` — 流传输

核心流传输引擎：
- 管理视频流、音频流和控制流的 UDP 会话
- RTP 数据包封装、AES-GCM 加密传输
- Reed-Solomon 前向纠错（FEC）抗丢包
- 控制消息类型：IDR 请求、参考帧失效、输入数据、HDR 切换、手柄震动等（共 16+ 种）
- 会话生命周期：`alloc` / `start` / `stop` / `join`

#### `network.cpp` / `network.h` — 网络工具

IP 地址分类与管理：
- 地址分类：PC（localhost）、LAN（私有网段）、WAN
- IPv4/IPv6 双栈支持
- 基于 ENet 的 UDP 主机管理
- 端口映射（基于基础端口的偏移计算）
- 地址归一化（IPv4-mapped IPv6 → IPv4）

#### `upnp.cpp` / `upnp.h` — UPnP 端口映射

使用 miniupnpc 自动配置路由器：
- 自动发现 IGD 设备
- 为 RTSP、视频、音频、控制、HTTP/HTTPS、Web UI 端口创建映射
- 支持 IPv6 Pinhole
- 映射生存期 3600 秒，120 秒刷新
- 退出时自动清理映射

#### `crypto.cpp` / `crypto.h` — 密码学

基于 OpenSSL 的完整加密基础设施：
- SHA-256 哈希、AES 密钥生成（基于 salt + PIN）
- X.509 证书操作（生成、解析、PEM 导入导出）
- RSA 签名/验签
- `cert_chain_t` 证书链验证
- 三种 AES 加密模式：ECB（传统配对）、GCM（流加密）、CBC

#### `httpcommon.cpp` / `httpcommon.h` — HTTP 公共基础

为 nvhttp 和 confighttp 提供共享基础设施：
- SSL 证书/私钥生成与加载
- 用户凭据管理（用户名 + 加盐 SHA-256 哈希，JSON 存储）
- 唯一设备 ID 生成（UUID）
- 文件下载（libcurl）、URL 转义/解析

#### `confighttp.cpp` / `confighttp.h` — Web UI 管理服务器

基于 Simple-Web-Server 的 HTTPS 管理界面：
- 用户认证（用户名/密码）、CSRF 令牌保护
- 静态文件服务、目录浏览
- RESTful API：应用配置管理、设置修改、客户端管理
- 安全头：X-Frame-Options、CSP

---

### 3.3 编码与媒体模块

#### `video.cpp` / `video.h` — 视频编码

基于 FFmpeg (libavcodec) 的视频捕获与编码：

| 特性 | 支持 |
|------|------|
| 编码格式 | H.264、HEVC、AV1 |
| 色深 | 8-bit、10-bit |
| 色度采样 | 4:2:0、4:4:4 |
| 硬件编码器 | NVENC、QSV、AMF、VideoToolbox、VAAPI |
| 色彩空间 | Rec.601、Rec.709、BT.2020 SDR、BT.2020 HDR |

管理编码器探测、初始化、参数配置，定义编码器平台格式抽象层（`encoder_platform_formats_t`）。

#### `audio.cpp` / `audio.h` — 音频捕获与编码

使用 Opus 多流编码器：
- 支持立体声、5.1、7.1 环绕声（各有普通/高质量两档，共 6 种配置）
- 采样率 48kHz
- 码率范围 96kbps（立体声）~ 2048kbps（高质量 7.1）
- 低延迟模式（`OPUS_APPLICATION_RESTRICTED_LOWDELAY`）

#### `video_colorspace.cpp` / `video_colorspace.h` — 色彩空间管理

视频编码色彩空间转换：
- 支持 Rec.601、Rec.709、BT.2020 SDR、BT.2020 HDR
- Sunshine 色彩空间 → FFmpeg `AVColor*` 参数转换
- RGB→YUV 颜色转换矩阵（UNORM/UINT 两种输出范围）

#### `cbs.cpp` / `cbs.h` — FFmpeg 码流处理

基于 FFmpeg Coded Bitstream (CBS) API：
- 操作 H.264 和 HEVC 的 NAL 单元
- 提取/修改 SPS（序列参数集）和 VPS（视频参数集）
- 注入 VUI（视频可用性信息）参数
- `validate_sps()` 验证色彩空间和 HDR 元数据

#### `stat_trackers.cpp` / `stat_trackers.h` — 统计跟踪

轻量级流媒体统计收集：
- `min_max_avg_tracker<T>` 模板类
- 在指定时间间隔内收集最小值/最大值/平均值
- 跟踪帧延迟、网络丢包率等性能指标

#### `input.cpp` / `input.h` — 输入处理

处理 Moonlight 客户端所有用户输入：
- 键盘、鼠标、手柄、触摸屏、手写笔
- 通过 moonlight-common-c 解析输入数据包
- 虚拟键码转换、手柄分配管理（最多 `MAX_GAMEPADS` 个）
- 触摸坐标缩放、椭圆接触区域处理

#### `display_device.cpp` / `display_device.h` — 显示设备管理

串流期间的显示配置管理：
- 自动切换分辨率、刷新率、HDR 状态
- 设备准备模式（验证/激活/主显示器/唯一显示器）
- 模式重映射（客户端请求 → 实际支持模式）
- 配置持久化（崩溃恢复）

#### NVENC 子模块 (`src/nvenc/`)

NVIDIA NVENC 硬件编码器的专用实现：

| 文件 | 用途 |
|------|------|
| `nvenc_base.h/cpp` | NVENC 编码器基类 |
| `nvenc_d3d11.h/cpp` | D3D11 纹理直接输入 |
| `nvenc_d3d11_native.h/cpp` | D3D11 原生路径 |
| `nvenc_d3d11_on_cuda.h/cpp` | D3D11 纹理通过 CUDA 转给 NVENC |
| `nvenc_config.h` | 配置参数 |
| `nvenc_colorspace.h` | 色彩空间处理 |

---

### 3.4 平台抽象层

平台抽象统一定义在 `src/platform/common.h` 的 `platf` 命名空间中，通过虚基类定义接口。

#### 屏幕捕获（`display_t`）

| 平台 | 后端 | 说明 |
|------|------|------|
| Windows | DXGI (RAM) | Desktop Duplication → 系统内存 |
| Windows | DXGI (VRAM) | Desktop Duplication → GPU 显存 (D3D11) |
| Windows | WGC | Windows Graphics Capture API |
| Linux | X11Grab | X11 屏幕抓取 |
| Linux | WlGrab | Wayland wlr-screencopy |
| Linux | KMSGrab | 内核 DRM/KMS 直接抓帧 |
| Linux | PortalGrab | XDG Desktop Portal + PipeWire |
| Linux | CUDA/NvFBC | NVIDIA 帧缓冲捕获 |
| macOS | AVFoundation | AVCaptureScreenInput |

#### 音频捕获（`audio_control_t` / `mic_t`）

| 平台 | 后端 |
|------|------|
| Windows | WASAPI |
| Linux | PulseAudio |
| macOS | CoreAudio + AudioToolbox |

#### 输入注入（`input_t`）

| 平台 | 后端 | 手柄模拟 |
|------|------|----------|
| Windows | Win32 API + ViGEmClient | Xbox 360 / DualShock 4 |
| Linux | inputtino + libevdev | Xbox One / DualSense / Switch Pro |
| macOS | IOKit / CoreGraphics | 不支持手柄 |

#### 服务发布（mDNS/Bonjour）

| 平台 | 实现 |
|------|------|
| Windows | mDNS |
| Linux | Avahi 或 mDNS |
| macOS | Bonjour |

---

### 3.5 基础设施与工具库

#### `task_pool.h` — 任务池
延迟任务和即时任务的调度池。支持 `push()` 提交即时任务、`pushDelayed()` 提交延时任务，任务返回 `std::future`。

#### `thread_pool.h` — 线程池
继承 `TaskPool`，创建多工作线程，通过条件变量调度。提供 `start()` / `stop()` / `join()` 生命周期管理。

#### `thread_safe.h` — 线程安全数据结构
- `event_t<T>` — 单值事件（raise/pop/view）
- `alarm_raw_t<T>` — 一次性闹钟通知
- `queue_t<T>` — 有界阻塞队列
- `shared_t<T>` — 引用计数共享对象
- `mail_raw_t` — 进程级邮箱系统

#### `round_robin.h` — 轮询迭代器
循环迭代器模板，用于轮询分配资源或循环缓冲区。

#### `sync.h` — 同步包装器
`sync_t<T, M>` 将任意类型与互斥锁绑定，自动加锁保护。

#### `utility.h` — 通用工具库（~1070 行）
- `safe_ptr` / `safe_ptr_v2` — 自定义析构智能指针
- `FailGuard` — RAII 失败守卫
- `buffer_t<T>` — 动态字节缓冲区
- `hex_t` — 十六进制转换
- `Either<T,E>` — Result 类型
- 大小端转换、元组解构宏

#### `uuid.h` — UUID 生成
RFC 4122 随机 UUID 生成，输出标准带连字符格式。

#### `move_by_copy.h` — 移动-拷贝适配器
`MoveByCopy<T>` 在拷贝构造时执行移动，用于将 move-only 对象传给只接受可拷贝类型的 API。

#### `rswrapper.h` / `rswrapper.c` — Reed-Solomon 纠错
nanors 库的 SIMD 优化封装，运行时自动选择最优指令集（SSSE3/AVX2/AVX512/NEON）。

#### `file_handler.cpp` / `file_handler.h` — 文件操作
文件系统操作封装：目录创建、文件读写。

---

## 四、如何编译运行

### 4.1 编译环境要求

| 要求 | 版本 |
|------|------|
| CMake | ≥ 3.25 |
| C++ 标准 | C++23 |
| GCC | ≥ 14 |
| Clang | ≥ 17 |
| Apple Clang | ≥ 15 |
| Node.js | 需要（Web UI 前端构建） |
| Git | 需要（子模块管理） |

### 4.2 Windows 构建步骤

> Windows 使用 MSYS2 + MinGW 工具链。**不支持交叉编译**。

#### 1) 安装 MSYS2

从 https://www.msys2.org 下载安装。AMD64 使用 **UCRT64** 终端，ARM64 使用 **CLANGARM64** 终端。

#### 2) 更新包管理器

```bash
pacman -Syu
```

#### 3) 设置工具链变量

```bash
# AMD64:
export TOOLCHAIN="ucrt-x86_64"

# ARM64:
export TOOLCHAIN="clang-aarch64"
```

#### 4) 安装依赖

```bash
dependencies=(
  "git"
  "mingw-w64-${TOOLCHAIN}-boost"
  "mingw-w64-${TOOLCHAIN}-cmake"
  "mingw-w64-${TOOLCHAIN}-cppwinrt"
  "mingw-w64-${TOOLCHAIN}-curl-winssl"
  "mingw-w64-${TOOLCHAIN}-miniupnpc"
  "mingw-w64-${TOOLCHAIN}-onevpl"
  "mingw-w64-${TOOLCHAIN}-openssl"
  "mingw-w64-${TOOLCHAIN}-opus"
  "mingw-w64-${TOOLCHAIN}-toolchain"
)
pacman -S "${dependencies[@]}"

# 仅 AMD64 额外安装：
pacman -S mingw-w64-${TOOLCHAIN}-MinHook mingw-w64-${TOOLCHAIN}-nodejs mingw-w64-${TOOLCHAIN}-nsis
```

#### 5) 克隆与构建

```bash
git clone https://github.com/lizardbyte/sunshine.git --recurse-submodules
cd sunshine
mkdir build
cmake -B build -G Ninja -S .
ninja -C build
```

#### 6) 打包

```bash
# NSIS 安装包
cpack -G NSIS --config ./build/CPackConfig.cmake

# WiX 安装包（需 .NET SDK）
cpack -G WIX --config ./build/CPackConfig.cmake

# 便携 ZIP
cpack -G ZIP --config ./build/CPackConfig.cmake
```

### 4.3 Linux 构建步骤

参考 `scripts/linux_build.sh` 安装发行版对应依赖（支持 Debian/Fedora/Arch），然后：

```bash
git clone https://github.com/lizardbyte/sunshine.git --recurse-submodules
cd sunshine && mkdir build
cmake -B build -G Ninja -S .
ninja -C build
```

**KMS 捕获需额外设置权限：**
```bash
sudo setcap cap_sys_admin,cap_sys_nice+p ./build/sunshine
```

### 4.4 macOS 构建步骤

```bash
# Homebrew 安装依赖
brew install boost cmake miniupnpc ninja node openssl@3 opus pkg-config

git clone https://github.com/lizardbyte/sunshine.git --recurse-submodules
cd sunshine && mkdir build
cmake -B build -G Ninja -S .
ninja -C build
```

### 4.5 Docker 构建

项目提供多个 Dockerfile（位于 `docker/` 目录）：
- `debian-trixie.dockerfile`
- `ubuntu-22.04.dockerfile`
- `ubuntu-24.04.dockerfile`
- `clion-toolchain.dockerfile`（CLion 远程工具链）

### 4.6 运行方式

```bash
# 直接运行
./sunshine

# 指定配置文件
./sunshine /path/to/sunshine.conf

# 重置凭据
./sunshine --creds {username} {password}

# 显示帮助
./sunshine --help

# 显示版本
./sunshine --version
```

运行后：
1. Sunshine 启动 HTTPS 管理界面（默认端口 47990）
2. 在浏览器访问 `https://localhost:47990` 进行配置
3. 首次使用需设置管理员用户名和密码
4. 在 Moonlight 客户端中添加主机 IP，输入 PIN 码完成配对
5. 即可开始串流

---

## 五、第三方依赖清单

### 通用依赖（所有平台）

| 依赖 | 用途 |
|------|------|
| **OpenSSL** | TLS/SSL 加密、证书管理、HTTPS |
| **Boost** (Log/ASIO/Process/Filesystem 等) | 异步 I/O、日志、进程管理、正则 |
| **FFmpeg** (预编译) | H.264/HEVC/AV1 编码，像素格式转换 |
| **Opus** (libopus) | 低延迟音频编码 |
| **libcurl** | HTTP 客户端 |
| **miniupnpc** | UPnP NAT 穿透 |
| **nlohmann_json** | JSON 解析/生成 |
| **moonlight-common-c** (子模块，含 ENet) | Moonlight 协议实现 + 可靠 UDP |
| **Simple-Web-Server** (子模块) | 轻量 HTTP/HTTPS 服务器 |
| **libdisplaydevice** (子模块) | 显示设备管理 |
| **nanors** | Reed-Solomon FEC |
| **nv-codec-headers** | NVIDIA Video Codec SDK 头 |
| **tray** | 系统托盘 |
| **Node.js + npm + Vite** | Web UI 前端构建 |

### Windows 专有

| 依赖 | 用途 |
|------|------|
| **MinHook** / **minhook-detours** | API Hook |
| **ViGEmClient** / **ViGEmBus** | 虚拟手柄驱动 |
| **NVAPI** | NVIDIA 驱动 API |
| **C++/WinRT (cppwinrt)** | WinRT API |
| **OneVPL** | Intel QSV 编码 |
| **D3D11 / DXGI** | 屏幕捕获 + GPU 编码 |

### Linux 专有

| 依赖 | 用途 |
|------|------|
| **libdrm** | KMS 屏幕捕获 |
| **libcap** | Linux 权限管理 |
| **libva (VA-API)** | 硬件编码 |
| **X11 / Wayland / PipeWire** | 屏幕捕获 |
| **CUDA (≥12.0)** | NvFBC + NVENC |
| **PulseAudio** | 音频捕获 |
| **inputtino / libevdev** | 虚拟输入 |
| **AppIndicator / libnotify** | 系统托盘 |

### macOS 专有

| 依赖 | 用途 |
|------|------|
| **AVFoundation / CoreMedia** | 屏幕捕获 |
| **VideoToolbox** | 硬件编码 |
| **CoreAudio / AudioToolbox** | 音频捕获 |
| **TPCircularBuffer** | 音频缓冲 |

---

## 六、集成到自有产品的指南

### 6.1 整体集成策略

Sunshine 是一个**完整的独立应用**，而非设计为可嵌入的库。将其集成到自有产品中，有以下几种策略：

#### 策略 A：作为独立进程部署（推荐）

```
你的产品 ──启动/配置──▶ Sunshine 进程 ◀──串流──▶ Moonlight 客户端
```

- **最简单**：将 Sunshine 作为子进程或服务启动
- 通过修改配置文件 (`sunshine.conf`) 预设参数
- 通过 HTTPS API (`confighttp`) 进行运行时管理
- 不需要修改 Sunshine 源码

#### 策略 B：源码级集成（深度定制）

将 Sunshine 源码模块化拆分，只取需要的部分编译进你的产品：
- 需要深入理解模块依赖关系
- 需要重写部分初始化逻辑
- 维护成本较高，但灵活度最大

#### 策略 C：Fork + 定制（折中方案）

Fork Sunshine 仓库，在其基础上修改定制：
- 修改品牌信息、Web UI、默认配置
- 添加自有功能模块
- 需要持续跟踪上游更新

### 6.2 模块化裁剪方案

根据需求裁剪不需要的模块：

| 如果不需要... | 可以移除 / 禁用 |
|---------------|-----------------|
| Web UI 管理界面 | `confighttp` 模块 + 前端构建 (Node.js/Vite) |
| UPnP 自动端口映射 | `upnp` 模块 + miniupnpc 依赖 |
| 系统托盘 | `system_tray` 模块 + tray 依赖 |
| 特定屏幕捕获后端 | 平台层中对应的 display 实现 |
| 特定音频后端 | 平台层中对应的 audio 实现 |
| 特定手柄模拟 | 平台层中对应的 input 实现 |
| macOS 支持 | 整个 `platform/macos` 目录 |
| Linux 支持 | 整个 `platform/linux` 目录 |

**不可裁剪的核心**：`stream`、`video`、`audio`、`input`、`crypto`、`nvhttp`、`rtsp`、`network`、`config`

### 6.3 关键改造点

#### 6.3.1 品牌定制

修改以下位置更换品牌：
- `CMakeLists.txt` 中的 `project()` 名称和描述
- `src/confighttp.cpp` 中的 Web UI 标题
- `src_assets/` 目录中的图标和前端资源
- `src/system_tray.cpp` 中的菜单文本
- 配置文件中的 `sunshine_name` 默认值

#### 6.3.2 协议兼容性

Sunshine 实现的是 **NVIDIA GameStream 协议**，客户端必须是 Moonlight 或兼容客户端。如果要：
- **对接自有客户端**：需要实现 GameStream 协议栈，或修改 `nvhttp` / `rtsp` / `stream` 模块
- **继续使用 Moonlight**：保持协议层不变，仅修改上层功能

#### 6.3.3 配置预设

创建预配置的 `sunshine.conf` 实现开箱即用：

```ini
# 核心配置示例
sunshine_name = YourProduct
min_log_level = info

# 编码配置
encoder = nvenc          # 或 qsv / amdvce / vaapi / software
min_fps_factor = 1

# 网络配置
port = 47989             # 基础端口
address_family = both    # ipv4 / ipv6 / both

# 安全配置
origin_web_ui_allowed = lan  # pc / lan / wan

# 禁用不需要的功能
upnp = off
```

#### 6.3.4 自动化部署

- 使用 CMake 的 `cpack` 生成安装包
- Windows：NSIS 或 WiX 安装向导
- Linux：DEB/RPM/Flatpak 包
- 打包时预置配置文件和证书

### 6.4 开箱即用的最小集成方案

最快速的集成方式：

```
步骤 1: Fork Sunshine 仓库
步骤 2: 修改品牌信息 (CMakeLists.txt + 前端资源)
步骤 3: 创建预配置的 sunshine.conf
步骤 4: 编写启动脚本/服务包装器
步骤 5: 使用 CPack 生成安装包
步骤 6: 在你的产品中以子进程方式启动 Sunshine
```

**启动集成示例（C++）：**

```cpp
#include <boost/process.hpp>

namespace bp = boost::process;

class SunshineManager {
public:
    void start(const std::string& config_path) {
        child_ = bp::child(
            "sunshine", config_path,
            bp::std_out > bp::null,
            bp::std_err > "sunshine_error.log"
        );
    }

    void stop() {
        if (child_.running()) {
            child_.terminate();
            child_.wait();
        }
    }

    bool is_running() const { return child_.running(); }

private:
    bp::child child_;
};
```

**通过 API 管理（HTTP 调用）：**

```python
import requests

# 获取配置
resp = requests.get("https://localhost:47990/api/config",
                    auth=("admin", "password"), verify=False)

# 获取已配对客户端列表
resp = requests.get("https://localhost:47990/api/clients",
                    auth=("admin", "password"), verify=False)
```

---

## 七、注意事项

### 7.1 许可证合规（最重要）

> ⚠️ **Sunshine 使用 GPL-3.0-only 许可证**

这意味着：

| 场景 | 是否允许 | 条件 |
|------|----------|------|
| 内部使用（不分发） | ✅ | 无限制 |
| 作为独立进程调用（不修改源码） | ✅ | 需保留 LICENSE、注明使用 |
| 修改源码后分发 | ✅ | **必须以 GPL-3.0 开源你的修改** |
| 将源码编译进闭源产品 | ❌ | GPL 传染性要求整个产品开源 |
| 以独立程序打包分发（不链接） | ✅ | 需附带 GPL-3.0 许可证文本和源码（或获取方式） |

**建议**：
- 如果产品是**闭源商业软件**，使用**策略 A**（独立进程），确保不发生代码链接
- 分发时必须附带 Sunshine 的 LICENSE 文件和源码获取方式
- 需法律顾问评估 GPL 合规性

### 7.2 安全注意事项

1. **证书管理**：首次运行会自动生成自签名 SSL 证书。生产部署建议使用正规 CA 证书
2. **凭据保护**：Web UI 密码以加盐 SHA-256 存储在 JSON 文件中，确保文件权限安全
3. **网络暴露**：
   - 默认 `origin_web_ui_allowed = lan`，不要暴露到公网
   - UPnP 会自动映射端口到公网，需评估风险
   - 建议在防火墙后部署
4. **AES-GCM 加密**：流数据使用 AES-128-GCM 加密，密钥在 RTSP 握手时协商
5. **PIN 配对**：使用 4 位 PIN 码，安全性有限，配对后通过证书认证

### 7.3 性能注意事项

1. **GPU 编码优先**：软件编码 CPU 占用极高，生产环境务必使用硬件编码
2. **编码器选择优先级**（通常）：
   ```
   NVENC > QSV > AMF > VAAPI > VideoToolbox > Software
   ```
3. **FEC 开销**：默认 FEC 百分比会增加约 20% 带宽，可根据网络质量调整
4. **屏幕捕获延迟**：
   - Windows: DXGI VRAM 最优（零拷贝），RAM 次之，WGC 最差
   - Linux: KMS 最优，NvFBC 次之，X11/Wayland 有额外拷贝
5. **内存占用**：典型使用约 200-500MB，视分辨率和编码器而定
6. **网络带宽**：
   - 1080p60 约需 15-50 Mbps
   - 4K60 约需 50-150 Mbps
   - 受编码器和画面复杂度影响

### 7.4 兼容性注意事项

1. **Moonlight 版本**：确保客户端版本与 Sunshine 协议版本兼容
2. **GPU 驱动**：
   - NVIDIA：需要较新驱动支持 NVENC
   - Intel：需要 OneVPL 运行时
   - AMD：需要 AMF 运行时
3. **Windows 版本**：需要 Windows 10 及以上（WGC 需要 Windows 10 1903+）
4. **Linux 内核**：KMS 捕获需要 DRM 权限（`cap_sys_admin`）
5. **防火墙端口**：需开放以下端口（基于默认配置 port=47989）:

   | 端口 | 协议 | 用途 |
   |------|------|------|
   | 47989 | TCP | HTTP |
   | 47984 | TCP | HTTPS |
   | 47990 | TCP | Web UI |
   | 48010 | TCP | RTSP |
   | 47998 | UDP | 视频流 |
   | 47998 | UDP | 控制流 |
   | 47999 | UDP | 控制流 (备) |
   | 48000 | UDP | 音频流 |
   | 48002 | UDP | 麦克风流 |

### 7.5 维护注意事项

1. **子模块管理**：项目使用大量 Git 子模块（moonlight-common-c、Simple-Web-Server、libdisplaydevice 等），克隆时必须 `--recurse-submodules`
2. **FFmpeg 版本**：Sunshine 使用预编译的 FFmpeg，升级时需确保 API 兼容性
3. **上游同步**：如果 Fork 开发，建议定期合并上游更新以获取安全补丁
4. **前端构建**：Web UI 使用 Vue3 + Vite，修改前端需要 Node.js 环境
5. **测试**：项目使用 CTest 框架，测试代码在 `tests/` 目录
6. **CI/CD**：参考 `.github/workflows/ci.yml` 设置持续集成
7. **编码器探测**：Sunshine 启动时会自动探测可用编码器，日志中可查看探测结果

---

## 附录：项目目录结构速查

```
Sunshine/
├── CMakeLists.txt          # 主构建配置
├── LICENSE                 # GPL-3.0 许可证
├── NOTICE                  # 第三方版权声明
├── package.json            # Web UI 前端依赖
├── vite.config.js          # Vite 构建配置
├── cmake/                  # CMake 构建脚本
│   ├── dependencies/       # 依赖查找与配置
│   ├── compile_definitions/# 编译宏定义
│   ├── targets/            # 构建目标定义
│   ├── packaging/          # 打包配置
│   ├── macros/             # CMake 宏
│   └── prep/               # 构建准备（版本、选项）
├── src/                    # C++ 源码
│   ├── main.cpp            # 程序入口
│   ├── stream.cpp          # 流传输核心
│   ├── video.cpp           # 视频编码
│   ├── audio.cpp           # 音频编码
│   ├── input.cpp           # 输入处理
│   ├── crypto.cpp          # 加密
│   ├── nvhttp.cpp          # GameStream 协议
│   ├── rtsp.cpp            # RTSP 信令
│   ├── config.cpp          # 配置管理
│   ├── confighttp.cpp      # Web UI 服务器
│   ├── network.cpp         # 网络工具
│   ├── process.cpp         # 进程管理
│   ├── upnp.cpp            # UPnP 端口映射
│   ├── nvenc/              # NVENC 编码器
│   └── platform/           # 平台抽象层
│       ├── common.h        # 统一接口定义
│       ├── windows/        # Windows 实现
│       ├── linux/          # Linux 实现
│       └── macos/          # macOS 实现
├── src_assets/             # 静态资源（图标、前端、配置模板）
├── tests/                  # 测试代码
├── third-party/            # 第三方库子模块
├── docs/                   # 文档源文件
├── docker/                 # Docker 构建文件
├── scripts/                # 构建辅助脚本
└── packaging/              # 发行包配置
```

---

*本文档基于 Sunshine 源码静态分析生成，具体行为以实际运行版本为准。*
