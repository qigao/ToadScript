# 上游拆分 SDK 迁移记录

## 2026-11-14：按用户授权重启 Toad 0.1.0 发布系列

- **明确授权**：用户要求推送全部改动并发布，随后要求删除本仓库历史 releases/packages，
  从 v0.1.0 开始。操作范围是 qigao/TurboScript；保留其他项目、共享上游及 Git 提交历史。
- **发布清单**：GitHub 上有八个历史 Release（3.0.2 至 3.0.10 的实际已发布版本）和
  关联本仓库的 TurboScript.Native 包（八个版本）。已核对其他 package types，未发现
  其他关联包；精确 inventory 保存在忽略目录 build/release-0.1.0/retirement-inventory.json。
  清理安排在新源码资格化后、新版公开发布前，按已核对的 release/package 身份操作。
- **版本与名称**：CMake、公开 script.h、vcpkg manifest、NuGet 元数据统一到 0.1.0，
  新包为 Toad.Native；Native ABI epoch 保持现有值，不引入版本分发或兼容别名。
- **CI 迁移**：旧 workflow 引用已删除的独立 host-abi/sdk-smoke 工程和失效 presets。
  现在统一使用现有 ci presets、精确发布 SDK 恢复和完整 build graph；Windows
  LLVM-MinGW x64、Linux Clang x64、macOS Clang arm64 执行完整 CTest，Android
  arm64 执行交叉构建、安装及 ELF LOAD 16 KiB 对齐检查。没有改 presets 或新增项目
  开关；旧的不可执行 sanitizer jobs 不作为本次 sanitizer 资格化声明。
- **产物与不可变性**：CI 生成四个平台 archive、Toad.Native nupkg、SHA256SUMS，
  每个 SDK 含版本、source commit、producer run、profile、上游版本清单。tag 发布只
  使用通过本次 CI 的产物；不覆盖旧版本或使用 skip-duplicate/clobber 掩盖冲突。
- **本地验证**：actionlint 通过，五处版本及 PackageId 一致性检查通过。
  本机以新版本执行 ci-llvm-mingw-x64-release-user 的 fresh configure、完整 build、
  59/59 CTest、install 均通过；日志在 build/release-0.1.0/。远端结果发布后另行记录。
- **远端环境修复**：首轮 43600274695 的 Linux/macOS/Android 成功；Windows
  compiler root 为 `D:\a\_temp\...`，当前 runner CMake 4.4 写 RC compiler 文件时
  把 `\a` 当作非法转义。仅在 workflow 的 compiler 环境输入边界统一为 `/`，
  不改 presets、编译器或发布 SDK。修复后重新执行完整平台矩阵。
