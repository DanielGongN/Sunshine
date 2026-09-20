# Sunshine `engine.conf` 配置参数说明

本文档根据当前分支的 `src/config.cpp::apply_config()`、`src/config.h` 和
`docs/configuration.md` 整理。`conf.md` 只是 Markdown 说明文件，Sunshine **不会直接读取它**；
需要把文中的 INI 配置项复制到实际配置文件中。

## 配置文件位置与语法

当前 Windows 分支默认读取：

```text
<sunshine.exe 所在目录>\config\engine.conf
```

也可以把配置文件完整路径作为 Sunshine 的第一个命令行参数。配置语法为每行一个
`名称 = 值`，以 `#` 开头的行为注释。布尔值支持
`enabled/disabled`、`true/false`、`yes/no` 和 `1/0`。

> 表格中的“默认值”以当前分支代码为准。上游文档仍把 `port` 写成 `47989`、把
> `system_tray` 写成 `enabled`，但当前代码默认值分别是 `41200` 和 `disabled`。

## 当前场景建议配置

以下配置适用于 Windows、NVIDIA NVENC、公网连接、视频 UDP 使用独立端口的场景。
它不会写死网卡地址；多网卡机器应将示例 `bind_address` 改为 Sunshine 主机真实网卡的
固定 IPv4 地址。

```ini
# 基础端口。当前分支的视频 UDP 端口为 port + 14，即 41214。
port = 41200
address_family = ipv4

# 多网卡时建议启用，并替换成 Sunshine 主机的真实网卡地址。
# bind_address = 192.168.1.10

# 编码和帧率
encoder = nvenc
minimum_fps_target = 60
max_bitrate = 0

# 网络。packetsize=0 表示由 Sunshine 自动决定。
ping_timeout = 10000
packetsize = 0
fec_percentage = 20
lan_encryption_mode = 0
wan_encryption_mode = 1

# NVENC 低延迟默认值
nvenc_preset = 1
nvenc_twopass = quarter_res
nvenc_spatial_aq = disabled
nvenc_vbv_increase = 0
nvenc_split_encode = driver_decides
nvenc_realtime_hags = enabled
nvenc_latency_over_power = enabled
nvenc_opengl_vulkan_on_dxgi = enabled
nvenc_h264_cavlc = disabled
```

`minimum_fps_target = 60` 会让静止或低帧率画面通过重复帧保持最低 60 FPS。它不能把
采集或编码本身只有 20～30 FPS 的内容变成真实 60 FPS，而且会增加带宽和客户端解码负载。

## 端口关系

所有端口都由基础参数 `port` 加固定偏移计算。以 `port = 41200` 为例：

| 用途 | 协议 | 偏移 | 实际端口 |
|---|---:|---:|---:|
| 单端口 Gateway / HTTPS 入口 | TCP、UDP | `+0` | `41200` |
| Web 配置界面 | TCP | `+1` | `41201` |
| 兼容 HTTP 服务 | TCP | `+5` | `41205` |
| 视频流 | UDP | `+14` | `41214` |
| 控制流 | UDP | `+15` | `41215` |
| 音频流 | UDP | `+16` | `41216` |
| 客户端麦克风 | UDP | `+17` | `41217` |
| RTSP 会话建立 | TCP | `+26` | `41226` |
| 内部 HTTPS | TCP | `+100` | `41300` |

公网部署时需要在防火墙和 NAT 中放行实际使用的端口。视频独立直连时至少应把公网
UDP 视频端口映射到主机的 `port + 14`。独立端口上的原生 Sunshine/Moonlight 视频包
不应再添加 Gateway 的 3 字节外层头；只有发到单端口 Gateway 的 UDP 包才使用
`[stream_id:1][length:2（大端）]` 包装。

