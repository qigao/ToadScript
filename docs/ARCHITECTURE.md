# Toad Architecture

## Overview

Toad is a high-performance embeddable scripting engine with TypeScript-like syntax.
The public execution backend is MIR only:

```text
source
  -> lexer/parser
  -> AST
  -> validation
  -> MIR lowering
  -> MIR module
       |-> MIR interpreter   (`toad_script_run`)
       |-> MIR native code   (`toad_script_run_jit`)
```

The AST is the frontend representation. It is not a public execution backend.
Unsupported language forms must fail during validation, MIR lowering, or runtime helper execution.

## Frontend

The frontend is implemented by `engine`:

- `parser/lexer.re`: re2c lexer.
- `parser/grammar.y`: Lemon grammar.
- `include/toad/types.h`: AST nodes and runtime value shapes.
- `src/validate.c`: validation and semantic checks.

The parser owns syntax and AST construction. Constant folding and simple syntax normalization can happen here, but execution policy belongs to the MIR backend.

## Runtime State

`toad_script_ctx_t` owns one script environment and the MIR state:

- `toad_env_t env`: variables, constants, functions, classes, modules, and runtime error state.
- `mir_interp_ctx`: MIR context used by `toad_script_run` and `toad_script_run_mir_interp`.
- `mir_ctx`: MIR context used by `toad_script_run_jit`.
- JIT cache: script hash to compiled native function pointer.
- Plugin handles and script import state.

Variables persist across runs on the same context because the environment is the single source of runtime state.

## MIR Backend

`script/src/mir.c` is the backend:

- Parses and validates source.
- Lowers AST nodes to MIR instructions.
- Emits runtime helper calls for dynamic values, OOP, maps, strings, parser/data binding, and other non-scalar operations.
- Emits optimized paths for numeric expressions, loops, direct math calls, OOP method call caches, and public numeric instance fields after validating the current receiver through CMeta. Vector indexing and map reads resolve the current environment value at each access rather than caching pointers into mutable containers.
- Executes the same MIR through either MIR interpreter or MIR JIT.

`toad_script_run()` and `toad_script_run_jit()` share this pipeline. They differ only in the final MIR execution mode.

## Runtime Helpers

Runtime helpers are C functions callable from MIR. They are used when an operation is dynamic or host-owned:

- environment load/store
- string/list/map/vector operations
- module dispatch
- OOP class definition, instance creation, member access, method dispatch, `super`, and `instanceof`
- parser/data binding, file, datetime, math/statistics, and plugin-backed functionality

Helpers must preserve the same semantics in MIR interpreter and MIR JIT mode. They should report errors through the context/environment state instead of manufacturing default values that hide failures.

## Modules

Core built-ins live under `engine/src` and optional extensions live under `modules/*`.

Important module groups:

- `core`, `math`, `stats`, `string`, `regex`, `io`
- `mapper`: class-first JSON/YAML/XML mapping through Salts DataBind parser APIs
- `ts`, `ta`, `fin`: time-series, technical analysis, and finance helpers
- `net`, `db`, and other plugin modules

The mapper is the canonical path for structured JSON/YAML/XML values. Toad
class field declarations are the type metadata and Salts DataBind parser APIs
owns document syntax. Data modules should consume typed class instances rather
than introducing a second schema format.

## Error Policy

Toad should fail explicitly when a script cannot be represented by MIR:

- parse errors use `TOAD_SCRIPT_ERROR_PARSE`
- validation errors use `TOAD_SCRIPT_ERROR_VALIDATE`
- MIR lowering or native compilation errors use `TOAD_SCRIPT_ERROR_JIT`
- runtime helper failures use `TOAD_SCRIPT_ERROR_RUNTIME`

No execution mode should silently switch to another backend. The host may choose MIR interpreter or MIR JIT explicitly, but both use the same MIR lowering contract.

## Testing

Use focused tests first:

- `test_script_mir`: MIR lowering, MIR interpreter, MIR JIT, and parity between both modes.
- `test_script`: public host API behavior.
- module-specific tests under `modules/*/test`.

For performance-sensitive changes, run `bench_script_mir` and compare warm/JIT exec-only results.

## Split SDK migration (2026-11-12)

The former Salts aggregate SDK has separate owners: SaltsBase provides native
systems primitives, parsers, TinyTest and coroutine execution; CMeta provides
reflection, CFlow, containers and Plugin; SaltsUtils provides DataBind; CNet
provides networking and cryptography. Toad consumes their installed
public targets and headers, with no replacement algorithms or compatibility
aliases. File/process ownership and error returns stay with Base, graph
execution stays with CFlow, and script policy stays in Toad.

Keeping the old aggregate SDK beside the new packages would mix Plugin,
reflection and native-system ABIs across module boundaries. Rewriting the
network module to bypass CHttp would duplicate protocol ownership. The chosen
approach is therefore to rebuild the existing integrations against the split
public SDKs and fail at incompatible published dependencies. It affects the
frontend, interpreter/JIT, native modules, host/plugin headers, tests, exported
CMake package and CI dependency roots. UUID storage remains 16 canonical bytes;
the native helper now accepts Base's UUID array and copies it without retaining
a borrow. All C/C++ hosts and plugins must be recompiled together.

Version selectors belong to the parent environment/.env and same-name CI
variables. Release metadata and receipts identify the restored bytes; CMake
owns required package lookup and the complete product graph. Defaults for tests
and benchmarks belong to CMakeOptions.cmake; module selection is unconditional.
Normal configure never clears caches or upgrades SDKs implicitly. Roll back by
selecting the prior coherent SDK family with its matching source revision and
separate installation directories, then configuring fresh; never combine ABI
families or overwrite another version's SDK.

The current published CHttp 2.1.0-rc.6 package still discovers monolithic Salts.
Windows MSVC configuration reaches that required lookup and stops. The
migration's compilation, linking, runtime suites and other platforms remain
unqualified until a compatible CHttp release is available. Published SDK
restoration and a successful upstream producer CI do not qualify Toad.
