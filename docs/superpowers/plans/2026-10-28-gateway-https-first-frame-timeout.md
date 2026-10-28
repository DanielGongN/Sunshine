# Gateway HTTPS First Frame Timeout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 先收齐合法 HTTPS 首帧，再开始内部连接和 TLS 握手计时，保持现有客户端兼容。

**Architecture:** 在共享隧道状态中增加独立首帧定时器和阶段时间点。首帧完成回调取消定时器并连接内部服务；写入首帧和读取回传独立启动，后续帧沿用现有连接。

**Tech Stack:** C++23、Boost.Asio、Boost.Log、gtest、MSYS2 UCRT64。

**Spec:** `docs/superpowers/specs/2026-10-28-gateway-https-first-frame-timeout-design.md`

## Global Constraints

- 客户端无需修改，协议继续使用 `[stream_id:1][payload_length:2 BE][payload]`。
- 首帧接收固定 10 秒；NVHTTP 内部 TLS 握手维持原有 5 秒。
- 合法网关首帧不要求包含完整 ClientHello。
- 构建目录前缀为 `cmake-build-`，通过项目指定 MSYS2 UCRT64 命令执行构建与测试。
- 验证后创建中文 `fix:` 本地提交，只包含本任务文件。

## Task 1: 首帧状态与回归

**Files:** 修改 `src/gateway.cpp`、`tests/unit/test_gateway.cpp`。

**Interfaces:** `gateway::start()` 不变；HTTPS 私有函数增加连接和写 payload 的帮助函数，共享状态保存目标端口、首帧定时器和阶段时间点。

- [x] 增加完整首帧到达前无内部连接和空帧不连接的测试，在旧代码上确认失败。
- [x] 增加状态字段与 `asio::steady_timer`，在 `0x01` 入口执行 `expires_after(std::chrono::seconds(10))`。
- [x] 定时回调通过 weak_ptr 获取状态，检查关闭和完成标记后记录 `event=first_frame_timeout`，调用 `close("first_frame_timeout", asio::error::timed_out)`。
- [x] 首帧接收成功先设置完成标记和时间点，再取消定时器、连接内部 HTTPS 并调用双向转发。
- [x] 保持首帧原样写入一次，后续帧继续使用原内部连接；日志累计 payload 字节数。
- [x] 调整不完整首帧测试为独立接收超时，断言无内部接入、接收进度及关闭原因。
- [x] 格式化并构建 `test_sunshine`，运行全部 `Gateway*` 测试；10 项连续两轮通过。

## Task 2: TLS 验证与提交

**Files:** 使用忽略构建目录中的回环脚本，更新本计划和设计验证结果。

**Interfaces:** 从最终生产源码提取 TCP 函数进行真实 TLS 回环，客户端协议实现保持相同格式。

- [x] TLS 1.2/1.3、有无客户端证书、拆帧及大块回传通过。
- [x] 首帧延迟 6 秒仍完成内部 5 秒预算下的握手。
- [x] 未收齐首帧约 10 秒关闭，无内部连接，并能再次成功建立 TLS 连接。
- [x] 检查 `.clang-format`、暂存空白和提交范围，仅包含本任务生产代码、测试和说明。
- [x] 创建 `fix: 分离HTTPS首帧接收与TLS握手超时` 本地提交，正文编号记录改动与验证。