## 通用与界面

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `locale` | `en` | 语言代码 | Sunshine 界面语言。当前代码还接受 `bg/cs/de/en/en_GB/en_US/es/fr/hu/it/ja/ko/pl/pt/pt_BR/ru/sv/tr/uk/vi/zh/zh_TW`。 |
| `sunshine_name` | 主机名 | 字符串 | Moonlight 中显示的主机名称。 |
| `min_log_level` | `info` | `verbose/debug/info/warning/error/fatal/none` 或 `0～6` | 最低日志级别。`verbose/debug` 可能影响串流性能。 |
| `notify_pre_releases` | `disabled` | 布尔值 | 是否提示 Sunshine 预发布版本。 |
| `system_tray` | `disabled` | 布尔值 | 是否显示系统托盘图标和桌面通知。 |
| `global_prep_cmd` | `[]` | JSON 数组 | 所有应用启动前执行命令，退出后执行撤销命令；命令失败会中止应用启动。 |
| `flags` | 空 | 标志字符 | 内部/命令行兼容标志，见“内部标志”；一般不要配置。 |

`global_prep_cmd` 示例：

```ini
global_prep_cmd = [{"do":"开始前命令","undo":"结束后命令","elevated":false}]
```

## 网络与串流

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `port` | `41200` | `1024～65509` | Sunshine 基础端口；其它服务端口随之偏移。 |
| `address_family` | `ipv4` | `ipv4`、`both` | 监听 IPv4，或同时监听 IPv4/IPv6。 |
| `bind_address` | 空 | 本机 IP | 限制 Sunshine 绑定到指定真实网卡。留空表示所有接口；地址必须存在且与地址族匹配。 |
| `upnp` | `disabled` | 布尔值 | 尝试通过路由器 UPnP 自动映射公网端口。手工映射端口时通常关闭。 |
| `external_ip` | 自动检测 | 公网 IP/主机地址 | 覆盖 Sunshine 自动探测到的公网地址；不包含端口。 |
| `ping_timeout` | `10000` | 毫秒，`>= 0` | 多久收不到客户端数据后关闭串流。公网抖动严重时可适当增大。 |
| `packetsize` | `0` | 字节，`0` 或 `200～65535` | 限制 UDP 包大小以避免低 MTU 链路分片；`0` 表示自动。常用手工值为 `1200` 左右。 |
| `fec_percentage` | `20` | `1～255` | 每帧视频追加的前向纠错包比例。提高可抗丢包，但会增加带宽。 |
| `lan_encryption_mode` | `0` | `0/1/2` | 局域网视频加密策略：`0` 从不，`1` 客户端支持时启用，`2` 强制。 |
| `wan_encryption_mode` | `1` | `0/1/2` | 公网视频加密策略：`0` 从不，`1` 尽可能启用，`2` 强制。 |

## 视频通用配置

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `capture` | 自动 | 平台捕获器名称 | 强制使用指定屏幕捕获后端；留空由 Sunshine 自动选择。 |
| `encoder` | 自动 | `nvenc/qsv/amdvce/videotoolbox/vaapi/software` 等 | 强制编码器；名称是否可用取决于平台和构建选项。 |
| `adapter_name` | 自动 | 适配器名称/ID | 多 GPU 时指定采集或编码使用的显卡。Windows 可用 `tools\dxgi-info.exe` 查询。 |
| `output_name` | 自动 | 显示设备 ID | 指定被捕获的显示器。Windows 使用启动日志中的 `device_id`。 |
| `qp` | `28` | 整数 | 不支持恒定码率时使用的量化参数；数值越高，压缩越强、画质越低。 |
| `min_threads` | `2` | 整数 | 软件编码使用的最少线程/切片数。 |
| `hevc_mode` | `0` | `0～3` | 是否允许客户端请求 HEVC Main/Main10。通常让客户端能力协商决定。 |
| `av1_mode` | `0` | `0～3` | 是否允许客户端请求 AV1 8/10 bit。通常让客户端能力协商决定。 |
| `max_bitrate` | `0` | Kbit/s | 编码码率上限；`0` 表示使用客户端请求值，不额外限速。 |
| `minimum_fps_target` | `0` | FPS，`0～1000` | 静止/低帧率场景的最低有效发送帧率。`0` 表示客户端请求帧率的一半。 |

