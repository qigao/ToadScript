# Toad

Toad is an embeddable scripting engine for C/C++ applications. It uses
a TypeScript/JavaScript-like syntax and a shared MIR lowering pipeline for
interpreted and native-code execution.

The current practical focus is host-driven automation and data processing:

- math, statistics, vectors, tables, strings, files and date/time helpers;
- cooperative tasks and timers;
- HTTP/1, HTTP/2 and WebSocket clients;
- technical analysis, strategy, risk and time-series modules;
- structured-data mapping, cryptography, rules and OS integration;
- a C API for compiling scripts, binding host values and registering native
  functions.

Host modules can opt into `TOAD_SCRIPT_MODULE_EXPLICIT_CALLS` for an explicitly
admitted callable set without ambient I/O or imports. See the
[capability and concurrency contract](docs/ARCHITECTURE.md#host-capability-admission-and-concurrent-initialization).
Callbacks retain the permissions granted by the host; this option does not
provide process isolation.

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
and CHttp SDKs. Select exact releases in the parent environment or
local `.env`; CMake does not load dotenv files or restore SDKs implicitly.
Use the same clean release checkout for `VCPKG_CACHE_REPOSITORY_ROOT` and
`VCPKG_CACHE_VERSION`. CI reads repository variables with the same names.

The release selectors follow [CHttp 0.1.0](https://github.com/qigao/chttp/releases/tag/v0.1.0),
the first stable release in its restarted version series:
[vcpkg-cache 0.1.3](https://github.com/qigao/vcpkg-cache/releases/tag/v0.1.3),
[SaltsBase 0.1.8](https://github.com/qigao/salts-base/releases/tag/v0.1.8),
[CMeta 0.1.7](https://github.com/qigao/cmeta/releases/tag/v0.1.7),
[SaltsUtils 0.1.4](https://github.com/qigao/salts-utils/releases/tag/v0.1.4),
[CNet 0.1.12](https://github.com/qigao/salts-cnet/releases/tag/v0.1.12).
`.env.example` documents the selectors and local toolchain inputs. These
version numbers alone do not establish a compatible binary SDK composition.

CMeta 0.1.7 now supplies the published binary composition for Base 0.1.8.
CHttp's producer used CMeta 0.1.6 built against the same Base; admission of the
explicitly selected patch upgrade follows CMeta's existing `SameMinorVersion`
contract. The restorer also enforces the selected CMeta release manifest's
exact Base dependency. The selected Windows LLVM-MinGW SDKs have been restored
from verified release assets, including CMeta 0.1.7 and CHttp 0.1.0.

**Validated locally (2026-11-14):** LLVM-MinGW configuration, Lemon/re2c code
generation, the complete build, all 59 formal CTests and preset installation
pass with this SDK composition. Cron integration has been removed, including
`timer.cron`; the released Base SDK is unchanged. Other platforms have not
been requalified by this local run.

The bundled `fuzzy` plugin and its TRE dependency have been removed.

CHttp 0.1.0 uses profile-specific TAR SDKs and the `Chttp` package with
`CHttp::` targets. It no longer requires the monolithic Salts SDK or the old
`CHttp.Native` NuGet payload. CNet's package is now `SaltsCNet`; the existing
`SALTSCORE_ROOT` environment input still selects its prefix. Utils 0.1.4 no
longer eagerly discovers QuickJS/Lua for unrelated targets, so Toad does not
need either backend to consume DataBind.

The restore entry point checks the selected GitHub release asset's size and
SHA-256 before extracting it. SaltsBase, CMeta and CHttp are also checked
against their public release manifests, including the selected platform,
version, producer evidence, digest and dependency constraints.
vcpkg-cache restoration uses the shared `resolve-release` operation to create
a literal URL/hash lock under `build/dependencies`, then `locked` to restore
it. Other SDKs and host generators use versioned destinations and receipts;
existing installations with different provenance fail instead of being replaced.
The script exports this round's `SALTS_BASE_ROOT`, `CMETA_ROOT`,
`SALTS_UTILS_ROOT`, `SALTSCORE_ROOT`, `CHTTP_ROOT`,
`VCPKG_INSTALLED_DIR`, `RE2C_ROOT` and `LEMON_ROOT`.

Third-party dependencies use the selected preinstalled vcpkg-cache triplet,
including MIR. Current presets do not run the vcpkg manifest toolchain;
CMake resolves packages from the restored prefix and fails on missing packages.
The shared NuGet feed is read-only; the local binary cache is writable.
Supply `GITHUB_TOKEN` in the parent environment for configuration; local SDK
restoration can use the active `gh` login. Toad uses its own engine and MIR;
it does not use or link the SaltsUtils QuickJS/Lua adapters. Utils 0.1.4
leaves discovery of unrelated adapters to their consumers. JPEG/libyuv belong
to Base's Media target and are not loaded for Core-only consumers. GmSSL
belongs to CNet's TLS/crypto dependency contract. SDKs and native plugins must
be rebuilt together when their dependency ABI changes.

## Build and test

Copy `.env.example` to an ignored `.env` and set the machine paths. Preserve
existing credentials and unrelated settings. The local restore wrapper loads
only the documented build/dependency fields; CMake does not read `.env`.
Use Python 3.12+, GitHub CLI, CMake and Ninja. Supply `GITHUB_TOKEN` in the
parent environment for shared vcpkg-cache restoration and the read-only NuGet
binary cache. An active `gh` session can supply it as shown below.

In one PowerShell session on Windows:

```powershell
$env:GITHUB_TOKEN = gh auth token
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Profile windows-llvm-mingw-x64 -Local
cmake --fresh --preset native-release-user
cmake --build --preset native-release-user
ctest --preset native-release-user --output-on-failure --no-tests=error
cmake --build --preset install-native-release-user
```

Run each command only after the preceding command succeeds. The existing
`native-release-user` preset selects LLVM-MinGW and enables the complete formal
test graph; `llvm-mingw-x64-release-user` builds Release without tests. For MSVC,
restore its matching SDK profile and use the existing MSVC presets through
`WithVs.cmd`, which loads `VSDEVCMD_BAT` in the same process.

The restore wrapper adds the selected SDK runtime directories to this shell's
PATH. Windows net plugins also stage and install their CMake target runtime DLLs
beside the plugin: CMeta's restricted explicit-path loader does not search PATH
for plugin dependencies.

`restore-native-sdks.ps1` calls `restore-native-sdks.py`, which restores exact
release TARs for Base, CMeta, Utils, CNet and CHttp, uses the shared locked
vcpkg-cache restore, and restores host tools through their existing package
contracts. Database integration is currently inactive; TurboDB is excluded
from SDK restoration and CMake assembly. It exports SDK paths only after the complete restore
succeeds. Keep these variables in the same shell or pass them to the IDE.
Dependency roots are versioned under ignored `build/dependencies`; synchronize
`.env` roots and selectors with the successful restoration result.

For LLVM-MinGW, set `LLVM_MINGW_ROOT` in the local `.env` to the installed
compiler directory, for example `C:/tools/llvm-mingw`, then run in the same
PowerShell session:

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Profile windows-llvm-mingw-x64 -Local
cmake --preset llvm-mingw-x64-release-user --fresh
```

The restore command loads the compiler setting and exports the matching
`qigao-x64-windows-llvm-mingw` SDK/cache paths. Starting CMake from another
shell requires loading those inputs again. Native and cross builds use the
restored host `RE2C_ROOT/bin/re2c`, `LEMON_ROOT/bin/lemon` and
`LEMON_ROOT/share/lemon/lempar.c`; the grammar build depends on these files.

Use `llvm-native-release-user` for Linux Clang/macOS with
`LLVM_NATIVE_TARGET_TRIPLET` set to the matching published triplet. Linux GCC
uses `-Profile linux-gcc-x64` and `gcc-release-user`. Windows LLVM-MinGW
requires its matching SDK profile and the shared compiler setup's toolchain.
For Android use `-Rid android-arm64-v8a -HostRid linux-x64` and
`android-arm64-release-user`. Cross builds use the restored host Lemon/re2c;
Android execution requires a separate device runner. Other advertised compiler
profiles require matching published SDKs; unsupported asset selections fail.

The release workflow uses the current presets and exact published restore entry
point for Windows LLVM-MinGW x64, Linux Clang x64, macOS Clang arm64 and Android
arm64-v8a. Native jobs build and test the complete graph; Android jobs cross-build,
install and check ELF load-segment alignment. SDK archives and `Toad.Native`
packages record their producer commit and dependency versions. The Toad release
series starts at [0.1.0](docs/releases/0.1.0.md).

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
