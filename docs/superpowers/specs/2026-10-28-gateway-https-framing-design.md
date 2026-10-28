# Gateway HTTPS 帧长度与握手诊断

用户已要求修复 `0x01` HTTPS 回传长度溢出、增加首帧和双向流量日志，并在验证后提交。

## 设计

网关协议仍为 `[stream_id:1][payload_length:2 BE][payload]`。回传读取缓冲限制为 65,535 字节，保证每次读取都能封装为一个合法帧。相比读取 65,536 字节后额外拆帧，这种方式直接利用 TCP 字节流保持完整性，改动更小。

每条 HTTPS 隧道共享一个诊断状态，包括编号、起始时间、首帧头接收量、首帧声明长度、首帧 payload 接收量、双向已转发 payload 字节数和首个关闭原因。异步读的 completion condition 更新首帧进度；异步写的完成回调统计实际写出的字节数，包括错误前已完成的部分。网关帧头不计入 TLS 流量。

连接建立、首帧声明、首帧接收完成和两个方向首次转发使用 Info 日志；后续逐帧转发使用 Debug 日志。最后一个持有状态的异步回调释放后输出一次关闭汇总，避免取消读取后的实际接收量遗漏。日志不记录 payload 内容。

## 边界

不改变现有内部连接时机、TLS 超时、RTSP 和 UDP 行为。无完整帧头时声明长度显示 `unknown`，空帧、异常 stream ID、连接和读写失败均保留关闭原因。所有状态由现有单个 Gateway TCP 工作线程访问。

## 验证

使用 gtest 驱动真实 Gateway 回环连接，验证大于 65,535 字节的回传、多帧请求和字节完整性；模拟首帧 payload 或长度头未收齐时内部连接关闭，断言诊断计数。继续运行现有 Gateway 测试，并通过真实 TLS 1.2/1.3、有无客户端证书和拆帧诊断。只提交本任务文件。

## 验证结果与日志示例

MSYS2 UCRT64 构建 `test_sunshine` 成功，8 项 Gateway gtest 全部通过。基于当前生产 TCP 转发函数的回环诊断通过 TLS 1.2/1.3、有无客户端证书、拆帧及合并发送、512 KiB TLS 回传和 1 MiB 原始回传。

声明 4096 字节而只发送 1521 字节时，内部 TLS 按原有设置约 5 秒超时，关闭汇总包含：

```text
event=closed reason=internal_read first_header_bytes=2 first_declared=4096 first_received=1521 c2i_bytes=0 i2c_bytes=0
```

`id` 关联同一隧道的所有日志；`event=connected internal_local=` 可与 HTTPS 接入日志的本机源端口对应。Info 级别保留首帧事件、首次双向转发和关闭汇总；Debug 级别增加每次转发的字节数与累计值。