## Windows 显示设备配置

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `dd_configuration_option` | `disabled` | `disabled/verify_only/ensure_active/ensure_primary/ensure_only_display` | 串流前验证、启用或切换目标显示设备。 |
| `dd_resolution_option` | `auto` | `disabled/auto/manual` | 是否自动使用客户端分辨率，或使用手工分辨率。 |
| `dd_manual_resolution` | 空 | 如 `1920x1080` | `dd_resolution_option=manual` 时使用的分辨率。 |
| `dd_refresh_rate_option` | `auto` | `disabled/auto/manual` | 是否自动使用客户端帧率，或使用手工刷新率。 |
| `dd_manual_refresh_rate` | 空 | Hz，如 `60` | `dd_refresh_rate_option=manual` 时使用的刷新率。 |
| `dd_hdr_option` | `auto` | `disabled/auto` | 是否根据客户端请求自动切换 HDR。 |
| `dd_config_revert_delay` | `3000` | 毫秒，`>= 0` | 应用关闭或最后会话结束后，延迟恢复显示配置。 |
| `dd_config_revert_on_disconnect` | `disabled` | 布尔值 | 所有客户端断开时立即走恢复显示配置逻辑。 |
| `dd_wa_hdr_toggle_delay` | `0` | 毫秒，`0～3000` | 虚拟显示器 HDR 颜色异常的开关延时规避；`0` 关闭，常用尝试值约 `500`。 |
| `dd_mode_remapping` | 三组空数组 | JSON 对象 | 按客户端请求的分辨率/FPS 重映射为实际显示模式。 |

`dd_mode_remapping` 示例：

```ini
dd_mode_remapping = {
  "mixed": [
    {
      "requested_resolution": "1920x1080",
      "requested_fps": "60",
      "final_resolution": "2560x1440",
      "final_refresh_rate": "60"
    }
  ],
  "resolution_only": [],
  "refresh_rate_only": []
}
```

## 音频与客户端麦克风

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `audio_sink` | 自动 | 音频设备名称/ID | 指定用于回环采集的音频输出设备。Windows 可用 `tools\audio-info.exe` 查询。 |
| `virtual_sink` | 空 | 音频设备名称/ID | 指定虚拟音频设备，例如 Steam Streaming Speakers。 |
| `stream_audio` | `enabled` | 布尔值 | 是否向客户端发送主机音频。 |
| `install_steam_audio_drivers` | `enabled` | 布尔值 | 在 Windows 上安装 Steam Streaming Speakers 驱动以支持环绕声/静音主机。 |
| `client_mic` | `disabled` | 布尔值 | 是否接收兼容客户端上传的 48 kHz Opus 麦克风音频。 |
| `client_mic_sink` | 空 | Windows 播放端点 | 客户端麦克风解码后写入的播放端点，例如 `CABLE Input`。 |

## 键盘、鼠标与手柄

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `keyboard` | `enabled` | 布尔值 | 接受客户端键盘输入。 |
| `mouse` | `enabled` | 布尔值 | 接受客户端鼠标输入。 |
| `controller` | `enabled` | 布尔值 | 接受客户端手柄输入。 |
| `gamepad` | 平台首个支持项/自动 | 支持的手柄名称 | 指定主机端模拟的手柄类型。 |
| `ds4_back_as_touchpad_click` | `enabled` | 布尔值 | 将 Select/Back 同时模拟为 DS4 触摸板点击。 |
| `motion_as_ds4` | `enabled` | 布尔值 | 客户端有运动传感器时优先模拟为 DS4。 |
| `touchpad_as_ds4` | `enabled` | 布尔值 | 客户端有触摸板时优先模拟为 DS4。 |
| `ds5_inputtino_randomize_mac` | `enabled` | 布尔值 | 随机化生成的虚拟 DS5 控制器 MAC 地址。 |
| `back_button_timeout` | `-1` | 毫秒 | Back/Select 按住多久后模拟 Home/Guide；`-1` 关闭。 |
| `key_repeat_delay` | `500` | 毫秒，`>= 0` | 键盘开始连发前的初始等待时间。 |
| `key_repeat_frequency` | `24.9` | 次/秒，`> 0` | 键盘连发频率。 |
| `always_send_scancodes` | `enabled` | 布尔值 | 总是发送扫描码以改善游戏兼容性；非美式键盘映射异常时可关闭。 |
| `high_resolution_scrolling` | `enabled` | 布尔值 | 传递高分辨率滚轮事件；旧应用滚动过快时可关闭。 |
| `native_pen_touch` | `enabled` | 布尔值 | 传递原生触控笔/触摸事件。 |
| `keybindings` | Shift/Ctrl/Alt 左键映射 | 整数数组 | 按“来源键码, 目标键码”成对重映射按键。 |
| `key_rightalt_to_key_win` | `disabled` | 布尔值 | 仅供 Web UI 使用：把 Right Alt 映射为 Windows 键；手改文件建议用 `keybindings`。 |

