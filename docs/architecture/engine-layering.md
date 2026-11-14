# Engine 与 Script 分层及命名

## 职责与依赖

`engine/` 拥有语法、AST、解释执行、动态值、环境、类与内建函数。
`script/` 拥有脚本生命周期、宿主 API、插件加载策略、任务/计时器和 MIR/JIT。
保留这两个 owner，避免让通用解释器依赖宿主调度及 MIR 生命周期。

- `TurboScript::Syntax`（`engine_syntax` 静态库）：re2c lexer、Lemon parser、AST，
  以及 `tbs_parse*`、`tbs_free`。公开依赖 Base 的日期类型及 Utils 的绑定描述。
- `TurboScript::Engine`（`engine` 静态库）：值、环境、解释执行、类、集合和内建模块。
  公开依赖 Syntax，容器实现使用 CMeta 的 CSTL。
- `TurboScript::TurboScript`（`turbo_script` 共享库）：宿主入口与 MIR/JIT。
  它依赖 Engine，插件加载实现保留在独立私有 loader 组件。

Syntax 不依赖 Engine 的执行实现；Engine 不依赖 Script 或 MIR。
目录、目标和文件名称表达 owner，不建立第二份运行时或语法事实源。

## 2026-11-13 命名迁移

旧 `exprtk/` 和 `turbo_script/` 目录分别改为 `engine/` 与 `script/`。
内部文件依赖所在目录提供上下文：`engine/src/eval.c`、`engine/src/value.c`、
`script/src/host/instance.c`、`script/src/mir/api.c`，不重复完整模块前缀。
Universe 的脚本适配为 `modules/fin/src/universe_bindings.c`，与原生
`universe.c` 区分，避免直接去前缀后发生文件覆盖。

公开头文件统一从 include 根的 `turboscript/` 命名空间访问：

| 旧入口 | 新入口 |
| --- | --- |
| `exprtk.h` | `<turboscript/engine.h>` |
| `exprtk_types.h` | `<turboscript/types.h>` |
| `exprtk_module.h` | `<turboscript/module.h>` |
| `exprtk_class.h` | `<turboscript/class.h>` |
| `exprtk_grammar.h` | `<turboscript/grammar.h>` |
| `simd_helpers.h` | `<turboscript/simd.h>` |
| `ts_plugin.h` | `<turboscript/plugin.h>` |
| `turbo_script.h` | `<turboscript/script.h>` |
| `turbo_script_export.h` | `<turboscript/export.h>` |
| `ts_plugin_loader.h` | `<turboscript/plugin_loader.h>` |

引擎 C 标识符从 `exprtk_*`、`EXPRTK_*` 改为 `tbs_*`、`TBS_*`；
解析器生成入口为 `tbsParse*`。引擎值和环境保留已有 CMeta identity 与 `stable_id` 字符串，
这些协议身份不随源码命名改动。已有字段布局、枚举数值、所有权、调用约定和错误语义保持，
没有添加版本分发、兼容别名或重试。脚本函数名、`turbo_script_*` 宿主函数、
插件的 `turboscript.module` 契约和 `turbo_script_repl` 命令名称保留。

这是原生源码及包入口的兼容性变更。宿主和插件必须更新 include、引擎符号和
CMake target 后一起重编译。旧 target `TurboScript::ExprTk/ExprTkSyntax`
对应 `TurboScript::Engine/Syntax`，不提供同名兼容层。回滚应恢复完整的前一轮
源码、头文件和构建引用，不能只替换某个旧头或已编译插件。

## 生成与验证

语法输入仍由 `engine/parser/grammar.y` 和 `lexer.re` 所有。
正式 CMake graph 生成 parser、lexer 和 namespaced token header；
`<turboscript/grammar.h>` 包装 `turboscript/detail/grammar_gen.h`。
生成文件在 build tree，安装时保留相同命名空间，不提交手写生成副本。
`parser/regenerate.bat <configured-user-preset>` 通过现有 build target 运行。

测试保留在 owner 的 `test/` 目录，名称使用 `test_engine_*` 与
`test_script_*`。使用当前 `msvc-release-user` 等 user presets 进行 configure、
完整 build 和 CTest。依赖配置失败时先解决该失败，不用临时工程证明本次迁移。
命名重写及静态检查不能代替编译、链接、生成器执行和正式运行时测试。

本次冻结 presets JSON、CMakeOptions 与公共 CMake 模块；只同步 CMakeLists 中
已有路径、源文件和目标引用，不增加配置变量或功能选项。
