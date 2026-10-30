# Video Send Diagnostics Implementation Plan

**Goal:** 保持现有发送行为并提供可定位缺包的发送侧统计。
**Architecture:** 会话内累计计数由视频广播线程独占，帧作用域退出时合并并限频输出。
**Tech Stack:** C++23、Boost.Log、MSYS2 UCRT64。
**Spec:** docs/superpowers/specs/2026-10-30-video-send-diagnostics-design.md

## Constraints
保护判断、发送节奏和重试策略保持不变。构建目录前缀 cmake-build-。仅提交本次文件。

## Tasks
- [x] 检查会话保护、恢复状态、批量与单包发送结果及日志级别。
- [x] 在 src/stream.cpp 增加会话累计计数、帧退出汇总、限频异常详情及按秒统计。
- [x] 检查单包返回值，批量成功计入提交，回退独立计数，修正 FEC 日志文案。
- [x] 将 build.sh 的构建目录规范为 cmake-build-release。
- [x] 格式、差异及计数关系检查通过；MSYS2 UCRT64 bash build.sh 全量构建成功；现有恢复与路由测试 12/12 通过（使用已有测试可执行文件）。
- [x] 验证通过后仅暂存本任务文件并创建中文本地提交。