按键映射示例：

```ini
# 默认：Shift -> Left Shift，Ctrl -> Left Ctrl，Alt -> Left Alt
keybindings = [0x10, 0xA0, 0x11, 0xA2, 0x12, 0xA4]

# 追加 Right Alt -> Left Windows 时可使用：
# keybindings = [0x10, 0xA0, 0x11, 0xA2, 0x12, 0xA4, 0xA5, 0x5B]
```

## Middleware 与自动断开

这些是当前分支新增的上层 Middleware 配置，并非视频 UDP 端口或 Gateway 包装开关。

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `middleware_enabled` | `disabled` | 布尔值 | 是否启用上游 Middleware WebSocket 客户端。 |
| `middleware_address` | `0.0.0.0` | IP/主机名 | Middleware 服务地址。`0.0.0.0` 通常只适合作为监听地址；作为连接目标时应配置实际地址。 |
| `middleware_port` | `40002` | TCP 端口 | Middleware WebSocket 服务端口。 |
| `middleware_gamepad_preinit` | `disabled` | 布尔值 | 是否在正式串流输入到达前预初始化虚拟手柄。 |
| `force_disconnected_timeout` | `900` | 秒 | 按键/控制保持不变达到阈值时强制结束会话。 |
| `standby_disconnected_timeout` | `900` | 秒 | 没有按住控制且持续无输入达到阈值时结束待机会话。 |

示例：

```ini
middleware_enabled = enabled
middleware_address = 127.0.0.1
middleware_port = 40002
middleware_gamepad_preinit = disabled
force_disconnected_timeout = 900
standby_disconnected_timeout = 900
```

## Web UI、状态、证书与文件

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `origin_web_ui_allowed` | `lan` | `pc/lan/wan` | 允许访问 HTTPS Web UI 的来源范围：本机、局域网或公网。 |
| `csrf_allowed_origins` | 内置本机地址 | 逗号分隔 HTTPS Origin | 追加允许跨站状态修改请求的来源，只接受以 `https://` 开头的条目。公网开放需谨慎。 |
| `file_apps` | `config\apps.json` | 文件路径 | Moonlight 可启动应用的 JSON 配置文件。 |
| `file_state` | `sunshine_state.json` | 文件路径 | Sunshine 配对等运行状态文件。 |
| `credentials_file` | 跟随 `file_state` | 文件路径 | Web UI 用户凭据文件；显式配置时覆盖默认关联路径。 |
| `log_path` | `config\engine.log` | 文件路径 | 日志基础路径；实际日志文件名会插入启动时间戳。 |
| `pkey` | 平台默认私钥路径 | PEM 文件路径 | Web UI 和 Moonlight 配对使用的私钥，推荐 RSA-2048。 |
| `cert` | 平台默认证书路径 | PEM 文件路径 | Web UI 和 Moonlight 配对使用的证书，推荐 RSA-2048 公钥。 |

