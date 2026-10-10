# 串流断开通知

Sunshine 向上游发送 `event: "disconnected"`。`data.type` 表示分类，`data.message` 携带当前能够确定的具体原因；顶层 `message` 保留相同内容，以兼容现有消费者。

```json
{
  "event": "disconnected",
  "message": "Could not encode video frame 123 with 'h264_nvenc'",
  "data": {
    "type": 41,
    "message": "Could not encode video frame 123 with 'h264_nvenc'"
  }
}
```

## 类型约定

| 值 | 名称 | 含义 / 当前触发路径 |
| ---: | --- | --- |
| 0 | `other` | 原因未知的兜底类型 |
| 1 | `force_timeout` | 挂机超时提醒，等待上游决定是否断开 |
| 2 | `standby_timeout` | 无操作超时提醒，等待上游决定是否断开 |
| 10 | `client_requested_stop` | 客户端通过经过认证的 `/cancel` 请求结束串流 |
| 11 | `client_reported_error` | 保留：需要客户端明确报告异常；当前客户端协议没有对应报告 |
| 12 | `peer_disconnected_unknown` | ENet 对端断开；无法确认用户意图或异常根因 |
| 20 | `control_timeout` | 控制通道建立后超时；不据此认定客户端异常或网络故障 |
| 21 | `stream_start_timeout` | 控制通道尚未建立，或未收到初始音频、视频、麦克风 PING |
| 30 | `upstream_requested_stop` | 上游平台下发 `disconnected` 指令结束串流 |
| 31 | `server_requested_stop` | 服务端管理操作主动停止，例如撤销客户端授权 |
| 32 | `server_shutdown` | Sunshine 正常关闭或重启 |
| 40 | `server_capture_error` | 服务端画面采集、采集显示器初始化等失败，导致串流结束 |
| 41 | `server_encoder_error` | 服务端编码设备、编码器初始化、图像转换或视频编码失败，导致串流结束 |
| 42 | `server_internal_error` | 服务端其他内部异常、工作线程意外退出、共享广播意外停止等 |
| 43 | `gateway_error` | 保留：必须确认是网关内部故障且导致会话结束；瞬时转发错误不使用该值 |
| 44 | `server_process_abnormal_exit` | 保留：崩溃、卡死后被终止等进程异常退出，需要外部监控补报 |
| 50 | `control_protocol_error` | 控制消息解密认证失败或协议校验失败，服务端主动关闭会话 |

## 归因与去重

- 会话与采集、编码上下文共享首次原因记录。错误发生时先保存类型和具体原因，再通知其他线程退出。
- 同一会话实际停止只生成一次断开通知。后续心跳超时、线程清理、服务关闭不能覆盖最初原因。
- `1/2` 是原有业务提醒，并不表示串流已经结束，因此可以先收到 `1/2`，随后收到真正停止的 `30` 等类型。
- 普通对端断开不等于用户主动退出；超时也无法证明是哪一端异常。保持 `12/20/21`，直到存在更明确的证据。
- `50` 表示服务端因协议问题关闭会话，不能直接等同于服务端程序缺陷。
- 显示器切换和编码器重新初始化过程、可恢复的音频故障、单次丢包或发送错误不单独当作致命断开。

本地 `[stream][disconnect]` 日志包含 `session`、`type`、`message`。通知沿用现有发送队列；中台未连接时仍会丢弃通知。本次没有新增客户端异常报告协议、网关故障监控或进程监控程序，因此 `11/43/44` 只定义接口值。
