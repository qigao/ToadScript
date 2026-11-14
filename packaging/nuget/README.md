# Toad.Native

嵌入式 C/C++ SDK 的包元数据使用 `Toad.Native`，包含 MIR 解释器、JIT、
公开头文件及当前构建 graph 中的原生插件，不附带 CLI。
打包目录为 `sdk/windows-x64`、`sdk/linux-x64`、`sdk/macos-arm64`、`sdk/android-arm64-v8a`。
版本从 `0.1.0` 开始；实际仓库为 [qigao/TurboScript](https://github.com/qigao/TurboScript)。

## 依赖与使用

依赖来自精确选择的已发布 SaltsBase、CMeta、SaltsUtils、CNet 和 CHttp SDK；选择入口见根 [README](../../README.md#upstream-dependency-versions)。
恢复时保留各 SDK 的独立安装根，按现有环境输入设置 `SALTS_BASE_ROOT`、
`CMETA_ROOT`、`SALTS_UTILS_ROOT`、`SALTSCORE_ROOT` 和 `CHTTP_ROOT`，不混入旧聚合 Salts SDK。
第三方依赖使用同一轮 vcpkg-cache、编译器、triplet 和项目 manifest。
数据库模块暂不组装，SDK 恢复与 CMake 配置不要求 TurboDB。

将解包后的对应 SDK 目录加入 CMake 包搜索路径，使用
`find_package(Toad CONFIG REQUIRED)` 并链接 `Toad::Script`。
宿主入口为 `<toad/script.h>` 和 `toad_script_*`；独立引擎入口为
`<toad/engine.h>` 和 `Toad::Engine`。更名前的宿主及原生插件需要更新
include、符号和 CMake 引用后一起重编译。`.tbs` 文件保留，CMeta 和插件
协议身份统一到 `toad.*`，旧身份不提供兼容别名。
插件使用者还需部署所需的匹配依赖动态库；Android 应用需部署 `libc++_shared.so`。

## 打包与验证

正式版本由 `CMakeLists.txt`、根 `vcpkg.json`、公开头文件、Git tag 和
`Toad.Native.csproj` 共同约束，当前源码版本为 `0.1.0`。
发布 workflow 要求对应 `v0.1.0` tag；普通 CI 的
`0.1.0-ci.<run>.<attempt>` 包只作为 artifact。每个 SDK 的
`toad-sdk-manifest.json` 记录实际源码提交、依赖版本和平台。

验证使用本项目现有 presets、完整 build graph、正式 CTest 和 install/package
流程，不建立独立 consumer 或 smoke 工程。Toad 不使用或链接 QuickJS/Lua
适配器，SaltsUtils 0.1.4 已取消无关适配器的自动查找。CHttp 0.1.0 通过
拆分 SDK TAR 恢复，不再使用历史 `CHttp.Native` NuGet 包。
当前选择已发布的 CMeta 0.1.7，其二进制与 Base 0.1.8 匹配，并通过既有
SameMinorVersion 契约满足 CHttp 的 CMeta 0.1.6 要求。Windows MSVC 的完整
发布 SDK 恢复已通过；未使用本地源码候选 SDK。
本机 LLVM-MinGW 的完整构建、59 项正式 CTest 及安装已通过。
发布 CI 按当前 presets 资格化 Windows LLVM-MinGW x64、Linux Clang x64、
macOS Clang arm64 和 Android arm64-v8a；原生平台运行完整 CTest，Android
检查交叉构建、安装和 ELF 对齐。实际结果以发布的 producer workflow 为准。
