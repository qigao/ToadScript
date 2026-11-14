# 上游拆分 SDK 迁移记录

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
