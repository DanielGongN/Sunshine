# Gateway HTTPS Framing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 HTTPS 回传帧长溢出，并让握手超时日志显示首帧进度和双向流量。

**Architecture:** 将 HTTPS 回传读取上限限制为 `0xFFFF`。共享隧道状态记录实际接收与转发进度，在异步操作释放状态后输出最终汇总。

**Tech Stack:** C++23、Boost.Asio、Boost.Log、gtest、Windows MSYS2 UCRT64。

**Spec:** `docs/superpowers/specs/2026-10-28-gateway-https-framing-design.md`

## Global Constraints

- 网关协议仍为 `[stream_id:1][payload_length:2 BE][payload]`。
- 网关帧头不计入 TLS 流量。
- 不改变现有内部连接时机、TLS 超时、RTSP 和 UDP 行为。
- 编译及测试通过 `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c` 执行。
- 验证后创建中文 `fix:` 本地提交，只暂存本任务文件。

## Task 1: 修复帧长度并记录隧道诊断

**Files:** 修改 `src/gateway.cpp`、`tests/unit/test_gateway.cpp`。

**Interfaces:** 保持 `gateway::start()` 不变；HTTPS 私有读取方法改为传递共享隧道状态。

- [x] 为真实 Gateway 回环连接增加大块回传回归，逐帧断言 `0 < length <= 65535`，拼接后等于原始数据。
- [x] 在旧代码运行回归，确认大块回传产生零长度帧（修复前的 TCP 回环诊断已复现）。
- [x] 将 HTTPS 回传缓冲改为 `std::array<char, 0xFFFF>`，保持一读一帧。
- [x] 增加共享状态：`first_header_bytes`、`first_frame_length_known`、`first_frame_declared`、`first_frame_received`、`client_to_internal_bytes`、`internal_to_client_bytes`。
- [x] 读取首帧时用 completion condition 记录进度：`return asio::transfer_all()(ec, received);`。完成回调再次保存接收量以覆盖取消路径。
- [x] 写完成回调按实际完成量累计，回传方向扣除最多 3 字节的帧头。首个关闭原因只写一次；状态析构时输出一次最终计数。
- [x] 增加未收齐首帧及不完整长度头的回归，断言关闭汇总中的实际接收量。
- [x] 使用 `.clang-format` 规则检查改动；构建 `test_sunshine` 并运行 `Gateway*` 测试。

## Task 2: TLS 验证与提交

**Files:** 复用忽略构建目录中的 TLS 诊断脚本；更新本计划的完成状态。

**Interfaces:** 从当前生产源码提取 TCP 函数，回环 TLS 服务端与客户端完成握手及数据回传。

- [x] 验证 TLS 1.2/1.3、有无客户端证书、拆帧与合并发送以及 512 KiB 回传均通过。
- [x] 验证声明 4096 字节但只发送部分 payload 的用例保留约 5 秒超时，并在日志显示准确的首帧接收量及零转发量。
- [x] 检查 `git diff --check` 和暂存差异，只提交生产代码、回归测试和这两份说明。
- [x] 创建 `fix: 修复HTTPS回传帧长度溢出并补充握手诊断`，正文编号说明修复、日志和验证。
