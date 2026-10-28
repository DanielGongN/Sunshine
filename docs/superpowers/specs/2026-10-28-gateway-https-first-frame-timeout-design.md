# Gateway HTTPS 首帧接收与握手分别计时

用户已确认：仅修改服务端，先收齐合法首帧再连接内部 HTTPS，增加首帧接收超时、日志和回归，验证后提交。本设计承接 `2026-10-28-gateway-https-framing-design.md`，调整其中的内部连接时机。

## 方案与兼容性

继续使用 `[stream_id:1][payload_length:2 BE][payload]`；客户端无需修改。`0x01` 已由现有入口读取，随后要求两字节长度完整、payload 长度为 1–65,535 且实际收齐。这里的合法性是网关帧合法性，允许 ClientHello 跨多个帧，不增加 TLS 内容解析。

采用完整首帧缓冲后连接内部服务的方案。相比延长内部 TLS 超时，该方案明确区分客户端首帧等待和 TLS 握手阶段；相比边收边转发，它保持现有按完整网关帧转发的语义。

## 状态与定时器

识别 `0x01` 后立即启动固定 10 秒首帧接收定时器，覆盖剩余帧头和 payload；部分数据到达不延长总预算。收齐首帧后先标记完成并取消定时器，再连接内部 HTTPS，启动双向转发并原样写入首帧一次。

内部 TLS 的 5 秒计时仍由 NVHTTP 在接受内部连接后启动，握手完成后取消。Gateway 不增加连接总时长限制。正常请求可继续复用同一条内部连接。

定时器以 weak_ptr 捕获隧道状态，正常关闭和首帧完成时取消。即使到期回调已经排队，也检查 `closed` 和首帧完成标记，防止误关闭已进入内部握手阶段的连接。首帧失败、空帧、超时均不连接内部 HTTPS。

## 日志

首帧超时输出 Warning：`event=first_frame_timeout`、预算、实际耗时、首帧头实收量、声明长度（未收完整时为 unknown）及 payload 实收量。关闭汇总保留双向字节数，关闭原因设为 `first_frame_timeout`，并增加首帧接收耗时与内部连接阶段耗时。内部阶段耗时是连接存续时间，不冒充实际 TLS 握手耗时。

连接接入先记录 `event=first-frame-wait`；首帧完成后记录 `event=connect`，内部连接成功后记录本地源端口。客户端无需增加新的交互步骤。

## 验证

gtest 验证首帧未收齐时无内部连接、完整首帧只转发一次、延迟首帧不消耗模拟内部握手预算、首帧 payload/长度头未收齐的独立超时日志，以及空帧不触发内部连接；保留大块回传和网关基础测试。

真实 TLS 回环使用生产 TCP 转发函数，覆盖 TLS 1.2/1.3、有无客户端证书、拆帧、512 KiB 回传。额外延迟首帧 6 秒，确认内部 5 秒握手仍成功；首帧超时后再次连接，确认可成功握手。RTSP、原始 TLS 直通、UDP 和客户端代码不在本次修改范围。

## 已完成的验证

- 旧实现的两项新增回归分别复现首帧等待消耗内部握手预算、空首帧触发内部连接。
- MSYS2 UCRT64 构建成功；10 项 Gateway gtest 连续两轮全部通过。
- 首帧 payload 和长度头未收齐时约 10 秒关闭，未建立内部连接，日志分别保留实际接收量和 unknown 声明长度。
- TLS 1.2/1.3、有无客户端证书、拆帧和合并发送、512 KiB TLS 回传及 1 MiB 原始回传通过。
- 首帧延迟 6 秒后 TLS 1.3 握手成功；首帧接收超时后再次连接也成功。
- 测试夹具先由系统分配公网 TCP 端口，再检查内部和 UDP 端口，避免从内部端口推导公网端口时遇到重复运行的端口占用。

超时诊断包含以下字段，数值会随实际接收进度变化：

```text
event=first_frame_timeout timeout_ms=10000 elapsed_ms=10005 first_header_bytes=2 first_declared=4096 first_received=1521
event=closed reason=first_frame_timeout first_header_bytes=2 first_declared=4096 first_received=1521 c2i_bytes=0 i2c_bytes=0 first_frame_elapsed_ms=10005 internal_elapsed_ms=0
```
