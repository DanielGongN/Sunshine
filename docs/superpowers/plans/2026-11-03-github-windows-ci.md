# GitHub Windows CI Implementation Plan

**Goal:** 修复 origin 上的 Windows 编译并验证 GitHub Actions 产物。

**Architecture:** 主项目管理 Simple-Web-Server 的 CMake 策略，子模块使用公开上游提交。fork 的 CI 调用独立 Windows 工作流，复用现有 CMake 和 CPack 配置。

**Tech Stack:** GitHub Actions、MSYS2 UCRT64、CMake、Ninja、CPack。

**Spec:** docs/superpowers/specs/2026-11-03-github-windows-ci-design.md

## Global Constraints

- 最终本地验证运行 MSYS2 UCRT64 的 bash build.sh。
- 构建目录使用 cmake-build- 前缀。
- 仅提交本任务文件，不提交其他工作区修改，不修改翻译。

## Steps

- [ ] 将 Simple-Web-Server 检出至公开上游提交，在 cmake/dependencies/common.cmake 的局部函数内设置 CMake 策略。
- [ ] 在 .github/workflows/ci.yml 增加 fork Windows 任务，并将上游发布入口限制于上游仓库。
- [ ] 将网页发布和翻译同步限制于上游仓库，避免 fork 缺少发布密钥导致无关失败。
- [ ] 完善 .github/workflows/build-windows.yml：可复用入口、UCRT64 依赖、限制并行度、便携 ZIP 和产物上传。
- [ ] 检查工作流语法及可下载依赖，运行规定的本地完整构建。
- [ ] 仅暂存任务文件，使用中文 fix 提交并推送 origin/master。
- [ ] 跟进对应提交的 GitHub CI，修复实际日志中出现的问题，直到成功且有非空构建产物。