```ini
# 只有通过指定 HTTPS Origin 访问 Web UI 时才需要追加。
# csrf_allowed_origins = https://admin.example.com,https://ops.example.com

# 不建议无必要开放到公网。
origin_web_ui_allowed = lan
```

## NVIDIA NVENC

仅在使用 NVIDIA NVENC 时生效。

| 参数 | 当前默认值 | 可选值/单位 | 用途 |
|---|---|---|---|
| `nvenc_preset` | `1` | `1～7` | 质量/延迟预设。越高压缩效率通常越好，但编码延迟和 GPU 开销越大。 |
| `nvenc_twopass` | `quarter_res` | `disabled/quarter_res/full_res` | 两遍分析模式。四分之一分辨率通常兼顾低延迟和码率控制。 |
| `nvenc_spatial_aq` | `disabled` | 布尔值 | 空间自适应量化，低码率时可改善主观画质，但会使用 CUDA 资源。 |
| `nvenc_vbv_increase` | `0` | 百分比，`0～400` | 放宽单帧 VBV/HRD 上限。可改善复杂场景质量，但会产生突发码率并加重丢包。 |
| `nvenc_split_encode` | `driver_decides` | `disabled/driver_decides/forced` | 多 NVENC 单元 GPU 上进行分片编码，以降低编码延迟。 |
| `nvenc_realtime_hags` | `enabled` | 布尔值 | Windows HAGS 下使用实时 GPU 调度优先级；高显存占用时若驱动冻结可关闭。 |
| `nvenc_latency_over_power` | `enabled` | 布尔值 | 请求高性能电源状态，优先降低编码延迟。 |
| `nvenc_opengl_vulkan_on_dxgi` | `enabled` | 布尔值 | Windows 上让全屏 OpenGL/Vulkan 通过 DXGI 呈现以改善采集帧率；属于系统级临时设置。 |
| `nvenc_h264_cavlc` | `disabled` | 布尔值 | H.264 使用旧式 CAVLC 而非 CABAC；同画质通常多耗约 10% 码率。 |

排查公网画面掉帧时应优先保持 `nvenc_vbv_increase = 0`。提高该值会制造更大的单帧
UDP 突发，经过 NAT、网关或队列较小的链路时更容易集中丢包。

## Intel Quick Sync Video（QSV）

| 参数 | 当前默认值 | 可选值 | 用途 |
|---|---|---|---|
| `qsv_preset` | `medium` | `veryfast/faster/fast/medium/slow/slower/veryslow` | 编码速度与压缩效率预设。 |
| `qsv_coder` | `auto` | `auto/cabac/cavlc` | H.264 熵编码方式。 |
| `qsv_slow_hevc` | `disabled` | 布尔值 | 允许较老 Intel GPU 使用低功耗 HEVC 编码路径。 |

## AMD AMF

| 参数 | 当前默认值 | 常用可选值 | 用途 |
|---|---|---|---|
| `amd_usage` | `ultralowlatency` | `transcoding/ultralowlatency/lowlatency/webcam/highquality/lowlatency_highquality`，依编解码器支持 | AMF 使用场景预设。 |
| `amd_rc` | `vbr_latency` | `cqp/cbr/vbr_peak/vbr_latency/qvbr/hqvbr/hqcbr`，依编解码器支持 | 码率控制模式。 |
| `amd_enforce_hrd` | `disabled` | 布尔值 | 强制 HRD 约束，帮助限制目标码率。 |
| `amd_quality` | `balanced` | `speed/balanced/quality` | 编码速度和画质预设。 |
| `amd_preanalysis` | `disabled` | 布尔值 | 启用预分析提高画质，但增加延迟。 |
| `amd_vbaq` | `enabled` | 布尔值 | 启用基于方差的自适应量化。 |
| `amd_coder` | `auto` | `auto/cabac/cavlc` | H.264 熵编码方式。 |

## 软件编码

| 参数 | 当前默认值 | 可选值 | 用途 |
|---|---|---|---|
| `sw_preset` | `superfast` | 编码器支持的 preset | 软件编码速度/压缩效率预设。更慢的 preset 通常延迟更高。 |
| `sw_tune` | `zerolatency` | 编码器支持的 tune | 软件编码调优；串流通常使用 `zerolatency`。 |

