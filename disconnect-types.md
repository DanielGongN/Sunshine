# 串流断开连接通知类型

`data.type` 用于区分断开连接的类型；有具体原因时，`data.message` 可以携带说明内容。

| `data.type` | 类型名称 | 含义及判定条件 |
| --- | --- | --- |
| **0** | `unknown` | 原因未知，证据不足时兜底 |
| **1** | `force_timeout` | 保留现有挂机超时通知 |
| **2** | `standby_timeout` | 保留现有无操作超时通知 |
| **3** | `game_process_exit` | 游戏进程退出，断开连接 (agent服务触发)|
| **10** | `client_requested_stop` | 客户端明确请求退出串流 |
| **11** | `client_reported_error` | 客户端明确报告解码失败、渲染失败等异常并结束串流 |
| **12** | `peer_disconnected_unknown` | 收到对端断开事件，但无法确定是主动退出还是异常后的清理 |
| **20** | `control_timeout` | 控制连接或心跳超时，具体根因未确定 |
| **21** | `stream_start_timeout` | 建立串流阶段超时，例如控制通道未建立或未收到视频初始 PING；音频、麦克风 PING 缺失不触发断开 |
| **30** | `upstream_requested_stop` | 上游平台下发断开指令 |
| **31** | `server_requested_stop` | 服务端管理操作主动结束串流，例如管理员停止、撤销客户端授权 |
| **32** | `server_shutdown` | Sunshine 正常退出或重启导致串流结束 |
| **40** | `server_capture_error` | 服务端画面采集失败，并因此结束串流 |
| **41** | `server_encoder_error` | 服务端编码器初始化、转换或编码失败，并因此结束串流 |
| **42** | `server_internal_error` | 服务端其他内部错误或工作线程异常退出导致串流结束 |
| **43** | `gateway_error` | 已确认网关内部转发故障导致串流结束 |
| **44** | `server_process_abnormal_exit` | 服务端进程崩溃、卡死后被终止等异常退出，需要外部监控补报 |
| **50** | `control_protocol_error` | 控制消息解密、认证或协议校验失败，服务端因此关闭会话 |

普通音频和已协商的麦克风为可选通道。没有初始 PING 时持续等待，不影响视频串流；稍后收到匹配 PING 后再启动对应处理。会话结束时取消等待。
