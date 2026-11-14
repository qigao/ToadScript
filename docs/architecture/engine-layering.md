# Engine 与 Script 分层及命名

## 职责与依赖

`engine/` 拥有语法、AST、解释执行、动态值、环境、类与内建函数。
`script/` 拥有脚本生命周期、宿主 API、插件加载策略、任务/计时器和 MIR/JIT。
保留这两个 owner，避免让通用解释器依赖宿主调度及 MIR 生命周期。

- `Toad::Syntax`（`toad_syntax` 静态库）：re2c lexer、Lemon parser、AST，
  以及 `toad_parse*`、`toad_free`。公开依赖 Base 的日期类型及 Utils 的绑定描述。
- `Toad::Engine`（`toad_engine` 静态库）：值、环境、解释执行、类、集合和内建模块。
  公开依赖 Syntax，容器实现使用 CMeta 的 CSTL。
- `Toad::Script`（`toad_script` 共享库）：宿主入口与 MIR/JIT。
  它依赖 Engine，插件加载实现保留在独立私有 loader 组件。

Syntax 不依赖 Engine 的执行实现；Engine 不依赖 Script 或 MIR。
目录、目标和文件名称表达 owner，不建立第二份运行时或语法事实源。

## 2026-11-13 命名迁移

旧 `exprtk/` 和 `turbo_script/` 目录分别改为 `engine/` 与 `script/`。
内部文件依赖所在目录提供上下文：`engine/src/eval.c`、`engine/src/value.c`、
`script/src/host/instance.c`、`script/src/mir/api.c`，不重复完整模块前缀。
Universe 的脚本适配为 `modules/fin/src/universe_bindings.c`，与原生
`universe.c` 区分，避免直接去前缀后发生文件覆盖。

公开头文件统一从 include 根的 `toad/` 命名空间访问：

| 旧入口 | 新入口 |
| --- | --- |
| `exprtk.h` | `<toad/engine.h>` |
| `exprtk_types.h` | `<toad/types.h>` |
| `exprtk_module.h` | `<toad/module.h>` |
| `exprtk_class.h` | `<toad/class.h>` |
| `exprtk_grammar.h` | `<toad/grammar.h>` |
| `simd_helpers.h` | `<toad/simd.h>` |
| `ts_plugin.h` | `<toad/plugin.h>` |
| `turbo_script.h` | `<toad/script.h>` |
| `turbo_script_export.h` | `<toad/export.h>` |
| `ts_plugin_loader.h` | `<toad/plugin_loader.h>` |

引擎 C 标识符从 `exprtk_*`（中间态为 `tbs_*`）改为 `toad_*`、`TOAD_*`；
解析器生成入口为 `toadParse*`。按完整更名要求，CMeta identity、`stable_id`、
shape 和类身份 hash 的命名域统一为 `toad.engine.*`、`toad.dynamic.*`、
`toad.class.*`；插件模块契约为 `toad.module`，插件错误空间为 `toad.plugin`，
策略 Function 契约为 `toad.fin.strategy.kelly`。旧身份不提供兼容别名；
类的派生身份也会改变，跨宿主/插件边界必须使用同一轮重编译产物。
这取代上一轮保留旧身份字符串的选择。已有字段布局、枚举数值、所有权、调用约定和错误语义保持，
没有添加版本分发、兼容别名或重试。宿主 API 从 `turbo_script_*` 改为
`toad_script_*`，简写 `ts_bind_*`/`ts_get_*` 也归入 `toad_script_*`。
CLI 从 `turbo_script_repl` 改为 `toad`，共享库名为 `toad_script`。
脚本函数名、`.tbs` 扩展名和既有可选
`tbs_` 插件文件名前缀保持，原生插件主输出仍为 `net`、`fin` 等逻辑名称。

这是原生源码及包入口的兼容性变更。宿主和插件必须更新 include、引擎符号和
CMake target 后一起重编译。原有 `TurboScript::ExprTk/ExprTkSyntax`（中间态为
`TurboScript::Engine/Syntax`）改为 `Toad::Engine/Syntax`，
`TurboScript::TurboScript` 改为 `Toad::Script`；包名改为 `Toad`，
NuGet 元数据改为 `Toad.Native`。候选方案是仅改显示名并保留旧原生符号；
采用统一原生命名便于宿主识别归属，代价是需要重编译，不提供旧名字兼容层。
实际 GitHub URL 保持，未执行远端仓库改名或发布。回滚应恢复完整的前一轮
源码、头文件和构建引用，不能只替换某个旧头或已编译插件。

## 生成与验证

语法输入仍由 `engine/parser/grammar.y` 和 `lexer.re` 所有。
正式 CMake graph 生成 parser、lexer 和 namespaced token header；
`<toad/grammar.h>` 包装 `toad/detail/grammar_gen.h`。
生成文件在 build tree，安装时保留相同命名空间，不提交手写生成副本。
`parser/regenerate.bat <configured-user-preset>` 通过现有 build target 运行。

测试保留在 owner 的 `test/` 目录，名称使用 `test_engine_*` 与
`test_script_*`。使用当前 `msvc-release-user` 等 user presets 进行 configure、
完整 build 和 CTest。依赖配置失败时先解决该失败，不用临时工程证明本次迁移。
命名重写及静态检查不能代替编译、链接、生成器执行和正式运行时测试。

本次冻结 presets JSON、CMakeOptions 与公共 CMake 模块；只同步 CMakeLists 中
已有路径、源文件和目标引用，不增加配置变量或功能选项。
