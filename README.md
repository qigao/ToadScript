# Toad

Toad is an embeddable scripting engine for C/C++ applications. It uses
a TypeScript/JavaScript-like syntax and a shared MIR lowering pipeline for
interpreted and native-code execution.

The current practical focus is host-driven automation and data processing:

- math, statistics, vectors, tables, strings, files and date/time helpers;
- cooperative tasks and timers;
- HTTP/1, HTTP/2 and WebSocket clients;
- technical analysis, strategy, risk and time-series modules;
- SQLite, structured-data mapping, cryptography, rules and OS integration;
- a C API for compiling scripts, binding host values and registering native
  functions.

Toad is not a TypeScript implementation or a security sandbox. Native
plugins execute with the host process's permissions, so only trusted scripts
and plugins should be loaded. Embedders can reject unapproved native plugin
names before DLL loading with `toad_script_init_with_plugin_authorizer()`; this
is admission control, not process isolation.

## Native naming

The native SDK is named Toad. Its public entry points are:

| Area | Header | C API | CMake target |
| --- | --- | --- | --- |
| Engine | `<toad/engine.h>` | `toad_*`, `TOAD_*` | `Toad::Engine` |
| Syntax | `<toad/grammar.h>` | `toad_parse*` | `Toad::Syntax` |
| Host | `<toad/script.h>` | `toad_script_*`, `TOAD_SCRIPT_*` | `Toad::Script` |

Use `find_package(Toad CONFIG REQUIRED)` and run scripts with
`toad --file hello.tbs`. The package metadata uses `Toad.Native`. This changes
native header paths and exported symbol names; update and rebuild hosts and
plugins together. CMeta identities now use `toad.engine.*`, `toad.class.*`
and `toad.dynamic.*`; the module contract is `toad.module`. Hosts and plugins
using the former identities are incompatible with these names. Existing script
names, `.tbs` files and optional `tbs_` plugin filenames are retained. The GitHub
repository address is unchanged. See the [migration details](docs/architecture/engine-layering.md).

## Upstream dependency versions

The build consumes separately published SaltsBase, CMeta, SaltsUtils, CNet,
CHttp and TurboDB SDKs. Select exact releases in the parent environment or
local `.env`; CMake does not load dotenv files or restore SDKs implicitly.
Use the same clean release checkout for `VCPKG_CACHE_REPOSITORY_ROOT` and
`VCPKG_CACHE_VERSION`. CI reads repository variables with the same names.

