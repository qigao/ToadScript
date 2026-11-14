# 上游拆分 SDK 迁移记录

## 2026-11-12：已发布 CHttp 阻塞完整构建（HIGH）

- **触发条件**：以最新拆分 SDK 执行
  `./cmake/WithVs.cmd cmake --fresh --preset msvc-release-user`。
- **事实**：已恢复并校验 vcpkg-cache 0.1.3、SaltsBase 0.1.5、CMeta 0.1.5、
  SaltsUtils 0.1.2、CNet 0.1.11、TurboDB 0.1.0、re2c 4.6.7、Lemon 1.0.0
  和 CHttp 2.1.0-rc.6。版本与本轮 SDK 根记录在忽略的
  `build/native-sdk-environment.json`；下载摘要和安装回执在
  `build/dependencies/`。本机 `.env` 的相关非密钥字段已同步，既有凭据保留。
- **事实**：vcpkg manifest 安装完成，MSVC 19.44 编译器探测及
  SaltsBase/CMeta/SaltsUtils/SaltsCore 必需查找成功。随后
  `CMakeLists.txt` 的 CHttp 必需查找失败：
  `SALTS_ROOT must name the installed Salts SDK`。
  详细日志：`build/configure-msvc-release-user.log`。
- **原因**：[CHttp 2.1.0-rc.6](https://github.com/qigao/chttp/releases/tag/v2.1.0-rc.6)
  的已安装 `ChttpConfig.cmake` 仍读取 `SALTS_ROOT` 并执行
  `find_dependency(Salts)`，要求旧聚合 SDK。稳定版 2.0.0 也早于拆分。
  新 CNet 不能作为旧 `Salts` 包的别名，混用会越过反射、插件及原生系统 ABI 边界。
- **授权与处理**：用户明确选择“只使用已发布版本，记录 CHttp 阻塞”。
  未构建 CHttp 当前源码，未修改其发布包，未恢复旧聚合 SDK，也未跳过 net 模块。
  保留配置阶段的原始失败；不继续执行依赖失败配置的 build/CTest/install。
- **继续条件**：发布与 Base 0.1.5、CMeta 0.1.5、Utils 0.1.2、CNet 0.1.11
  相容的 CHttp SDK 后，更新 `.env` 与同名 CI variable `CHTTP_VERSION`，
  重新执行 `restore-native-sdks.ps1 -Rid windows-x64 -Local`。
  使用返回的新根进行 `cmake --fresh --preset msvc-release-user`，随后依次执行
  同名完整 build、CTest 和 `install-msvc-release-user` preset。
- **已完成检查**：SDK 恢复及匹配回执复用成功；CMake configure/build/test preset
  列表解析成功；JSON/YAML、Python 与 PowerShell 语法检查成功；
  `actionlint .github/workflows/native-sdk-release.yml` 和 `git diff --check` 成功。
  `qigao/TurboScript` 的七个同名 CI 版本变量已写入并读回核对，值与 `.env.example`
  一致；未触发 workflow、提交代码或发布包。
- **未验证范围（MED）**：TurboScript 源码及测试尚未编译、链接或运行；
  CHttp 后续依赖查找、生成器、模块与测试图、安装/打包，以及 Linux/macOS/Android
  的完整构建均未通过实际执行资格验证。CI 静态检查不代表 CI 已运行。
  后续可能仍需依据编译错误修正 CMeta/CFlow/DataBind 等集成；不能将本次迁移宣称为完成。

## 迁移时的配置事实源

- 原有工作区已有暂存与未暂存的 preset/依赖改动。本次接续它们，未清空暂存区或提交。
  `AGENTS.md` 等既有用户改动保留；`.codegraph/` 是忽略的本地分析索引。
- 包归属：Base 保留 `Salts::Core`、TinyTest、原生系统 API 与解析器；
  CMeta 使用 `CMeta::CMeta/CFlow/CSTL/Plugin/PluginABI`；Utils 提供 DataBind；
  CNet 的 CMake 包名仍为 `SaltsCore`，根为 `SALTSCORE_ROOT`。
- Base 的线程/文件/进程/时间 API 已去掉旧 `cmeta_*` 前缀；UUID 是 `uuid_t`
  字节数组，表达式构造函数复制 16 字节，不保留调用方指针。头文件及 C++ 宿主、
  插件需一起重编译；脚本函数名保持原有名称。范围与取舍见
  `docs/ARCHITECTURE.md` 的 Split SDK migration 章节。
- 共享 vcpkg release 的 `versions/baseline.json` 不包含 MIR/TRE 的 registry 条目。
  它们来自同一精确 checkout 的 overlay ports；不能仅向 registry packages 列表添加名称。
  `quickjs-ng` 的 port 名不能写成 `quickjs`。Lua/QuickJS-NG 与 JPEG/libyuv 是所选
  上游包导出配置的必需依赖，不是新引入的 TurboScript 执行后端。
- SDK 升级使用不同版本/平台目录；vcpkg 的本轮目录由 manifest 正常完成依赖安装。
  不覆盖其他工程的已安装 SDK，不修改共享基础设施 checkout。
  CI 复用所选基础设施 checkout 的 `setup-vcpkg-curl-cache`，使用其精确工具与缓存策略；
  本地恢复默认使用只读 NuGet feed 和可写本地缓存，保留显式父环境缓存设置。
- 模块无条件组装；正式测试使用 `BUILD_TESTS`，benchmark 使用 `BUILD_BENCHMARK`。
  测试归原模块，benchmark 经 CTest 的 `benchmark` 标签运行。
  CI 使用现有产品 user presets，不再调用独立 installed-consumer/smoke 工程；
  MIR 的源码 marker 自证检查不再注册为运行时测试，原有正式 MIR/Host suites 保留。
  Linux 发布图使用 `llvm-native-benchmark-user`，通过同一轮 CTest 执行正式 suites
  和 benchmark。未提供完整匹配 SDK 的 Windows ARM64、Android x64、macOS GCC
  和 Linux ARM64 GCC 入口不再对外展示。