当实际软件编码器为 SVT-AV1 时，`sw_preset` 还会被转换为对应的 SVT-AV1 preset。

## macOS VideoToolbox

| 参数 | 当前默认值 | 可选值 | 用途 |
|---|---|---|---|
| `vt_coder` | `auto` | `auto/cabac/cavlc` | H.264 熵编码方式。 |
| `vt_software` | `auto` | `auto/allowed/forced` | 自动、允许或强制使用 VideoToolbox 软件编码。 |
| `vt_realtime` | `enabled` | 布尔值 | 使用实时编码模式。 |

## Linux VAAPI 与 Vulkan Video

| 参数 | 当前默认值 | 可选值/含义 | 用途 |
|---|---|---|---|
| `vaapi_strict_rc_buffer` | `disabled` | 布尔值 | 严格约束码率缓冲，可减少场景切换时网络丢帧，但运动画质可能下降。 |
| `vk_tune` | `2` | `0` 默认、`1` 高质量、`2` 低延迟、`3` 超低延迟、`4` 无损 | Vulkan Video 编码调优模式。 |
| `vk_rc_mode` | `2` | `0` 驱动决定、`1` CQP、`2` CBR、`4` VBR | Vulkan Video 码率控制模式。 |

## 内部标志

`flags` 会按字符启用内部行为，通常仅用于调试或兼容，不建议写入常规配置。当前代码中的
标志包括从标准输入读取 PIN、不读写状态、强制替换视频头、UPnP 和固定 PIN。
正常配置 UPnP 请使用明确的 `upnp = enabled`，不要依赖 `flags`。

## 完整参数名核对清单

下面列出当前 `apply_config()` 实际识别的参数，便于确认模板没有遗漏：

```text
adapter_name, address_family, always_send_scancodes, amd_coder,
amd_enforce_hrd, amd_preanalysis, amd_quality, amd_rc, amd_usage, amd_vbaq,
audio_sink, av1_mode, back_button_timeout, bind_address, capture, cert,
client_mic, client_mic_sink, controller, credentials_file, csrf_allowed_origins,
dd_config_revert_delay, dd_config_revert_on_disconnect, dd_configuration_option,
dd_hdr_option, dd_manual_refresh_rate, dd_manual_resolution, dd_mode_remapping,
dd_refresh_rate_option, dd_resolution_option, dd_wa_hdr_toggle_delay,
ds4_back_as_touchpad_click, ds5_inputtino_randomize_mac, encoder, external_ip,
fec_percentage, file_apps, file_state, flags, force_disconnected_timeout, gamepad,
global_prep_cmd, hevc_mode, high_resolution_scrolling,
install_steam_audio_drivers, key_repeat_delay, key_repeat_frequency,
key_rightalt_to_key_win, keybindings, keyboard, lan_encryption_mode, locale,
log_path, max_bitrate, middleware_address, middleware_enabled,
middleware_gamepad_preinit, middleware_port, min_log_level, min_threads,
minimum_fps_target, motion_as_ds4, mouse, native_pen_touch, notify_pre_releases,
nvenc_h264_cavlc, nvenc_latency_over_power, nvenc_opengl_vulkan_on_dxgi,
nvenc_preset, nvenc_realtime_hags, nvenc_spatial_aq, nvenc_split_encode,
nvenc_twopass, nvenc_vbv_increase, origin_web_ui_allowed, output_name,
packetsize, ping_timeout, pkey, port, qp, qsv_coder, qsv_preset, qsv_slow_hevc,
standby_disconnected_timeout, stream_audio, sunshine_name, sw_preset, sw_tune,
system_tray, touchpad_as_ds4, upnp, vaapi_strict_rc_buffer, virtual_sink,
vk_rc_mode, vk_tune, vt_coder, vt_realtime, vt_software, wan_encryption_mode
```