The selected published set is [vcpkg-cache 0.1.3](https://github.com/qigao/vcpkg-cache/releases/tag/v0.1.3),
[SaltsBase 0.1.5](https://github.com/qigao/salts-base/releases/tag/v0.1.5),
[CMeta 0.1.5](https://github.com/qigao/cmeta/releases/tag/v0.1.5),
[SaltsUtils 0.1.2](https://github.com/qigao/salts-utils/releases/tag/v0.1.2),
[CNet 0.1.11](https://github.com/qigao/salts-cnet/releases/tag/v0.1.11), and
[TurboDB 0.1.0](https://github.com/qigao/turbodb/releases/tag/v0.1.0).
`.env.example` documents every version selector and local toolchain input.

**Current blockers (2026-11-13):** configuration first stops because the
selected SaltsUtils 0.1.2 package unconditionally searches for `qjsConfig.cmake`,
even though Toad does not use its QuickJS adapter. This is an upstream package
boundary problem; QuickJS must not be added to this repository to satisfy it.
[SaltsUtils 0.1.3 still performs the same unconditional lookup](https://github.com/qigao/salts-utils/blob/v0.1.3/cmake/SaltsUtilsConfig.cmake.in).
The selected CHttp prerelease,
[2.1.0-rc.6](https://github.com/qigao/chttp/releases/tag/v2.1.0-rc.6), still
requires the monolithic `Salts` package and `SALTS_ROOT`. Its installed
`ChttpConfig.cmake` cannot consume this split SDK family. The stable 2.0.0
package also predates the split. Compatible published CHttp and SaltsUtils SDKs
that resolve these package-boundary problems are needed before compilation and
CTest can qualify the migration. No source-build substitution or old Salts
ABI compatibility layer is used.

The restore entry point checks the selected GitHub release asset's size and
SHA-256 before extracting it. SaltsBase is also checked against its public
manifest's version, platform, triplet, successful producer CI and digest.
vcpkg-cache restoration uses the shared `resolve-release` operation to create
a literal URL/hash lock under `build/dependencies`, then `locked` to restore
it. Other SDKs and host generators use versioned destinations and receipts;
existing installations with different provenance fail instead of being replaced.
The script exports this round's `SALTS_BASE_ROOT`, `CMETA_ROOT`,
`SALTS_UTILS_ROOT`, `SALTSCORE_ROOT`, `CHTTP_ROOT`, `TURBODB_ROOT`,
`VCPKG_INSTALLED_DIR`, `RE2C_ROOT` and `LEMON_ROOT`.

Third-party dependencies use `vcpkg.json` and the selected vcpkg-cache
manifest toolchain, ports and triplets. The canonical manifest installation
completes the restored third-party tree for Toad, including MIR and
TRE. The shared NuGet feed is read-only; the local binary cache is writable.
Supply `GITHUB_TOKEN` in the parent environment for configuration; local SDK
restoration can use the active `gh` login. Toad uses its own engine and MIR;
it does not use or link the SaltsUtils QuickJS/Lua adapters. The current
SaltsUtils package's unconditional backend lookups must be corrected upstream,
rather than adding those backends to Toad or bypassing the package config.
JPEG/libyuv are required by Base's exported media targets; GmSSL
belongs to CNet's TLS/crypto dependency contract. SDKs and native plugins must
be rebuilt together when their dependency ABI changes.

## Build and test

Copy `.env.example` to an ignored `.env` and update the machine paths. Preserve
any existing credentials and unrelated settings. On Windows, run in one
PowerShell session with Python 3.12+, GitHub CLI, CMake and Ninja on `PATH`:

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Local
./cmake/WithVs.cmd cmake --fresh --preset msvc-release-user
./cmake/WithVs.cmd cmake --build --preset msvc-release-user
./cmake/WithVs.cmd ctest --preset msvc-release-user --output-on-failure --no-tests=error
./cmake/WithVs.cmd cmake --build --preset install-msvc-release-user
```

The configure command currently fails at the SaltsUtils boundary described
above; build, test and install require the dependency blockers to be resolved.
`WithVs.cmd` loads `VSDEVCMD_BAT` in the same process as CMake/CTest,
preserves the configured vcpkg checkout and propagates command failures.

Use `llvm-native-release-user` for Linux Clang/macOS with
`LLVM_NATIVE_TARGET_TRIPLET` set to the matching published triplet. Linux GCC
uses `-Profile linux-gcc-x64` and `gcc-release-user`. Windows LLVM-MinGW
requires its matching SDK profile and the shared compiler setup's toolchain.
For Android use `-Rid android-arm64-v8a -HostRid linux-x64` and
`android-arm64-release-user`. Cross builds use the restored host Lemon/re2c;
Android execution requires a separate device runner. Other advertised compiler
profiles require matching published SDKs; unsupported asset selections fail.

All bundled modules are assembled unconditionally. `BUILD_TESTS` controls
formal suites, while `BUILD_BENCHMARK` controls benchmarks registered in CTest
with the `benchmark` label. CI builds the complete configured graph, runs host
CTest suites, and installs/packages the same graph. No independent installed
consumer or smoke project is invoked. Installation defaults to
`build/<preset>/stage`; changing SDK/compiler/triplet requires an explicit
`cmake --fresh --preset <user-preset>`.

Base owns `Salts::Core`, native file/process/thread/time/UUID APIs, parsers
and coroutine execution. CMeta owns `CMeta::CMeta`, `CMeta::CFlow`,
`CMeta::CSTL`, `CMeta::Plugin` and `CMeta::PluginABI`. DataBind remains in
SaltsUtils; CNet supplies networking and TLS. Plugin macros remain
`CMETA_PLUGIN_*`, the query symbol is `cmeta_plugin_query`, and the public
plugin header is now `<cmeta/plugin.h>`. UUID values use Base's 16-byte
`uuid_t` array and are copied into expression values. Recompile C/C++ hosts
and native plugins against the new headers; script function names are retained.

Run a script with:

```text
toad --file hello.tbs
```

See [Getting Started](docs/getting-started.md), the
[language guide](docs/language-guide.md), the
[C API](script/include/toad/script.h), and the
[plugin system](docs/PLUGIN_SYSTEM.md).

## License

Toad first-party code is licensed under the Apache License 2.0. See
[LICENSE](LICENSE). Bundled and vendored third-party components retain their
respective upstream licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
