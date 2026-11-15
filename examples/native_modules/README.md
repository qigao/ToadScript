# Application-defined CMeta modules

This example composes two independently built native libraries in one Toad
script. Native producers declare ordinary functions through CMeta; they do not
receive an Engine environment, allocate script values or implement a Toad module
vtable. These producer and Host APIs are available starting with Toad 0.1.3.

| File | Owner and responsibility |
| --- | --- |
| [calibration.c](calibration.c) | C11 plugin: normalize a sensor reading, gain, integer predicate |
| [maintenance.cpp](maintenance.cpp) | C++17 plugin: nonlinear cost and integer limit |
| [host.c](host.c) | Host: registry, explicit attachment, script, interpreter/JIT, cleanup |
| [CMakeLists.txt](CMakeLists.txt) | Two MODULE libraries and one Host executable in Toad's normal build graph |

For example, a downstream native library can declare a function with:

```c
#include <toad/plugin.h>

TOAD_PLUGIN_FUNCTION(double, square_cost, (double, value, CMETA_PARAM_IN));
double square_cost(double value) { return value * value; }

#define COST_EXPORTS(X) \
    X(function, square_cost, "square_cost", "application.cost", 1u, 0u)
CMETA_PLUGIN_DECLARE(cost, "application.cost", (1u, 0u, 0u),
    COST_EXPORTS, CMETA_PLUGIN_PASSIVE());
```

`TOAD_PLUGIN_FUNCTION` and `TOAD_PLUGIN_FUNCTION0` are scalar value-result
conveniences over CMeta's exact-invoke declarations. CMeta remains the source
of function metadata, ABI carriers and publication. Use canonical
`Function0InvokeDecl(value, void, name)` for a void result. Publish exactly one
`CMETA_PLUGIN_DECLARE` per DSO. Functions can be implemented in separate source
files; keep their declarations and single export list in the publishing source
or a shared header. The header-only `Toad::PluginABI` target supplies the Toad
headers and `CMeta::PluginABI`; these stateless producers do not link the Script
DLL or Engine runtime. The Host links `Toad::Script` and `CMeta::Plugin` from its
selected SDKs. Downstream CMake can use:

```cmake
find_package(Toad CONFIG REQUIRED)
add_library(cost MODULE cost.c)
target_link_libraries(cost PRIVATE Toad::PluginABI)

add_executable(application host.c)
target_link_libraries(application PRIVATE Toad::Script CMeta::Plugin)
```

The Host owns an address-stable `cmeta_plugin_registry`, loads each explicit
DSO path, calls `cmeta_plugin_registry_start`, then checks:

```c
toad_script_context_register_plugin(ctx, &registry, plugin, result);
```

Each export ID is its script function name. The example compiles with
`TOAD_SCRIPT_MODULE_EXPLICIT_CALLS`:

```javascript
func calculate(raw) {
    value = plant_normalize(raw, 2.0);
    return plant_cost(value, 10.0) + plant_limit();
};
export("calculate");
```

For `raw = 8`, normalization gives 3, cost gives 90, and the final result is 132.
The executable checks and prints that result for both interpreter and JIT.

Run through the repository's existing presets, with the selected SDK/toolchain
environment loaded:

```powershell
cmake --preset native-release-user
cmake --build --preset native-release-user
ctest --preset native-release-user -R '^example_native_modules$' --output-on-failure
```

CTest supplies the actual plugin paths. To use the executable directly, pass
the calibration DSO path and maintenance DSO path in that order. It returns a
nonzero status on registry, script, result or cleanup failure.

Registration admits all exports atomically before module compilation. It
rejects non-passive/interface/aggregate modules, unsupported signatures, name
collisions and exhausted capacity. It accepts 0–16 IN scalar arguments of bool,
int, long, float or double, and scalar/void results. BOOL is not coerced to a
number; int/long require a native-range INT64. Numeric arguments admit NUMBER or
INT64 within [-2^53, 2^53]; float rounds to native precision and rejects overflow.
Non-finite results fail with HOST_ERROR, which faults that instance. See the
[full contract](../../docs/PLUGIN_SYSTEM.md#explicit-host-attachment-protocol).

Toad keeps a lease until **final context cleanup**, including all retained
module, instance and result references. Destroy those objects and free the
context before stopping/unloading plugins and destroying the registry. Closing
the context early rejects further execution but retains its lease until the
last object is released. Removing an individual Host callback does not release
the provider lease. The Host must keep the registry's address and storage alive
throughout this interval. Native code is trusted; registering a function grants
that capability and does not establish purity or process isolation. C++ module
functions must not throw across the C ABI.

Existing `TOAD_PLUGIN_MODULE*`, stateful module adapters and script imports stay
available for plugins that need Engine-specific APIs. This scalar Host path
needs no changes to presets, feature options or process-wide builtin registries.