- **正式发布结果**：master 资格化运行 `43600628510` 全部成功后，annotated tag
  `v0.1.0` 指向 `6968c876b62005d64cb1225295ac9b5faa06ddcd`。tag producer
  [43600948801](https://github.com/qigao/TurboScript/actions/runs/43600948801)
  的四个平台、pack、publish 全部成功。历史八个 Releases/附件及 TurboScript.Native
  的八个版本已删除并复查为空；其后仅发布
  [Toad 0.1.0](https://github.com/qigao/TurboScript/releases/tag/v0.1.0) 和 public
  [Toad.Native 0.1.0](https://github.com/users/qigao/packages/nuget/package/Toad.Native)。
  下载公开资产后核对全部六个文件的大小、GitHub SHA-256、SHA256SUMS，四个平台
  archive 的版本/源码/producer/profile、公开头文件，以及 nupkg 内相同 SDK manifest
  均一致。核验回执在忽略目录 build/release-0.1.0/publish-result.json。

## 2026-11-14：Cron 移除后完成 LLVM-MinGW 构建与正式回归

- **用户授权与范围**：TurboDB 暂不启用；删除 fuzzy，取代此前 TRE → PCRE2
  迁移计划；Lemon/re2c 使用 vcpkg-cache 宿主工具；随后用户移除 Cron 并要求继续。
  当前 modules 目录只装配 net/fin，不恢复已删除的其他模块。
- **环境事实**：最初 compiler 展开为 `/bin/...` 是 shell 未加载
  `LLVM_MINGW_ROOT`。本机 `C:/tools/llvm-mingw` 为 LLVM-MinGW 20261020 / Clang
  23.1.4，与发布 compiler contract 匹配。`.env` 保留用户 compiler root 和凭据，
  SDK 与 cache 根统一为 LLVM-MinGW 及 `qigao-x64-windows-llvm-mingw`。
  `restore-native-sdks.ps1 -Local -Profile windows-llvm-mingw-x64` 只加载构建字段，
  成功恢复后向同一 shell 导出 SDK 根及运行库 PATH；不让 CMake 隐式读 `.env`。
  共享 vcpkg-cache helper 要求父环境 token；本地可先执行 `$env:GITHUB_TOKEN = gh auth token`。
- **发布依赖**：vcpkg-cache 0.1.3 / Base 0.1.8 / CMeta 0.1.7 / Utils 0.1.4 /
  CNet 0.1.12 / CHttp 0.1.0。原始 TAR/宿主工具均按既有入口恢复，不修改发布 SDK。
  MIR 已在该 cache 的 `share/MIR` 安装，根 CMake 按 http-services 的契约加入
  选定 preinstalled triplet prefix；无需重新安装 MIR 或新增配置变量。
- **删除**：fuzzy 的六个文件、装配、安装 target、TRE 依赖和相关 API 文档已移除；
  不新增 PCRE2 实现或兼容 API。TurboDB 不参与两个恢复入口及 CI 默认发布依赖。
  Cron 的 include/link 已由用户删除，本轮移除 timer 中的 Cron kind、表达式、
  墙钟路径、函数实现和注册；保留 after/every、现有有界槽位、closure retain/release
  及 executor 取消/关闭协议，并在正式 timer suite 验证删除 API 后的拒绝行为。
  Base 0.1.8 原 Cron 导出目录错误未修改，现在已无该消费依赖。
- **生成工具**：`cmake/FindTools.cmake` 从 `RE2C_ROOT/bin`、`LEMON_ROOT/bin`
  必需查找宿主程序；`LEMPAR` 必需来自 `LEMON_ROOT/share/lemon/lempar.c`。
  原生/交叉构建不再要求 in-tree lemon target 或旧 template 路径。Engine 的生成
  规则显式依赖程序、grammar、SDK template；实际 parser/lexer 生成及静态库链接已通过。
- **fin 合并修复**：补齐 universe bindings 公开声明 include，删除三个对已不存在且
  无实际用途的 ts_internal.h include，修复 ts.h/fin.h 重复 guard。TA/TS 源码仍归
  fin；五个测试迁至 fin/test 并正式注册。静态 fin 不再包含测试和多个 DSO 入口，
  各自入口组成 fin/ta/ts 插件，保留既有脚本模块名及相关测试行为。
- **MIR 合并修复（HIGH，事实）**：当前合并残留旧 exprtk/turbo_script 标识符、
  重复函数片段和错误的 managed-value helper 声明，且重新混入已移除的容器裸指针
  cache。对照仓库 `e2e0aa3` 的原修复，保留当前命名和 SDK API，恢复参数寄存器
  bound_reg、三参数 value helper 声明、按当前环境值访问容器与索引边界校验。
  删除无调用的旧指针 helper/结构。已有 MIR/OOP/host 生命周期及 slot 状态回归通过。
- **Windows 加载（HIGH，事实）**：RPC 的 `interface` 宏在显式 windows.h include
  后污染 CMeta 字段；仅在 loader 的该平台 include 边界解除宏。net.dll 已存在时，
  CMeta 的 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`
  不从 PATH 找 CHttp/CNet DLL；按 `TARGET_RUNTIME_DLLS` 在构建及安装时把确切
  target 依赖放在插件旁边，保留加载策略，不添加 cwd/PATH 插件搜索。
- **正式验证**：`cmake --fresh --preset llvm-mingw-x64-release-user` 配置/生成成功；
  `cmake --build --preset llvm-mingw-x64-release-user --parallel 3` 完整 Release 图通过。
  `cmake --fresh --preset native-release-user`、完整 build 图通过；
  `ctest --preset native-release-user --output-on-failure --timeout 45` 为 **59/59 通过**。
  `cmake --build --preset install-native-release-user --parallel 3` 通过，插件运行库已
  安装到 stage/bin/plugins。`python cmake/ci/test_github_packages.py` 为 **7/7 通过**。
  日志在忽略目录 `build/mingw-environment/`：configure-tests.log、build-tests.log、
  ctest-full.log、install.log、build-release-final.log。CodeGraph sync 成功。
- **验证边界**：本轮只资格化本机 Windows LLVM-MinGW Release；未重跑其他平台、
  sanitizer 或远端 CI。未创建独立 consumer/smoke 工程。旧源码字符串扫描测试已移除，
  managed slot 的正式 C 运行时测试继续执行。未宣称发布了新的 Toad 版本。
- **工作区边界**：保留用户已有 staged/unstaged 改动；本轮不提交、不覆盖 presets、
  CMakeOptions、CmakeUtils、Sanitizers、WithVs 或 manifest，不新增项目功能开关。


## 2026-11-14：CMeta 0.1.7 正式发布，Toad 已恢复匹配的发布 SDK

- **授权与发布**：用户明确要求发布 CMeta。源提交
  `7e8740777c0dc6caa6a00c23244520f8e11ab28a` 已推送到 CMeta master，
  源 tree 为 `00586a9aa0d15a6099c0a317e95792234fe2b63f`。
  已同步 CMeta 的 Base 0.1.8 / CNet 0.1.12 / 固定 CNet 源码 ref CI 变量。
  精确源码 CI `43589164253` 的十个平台及聚合结果全部成功。
- **发布产物**：正式稳定版 https://github.com/qigao/cmeta/releases/tag/v0.1.7
  已于 `2026-11-14T11:28:19Z` 发布。十个 SDK TAR 均为同一 CI 的原始字节，
  未重建、未重新打包；另附 release manifest 与 SHA256SUMS，共 12 个资产。
  发布前逐个核对原始 producer ZIP digest、TAR digest、SDK 版本、Base 依赖记录
  及原生 JUnit；草稿上传后再次比对全部资产大小/SHA-256，公开后复核 tag
  指向上述精确源提交。未修改旧的 v0.1.6 标签、资产或安装配置。
- **测试证据**：八个平台的原生完整 CTest graph 全部通过，包含 Linux TSAN；
  Windows 两平台各 120 项，Linux/macOS 六平台各 118 项。
  Android ARM64/x64 为交叉构建及安装验收，无设备运行声明。
  Android CMeta SDK 仅含静态 archive，没有最终共享 ELF；16 KiB PT_LOAD
  对齐应由最终消费方链接/安装验收，发布说明未声称这些 archive 具备共享 ELF 对齐。
- **Toad 接入**：`.env.example`、忽略的 `.env`、现有七个同名 CI 版本变量已
  同步为 cache 0.1.3、Base 0.1.8、CMeta 0.1.7、Utils 0.1.4、CNet 0.1.12、
  CHttp 0.1.0、TurboDB 0.1.0。恢复器实际从正式发布资产恢复完整 Windows
  MSVC SDK 集合，CMeta 的新版本与 Base 精确约束检查通过，CHttp 的同 minor
  CMeta patch admission 通过。环境根来自本轮恢复结果，未指向本地 CMeta build。
  证据：`build/chttp-0.1.0-migration/restore-cmeta-0.1.7.log`。
- **平台名称**：CNet 0.1.12 的 release profile 仍使用 `linux-x64`、
  `linux-arm64`、`macos-arm64`、`windows-x64`；恢复器显式映射相应的
  Clang/LLVM-MinGW profile，MSVC/GCC/Android 保持原名，不猜测其他资产。
- **剩余阻塞（HIGH）**：在新 SDK 恢复后再次执行现有
  `./cmake/WithVs.cmd cmake --fresh --preset msvc-release-user`，仍在读取
  presets 时失败于 `Invalid "configurePreset": "ci-linux-sanitizer-user"`。
  日志为同目录 `configure-cmeta-0.1.7.log`。冻结文件未改；未绕过 preset，
  未继续 Toad build/CTest/install/package，也未触发或宣称 Toad CI 通过。
  CMeta/Base 组合和 qjs 的旧阻塞已解除。
- **范围**：CMeta 工作区在已发布 tag v0.1.7 上保持干净。Toad 的本地迁移改动
  继续保留，未因 CMeta 发布而提交、推送或发布 Toad。


## 2026-11-14：CMeta 0.1.7 已在本地验证 Base 0.1.8

- 用户明确授权升级 CMeta。在相邻 `C:/projects/cpp/turbonet/cmeta` 仓库的
  `upgrade/base-0.1.8-20261114` 分支准备 0.1.7，选择已发布 Base 0.1.8 与
  CNet 0.1.12。MSVC x64 的完整 configure/build、120/120 项正式 CTest、
  install 均通过；安装包精确要求 Base 0.1.8。详见该仓库 `HANDOFF.MD`。
- 未发布 0.1.7，因此 Toad 的 release-only 版本选择仍为 CMeta 0.1.6，
  不将 `.env` 指向未发布的本地 SDK，也不篡改旧 CMeta 发布资产。
- CHttp 0.1.0 的生产者使用 CMeta 0.1.6。恢复器改按 CMeta 既有
  `SameMinorVersion` 契约接纳用户明确选择的同 minor 新 patch；
  继续通过 CMeta 自己的发布清单强制 Base 精确一致，其他依赖约束保持。
  当前旧的 0.1.6/Base 0.1.8 不匹配组合仍会失败，未增加 fallback。
- 修正上一轮额外添加的 GmSSL 查找：当前项目和 CMeta 测试只消费共享 CNet，
  没有链接 `Salts::Crypto`，因此不应为未使用的导出目标引入其依赖查找。
  Toad 的 vcpkg manifest 保持原样。
- Toad 的完整构建仍受已记录的 preset 无效引用阻塞；新 SDK 的下游验证
  尚未执行。未更新远端 CI 变量、提交、推送或发布。


## 2026-11-14：接入 CHttp 0.1.0 发布 TAR，记录新的发布组合阻塞

- **发布事实**：`qigao/chttp` 的 `v0.1.0` 是重启版本序列的首个稳定版。
  SDK 为 profile-specific TAR，不再提供新的 `CHttp.Native` NuGet。
  发布源码为 `d35801162772118e3f9eee6040140c9f892947f1`，生产 CI 为
  `43581831745`；发布清单声明 Base 0.1.8、CMeta 0.1.6（source SDK）、
  CNet 0.1.12、Utils 0.1.4、vcpkg-cache 0.1.3。
  一手依据：https://github.com/qigao/chttp/releases/tag/v0.1.0
- **实现**：复用 `restore-native-sdks.py` 的 TAR 下载、摘要和回执流程，
  替换 CHttp 的旧 NuGet 调用，并核对 CHttp manifest、资产大小/SHA-256、
  对应生产 CI 的成功状态和源码提交、平台与依赖版本。
  CNet 使用新 `SaltsCNet` 包/资产布局，保留既有 `SALTSCORE_ROOT` 输入名；
  根 CMake 必需查找 `SaltsCNet`、显式查找其静态 Crypto 所需的 GmSSL。
  PowerShell 入口调用精确版本 Python 恢复器，支持现有调用方的 HostRid/Profile，
  本地只加载非密钥构建输入，完整成功后才导出恢复结果；错误保留非零退出码。
- **配置与环境**：`.env.example` 和忽略的本机 `.env` 已选择上述版本及本轮
  版本独立的 SDK/cache/tool 路径。未改凭据、vcpkg manifest、presets、
  CMakeOptions 或公共 CMake helper；28 个冻结文件 hash 与本轮操作前一致。
  当前版本选择是待验证组合，不能当成已可构建的 SDK 集合。
  未激活远端 CI 变量、触发 CI、发布包或修改上游 SDK。
- **已验证的恢复**：更新入口实际恢复了 Windows MSVC 的 Base/CMeta/Utils/CNet/CHttp
  资产、旧有 TurboDB/host tools 以及锁定的 vcpkg-cache。CHttp TAR 大小为
  3474944 bytes，SHA-256 为
  `565a977cd4dfc7b4e33547684338353b0b68afc75541ce85f0588b6dd0de6758`。
  初次资产恢复日志在 `build/chttp-0.1.0-migration/restore.log`；
  PowerShell 入口复用验证日志在同目录 `restore-wrapper.log`。
- **恢复阻塞（HIGH）**：进一步核对发现，已发布 CMeta 0.1.6 的 manifest
  和安装包要求 Base 0.1.6 EXACT。CHttp/Utils 的生产者使用的是同一 CMeta 源码
  基于 Base 0.1.8 构建的 source SDK，不等于该已发布二进制组合。
  已增加 CMeta manifest 约束检查，再次通过正式恢复入口验证会失败于：
  `CMeta 0.1.6 release requires SaltsBase 0.1.6; selected 0.1.8`。
  完整恢复结果不再被导出。日志为同目录 `restore-compatibility.log`。
  最新 CMeta 发布仍为 0.1.6；遵守只使用已发布 SDK 的授权，不构建其源码、
  不修改 EXACT、不换回旧 Base，也不伪造包/target。需要匹配 Base 0.1.8 的
  正式 CMeta 二进制发布后才能继续。
- **配置阻塞（HIGH，独立于依赖）**：实际执行
  `./cmake/WithVs.cmd cmake --fresh --preset msvc-release-user` 时，
  CMake 在读取当前 presets 时先失败：`Invalid "configurePreset": "ci-linux-sanitizer-user"`。
  `cmake --list-presets` 同样失败。原始配置日志为同目录 `configure-msvc.log`。
  用户要求冻结 presets，因此未改无效引用，也未绕过 presets 创建验证工程。
- **旧阻塞已过期**：Utils 0.1.4 不再无条件查找无关的 QuickJS/Lua targets；
  CHttp 0.1.0 不再要求旧聚合 Salts。Toad 未新增、使用或链接 QuickJS。
- **验证结果与范围**：Python/PowerShell 语法、变更范围的空白检查通过，
  `python cmake/ci/test_github_packages.py` 的 6 项现有脚本测试通过。
  这些测试不证明新 SDK 的 C/C++ ABI 或运行时行为；build、CTest、install、
  package 均未运行。当前 CI 另含历史 NuGet 恢复调用及旧 ci-* preset 引用，
  本轮没有将这些旧 CI jobs 改写为新的发布流程。源码与证据临时文件仅在忽略的
  `build/chttp-0.1.0-migration/` 和原有依赖目录。


## 2026-11-13：明确禁止引入 QuickJS

- **用户约束**：本仓库不使用 QuickJS，不为通过配置而添加 qjs、QuickJS
  或 Lua 适配器依赖。Toad 的执行实现仍为自身 Engine 与 MIR。
- **事实与归属**：根 manifest 没有 qjs，生产源码及 CMake 没有 QuickJS API
  或 `Salts::QuickJS` 链接；现有 `Salts::DataBind`、`Salts::Mustache` 等
  目标需要 SaltsUtils，因此不能直接删掉其必需查找。
  已安装的 SaltsUtils 0.1.2 包配置无条件执行
  `find_dependency(qjs CONFIG REQUIRED)` 和 `find_dependency(unofficial-lua CONFIG REQUIRED)`，
  即使下游未使用该包导出的 QuickJS/Lua targets。
- **发布核对**：读取 GitHub 发布 API 及 `v0.1.3` 的
  `cmake/SaltsUtilsConfig.cmake.in`，最新发布仍无条件查找两个后端；
  仅升级该版本无法解决此问题，且它要求另一组匹配的 Base/CMeta SDK。
  一手依据：https://github.com/qigao/salts-utils/blob/v0.1.3/cmake/SaltsUtilsConfig.cmake.in
- **处理**：修正 README 和包文档中将此描述为 Toad 必需依赖的歧义，
  明确记录为上游包依赖边界阻塞。不编辑发布 SDK、不直接 include 其 targets、
  不伪造 qjs 包或 target、不删除必需的 DataBind 依赖，不改 presets/manifest/环境。
- **验证限制（HIGH）**：等待能按实际使用能力查找依赖的正式 SaltsUtils SDK；
  本轮仅修正文档，未重复此前失败的 configure/build/CTest。


## 2026-11-13：补齐 Toad 运行时身份更名

- **触发与选择**：用户指出旧名称仍存在。本轮将先前刻意保留的运行时
  CMeta/插件身份一并更名，取代下方历史记录中的“保留旧协议身份”决定。
- **改动**：引擎 identity、stable_id 和 shape 使用 `toad.engine.*`；
  动态值 shape 使用 `toad.dynamic.*`；类命名域及 hash salt 使用
  `toad.class.*`。插件模块 export/contract ID 为 `toad.module`，错误空间
  为 `toad.plugin`，策略 Function contract 为 `toad.fin.strategy.kelly`。
  同步既有 CMeta 回归断言、头文件说明、README、包及迁移文档。
- **兼容影响（HIGH）**：旧身份的插件及描述符不兼容，类的派生身份也改变；
  宿主与插件必须统一重编译，不新增旧身份别名或并行接口。
  字段布局、枚举值、所有权和脚本语言函数未改。
- **检查结果**：对 `engine/`、`script/`、`modules/` 不区分大小写检索
  两个旧名称均为零匹配。8 个源码/头文件/测试文件相对本轮备份的空白检查
  通过，更新文档的 `git diff --check` 通过；39 个冻结文件 hash 未变。
  当前实际文件名也无旧名称；Git 状态中此前待删除的旧路径未被恢复。
- **剩余范围**：真实 GitHub URL、迁移历史、冻结的公共构建文件和未调用的
  旧独立 CI 验证工程仍含历史名称。本轮不改冻结配置，也不重新启用旧工程。
- **验证限制**：上一轮正式配置已在 SaltsUtils 缺少 `qjsConfig.cmake` 时失败，
  证据为 `build/configure-toad-msvc.log`；配置输入未改，本轮未重复配置，
  未执行 build/CTest/install/package，不能宣称运行时身份迁移已通过测试。


## 2026-11-13：项目更名为 Toad

- **授权与范围**：用户选择 Toad 并要求继续，统一产品、公开 C API、头文件、
  CMake 包/targets、CLI、NuGet 元数据、现有测试和文档。迁移与回滚说明见
  `docs/architecture/engine-layering.md`。未改实际工作区目录或 GitHub 仓库地址，
  未发布包、提交或推送。
- **新入口**：`<toad/engine.h>` / `toad_*` / `Toad::Engine`，
  `<toad/script.h>` / `toad_script_*` / `Toad::Script`；语法 target 为
  `Toad::Syntax`，CLI 为 `toad`，包配置为 `ToadConfig.cmake`，
  NuGet 元数据为 `Toad.Native`。需要宿主与插件统一更新并重编译。
- **兼容边界**：已有结构布局、枚举值、所有权及错误语义不改；稳定
  `turboscript.*` CMeta/插件身份、脚本函数、`.tbs` 扩展名、原生插件的
  逻辑输出名和既有可选 `tbs_` 文件名前缀保留。没有新增兼容别名或版本接口。
- **配置冻结**：39 个受保护文件的 SHA-256 与本轮操作前一致，包含所有
  presets JSON、CMakeOptions、公共 CMake 模块和依赖恢复入口。
  `vcpkg.json` 仅包名改为 `toad`，依赖及其他字段不变；未改 `.env` 或
  CI variables，未新增构建选项。备份与路径映射仅位于忽略的 `build/toad-rename/`。
- **静态验证**：`cmake --list-presets`、`actionlint .github/workflows/native-sdk-release.yml`
  通过；NuGet XML 和导出检查器的 Python 语法有效，公开头文件的 60 个声明与
  export allowlist 一致（尚非二进制导出/ABI 验证）。相对本轮改名前备份的
  254 个文件通过 `git diff --no-index --check` 空白检查；交接与更新文档的
  `git diff --check` 通过。CodeGraph 最终同步成功。
- **正式配置（HIGH，仍阻塞）**：加载现有环境后执行
  `./cmake/WithVs.cmd cmake --fresh --preset msvc-release-user`，失败于
  SaltsUtils 0.1.2 的 `find_dependency(qjs CONFIG REQUIRED)`，缺失
  `qjsConfig.cmake`。原始日志为 `build/configure-toad-msvc.log`。
  未继续 build、CTest、install 或打包。下方已记录的 CHttp 发布包与
  生成器、模块合并/删除问题仍需解决；其中旧 `_tbs` target 引用已随命名
  改成 `toad_*_plugin`，缺失实现或 target 本身未被恢复或跳过。


## 2026-11-13：Engine/Script 命名迁移与配置冻结

- **授权范围**：接续用户的 `exprtk/` → `engine/`、`turbo_script/` → `script/`
  目录调整，去掉内部文件的重复前缀；公开头改用 `<turboscript/...>`，
  引擎符号改用 `tbs_*`/`TBS_*`，对应公开 CMake targets 为
  `TurboScript::Engine`、`TurboScript::Syntax`。
  `turbo_script_*` 宿主函数、共享库/CLI 名称和插件契约保留。
- **用户最新约束**：当前 presets JSON、CMakeOptions、公共 CMake 模块和依赖配置冻结，
  不增加配置变量或选项。参考 `C:/projects/cpp/turbonet/http-services/CMakeLists.txt`
  的现有依赖边界；本次只同步 CMakeLists 中必要的目录、源文件和目标引用。
  操作前后校验 39 个受保护配置文件的 SHA-256，全部保持一致。
  未修改 `.env`、CI variables 或其他工程。
- **实现**：公开头归 `engine/include/engine/` 与
  `script/include/turboscript/`；生成 token header 归 build tree 的
  `turboscript/detail/grammar_gen.h`，安装布局一致。
  内部 include 按目标文件重算相对路径，避免多个 `internal.h` 被错误解析。
  Universe 原生实现与脚本适配分别为 `universe.c` 和 `universe_bindings.c/.h`，
  防止直接去前缀造成覆盖或局部头文件遮蔽。
- **兼容性**：本次是原生源码/include/target 名称变更，所有宿主和插件需更新后重编译。
  结构布局、枚举数值、所有权与错误语义未改；已有 CMeta identity 与 stable_id
  字符串（含历史 `turboscript.exprtk.*`）按协议身份保留，不能为了删去旧名称而更改。
  正式 CMeta 回归用例仍断言这些稳定身份。迁移表、取舍与回滚见
  `docs/architecture/engine-layering.md`。未添加兼容别名或版本分发层。
- **实际验证**：`cmake --list-presets`、`actionlint`、本次改动范围的
  `git diff --check` 通过；受保护配置 hash 比对通过。
  对现有 `msvc-release-user` 执行配置，原始日志保留在
  `build/configure-naming-msvc.log`，退出码为 1。
- **当前第一阻塞（HIGH）**：所选 SaltsUtils 0.1.2 的安装配置执行
  `find_dependency(qjs CONFIG REQUIRED)`，当前 manifest 安装结果不含
  `qjsConfig.cmake`。配置停在根 CMakeLists 的 SaltsUtils 查找，尚未到达
  CHttp、源码目录、生成器、编译、链接或正式 CTest。
  未补 manifest 依赖、切换 SDK、跳过模块或建立独立验证工程。
  先修复当前依赖事实源，再使用现有 preset 完成正式验证。
- **已有合并状态的后续问题（HIGH，静态事实）**：
  `cmake/FindTools.cmake` 仍要求已删除的 in-tree Lemon target/template；
  根安装列表和 Script 测试/CLI 仍引用已移除目录对应的
  `ta_tbs`、`ts_tbs`、`mapper_tbs`、`crypto_tbs` 等 target；
  `fin/src` 中有测试 TU，仍被生产源码 glob 收集；timeseries 源码仍引用缺失的
  `ts_internal.h`，fuzzy 仍引用当前源树/所选 SDK 中缺失的 automaton 头。
  它们来自本次命名前的模块合并/删除，未通过恢复用户删除的目录或静默跳过测试处理。
- **检查限制（MED）**：命名重写和静态检查不能证明行为兼容；全部运行时测试与
  安装/打包仍未执行。全工作区 `git diff --check` 另有未改动文件中的既有空白问题：
  `vcpkg.json`、`vcpkg-configuration.json` 和 `modules/CMakeLists.txt`；冻结文件未为
  清理这些空白而改写。临时备份和命名映射仅在忽略的 `build/naming-migration/`。

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
