On Windows we use msys2 and ucrt64 to compile.
You need to prefix commands with `E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.

For every subsequent change, the final build verification must run `bash build.sh` in MSYS2 UCRT64:
`E:\WorkComp\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "bash build.sh"`.
Direct CMake builds or individual tests may supplement this final build, but must not replace it.

Prefix build directories with `cmake-build-`.

The test executable is named `test_sunshine` and will be located inside the `tests` directory within
the build directory.

The project uses gtest as a test framework.

Always follow the style guidelines defined in .clang-format for c/c++ code.

When adding localization do not update any language other than `en`. This also means to exclude en-US or other variants.

## Git 提交规范

每次完成改动并通过相应验证后，主动创建本地 `git commit`，无需再次询问。

- 问题修复使用 `fix: 中文修复说明`。
- 新增功能使用 `feature: 中文功能说明`。
- 同一次提交涉及多个改动点时，在提交正文中使用 `1. `、`2. `、`3. ` 依次编号，并用中文说明每个改动点。
- 只暂存并提交当前任务的改动，保留工作区中其他未提交的改动。
