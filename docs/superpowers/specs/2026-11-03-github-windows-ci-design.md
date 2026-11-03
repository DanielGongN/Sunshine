# GitHub Windows 构建设计

目标：origin 上的 master 推送能够在 GitHub Actions 编译 Windows x64，并上传便携 ZIP。

- 将 Simple-Web-Server 固定到公开的上游提交 546895a93a29062bb178367b46c7afb72da9881e。
- 在主项目添加该依赖时设置 CMake 兼容策略，保留原本仅本机存在的子模块提交的兼容效果。
- Boost 系统包允许满足 1.91.0 最低要求的兼容新版本，避免 MSYS2 滚动更新后触发失效的源码下载。
- fork 的 CI 调用独立 Windows 工作流，使用 MSYS2 UCRT64、Node.js 和 .NET；关闭文档及硬件相关测试，限制编译并行数以适应托管机器内存。
- 上游 LizardByte/Sunshine 保留完整发布流水线；fork 不执行依赖上游签名证书和发布设置的任务。
- 网页发布和翻译同步仅在上游仓库执行，避免 fork 缺少上游发布密钥导致无关失败。
- 独立工作流支持手动运行，v 开头的标签保留既有 GitHub Release 上传行为。
- 构建目录使用 cmake-build- 前缀，保留用户其他未提交的更改。
- Windows x64 包将根目录的 interception.dll 安装到主程序同目录，将根目录的 engine.conf 安装到 config/engine.conf。
- GitHub 上传产物前检查两个运行文件的路径、非空内容和 SHA-256，确保与仓库源文件一致。

验收：MSYS2 UCRT64 的 bash build.sh 成功；提交推送后，GitHub CI 成功并存在非空的 Sunshine-Windows-AMD64 构建产物。
