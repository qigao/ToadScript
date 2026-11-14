# Toad.Native

嵌入式 C/C++ SDK 的包元数据使用 `Toad.Native`，包含 MIR 解释器、JIT、
公开头文件及当前构建 graph 中的原生插件，不附带 CLI。
打包目录为 `sdk/linux-x64`、`sdk/macos-arm64`、`sdk/android-arm64-v8a`。
本次更名未发布包；实际仓库仍为 [qigao/TurboScript](https://github.com/qigao/TurboScript)。

## 依赖与使用

依赖来自精确选择的已发布 SaltsBase、CMeta、SaltsUtils、CNet、CHttp 和
TurboDB SDK；选择入口及当前阻塞见根 [README](../../README.md#upstream-dependency-versions)。
恢复时保留各 SDK 的独立安装根，按现有环境输入设置 `SALTS_BASE_ROOT`、
`CMETA_ROOT`、`SALTS_UTILS_ROOT`、`SALTSCORE_ROOT`、`CHTTP_ROOT` 和
`TURBODB_ROOT`，不混入旧聚合 Salts SDK。
第三方依赖使用同一轮 vcpkg-cache、编译器、triplet 和项目 manifest。

将解包后的对应 SDK 目录加入 CMake 包搜索路径，使用
`find_package(Toad CONFIG REQUIRED)` 并链接 `Toad::Script`。
宿主入口为 `<toad/script.h>` 和 `toad_script_*`；独立引擎入口为
`<toad/engine.h>` 和 `Toad::Engine`。更名前的宿主及原生插件需要更新
include、符号和 CMake 引用后一起重编译。`.tbs` 文件保留，CMeta 和插件
协议身份统一到 `toad.*`，旧身份不提供兼容别名。
插件使用者还需部署所需的匹配依赖动态库；Android 应用需部署 `libc++_shared.so`。

## 打包与验证

正式版本由 `CMakeLists.txt`、根 `vcpkg.json`、公开头文件、Git tag 和
`Toad.Native.csproj` 共同约束，当前源码版本为 `3.0.10`。
发布 workflow 要求对应 `v3.0.10` tag；普通 CI 的
`3.0.10-ci.<run>.<attempt>` 包只作为 artifact。每个 SDK 的
`toad-sdk-manifest.txt` 记录实际源码提交、依赖版本和平台。

验证使用本项目现有 presets、完整 build graph、正式 CTest 和 install/package
流程，不建立独立 consumer 或 smoke 工程。Toad 不使用或链接 QuickJS/Lua
适配器。当前本机配置被 SaltsUtils 对 `qjsConfig.cmake` 的无条件查找阻塞，
需要修复上游包的依赖边界，不能通过向本仓库添加 QuickJS 解决。
已选择的 CHttp 发布包还要求旧聚合 Salts SDK。
编译、CTest、安装及打包尚未通过。Android 交叉编译和 ELF 验收也不能代替设备运行。
