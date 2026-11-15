# Toad Native Plugin System

Toad uses the **current Salts Plugin ABI** for native plugin discovery,
loading, lifecycle, leases, and capability publication.

Toad does not define a second generic native-plugin ABI.

## Architecture

```text
import("fin")
   |
   v
Toad language/import layer
   |
   v
CMeta::Plugin registry
  load -> start -> acquire lease
   |
   v
cmeta_plugin_manifest
   |
   +--> FUNCTION export
   |      CMeta FunctionDesc
   |      CMeta FunctionAbi
   |      exact invoke/context
   |
   +--> INTERFACE export
          CMeta InterfaceDesc
          { self, vtable }
   |
   v
Toad binding cache
   |
   +--> interpreter / JIT calls
   +--> CFlow admission for graphable Functions
```

### Ownership

- **CMeta::Plugin** owns DSO loading, the single Plugin ABI, lifecycle, and
  leases.
- **CMeta** owns native function/interface type, ABI, effects, properties,
  direction, and ownership semantics.
- **Toad** owns `import()`, script namespaces, script-value conversion,
  diagnostics, interpreter/JIT execution, and Host Module ABI.
- **CFlow** may consume a pre-bound canonical Function when its reflected
  effects and type contract allow graph admission.

The Host Module ABI used to execute compiled Toad programs is separate
from the Salts Plugin ABI used to publish native capabilities.

### Explicit Host attachment protocol

Native module producers publish immutable CMeta Function exports with
`CMETA_PLUGIN_DECLARE`; exact invocation is generated from the same Function
declaration. Toad's legacy per-context module macros also use that declaration
machinery rather than maintaining another manifest/query implementation.

`toad_script_context_register_plugin(ctx, registry, ref, result)` attaches a
STARTED, passive function module to the existing Host callback registry. The
caller owns the address-stable CMeta registry; Toad owns one acquired Plugin
lease and a bounded immutable scalar binding cache until final context cleanup.
The registry must outlive the context and all its retained modules/instances/results.
No DSO search, start, automatic import or alternate loader occurs in this call.

Admission is serialized on the context owner thread and frozen while any Host
module exists. All export IDs, canonical Function contracts, scalar signatures,
capacity and collisions are checked before publication; a failed attachment
releases its candidate lease and leaves existing callbacks unchanged. The bound
cache is never discovered again in the invocation path. Each context admits at
most 16 attached plugins and 256 total callbacks; native functions have at most
16 IN parameters. No new queue, process registry or asynchronous execution is
introduced. On context teardown, callback borrows end before leases are released.

This route deliberately admits only passive FUNCTION manifests in the existing
bool/int/long/float/double scalar universe (plus void results). Interface modules,
pointer/OUT/aggregate signatures and generated DataBind Service bindings retain
their existing loader paths; they are rejected by this explicit scalar route,
never omitted or converted to another representation. Plain identifier export
IDs are called directly from closed scripts. Native code is a trusted host
capability, not proof of purity or isolation. Existing import authorization and
EXPLICIT_CALLS defaults remain unchanged; attaching a lease does not authorize
ambient imports or permit native module loading from a closed script.

Migration is additive at the public C API. Existing imports and module ABI
layouts remain supported. Applications can move native math functions to CMeta
declarations and attach their already-loaded modules before script compilation;
rollback keeps the previous host and plugin build together. The CMeta ABI epoch
is not duplicated or expanded into a Toad-specific versioned interface.

## Loading and discovery

For a logical import such as:

```javascript
import("fin");
```

Toad resolves the platform plugin filename and loads it through
`cmeta_plugin_registry_*`. The DSO must publish the current
`cmeta_plugin_query` entry and a valid current-ABI manifest.

The loader searches package/executable plugin locations; it does not search the
process current working directory.

Typical names:

```text
Windows  fin.dll
Linux    fin.so
macOS    fin.dylib
```

Some repository plugins use collision-safe physical filenames while keeping the
same logical plugin ID.

## Canonical Function exports

Use a Function export for ordinary stateless native operations.

The semantic source of truth is:

```text
FunctionDesc + FunctionAbi + exact invoke adapter
```

The Toad dispatch row is only a cached language binding.

The following producer works in C11 and C++17. It needs no handwritten invoke
thunk, mutable export array, once initializer, manifest or query function:

```c
#include <toad/plugin.h>

TOAD_PLUGIN_FUNCTION(double, my_double,
    (double, value, CMETA_PARAM_IN));

double my_double(double value) {
    return value * 2.0;
}

#define MATH_EXPORTS(X) \
    X(function, my_double, "my_double", "example.math", 1u, 0u)
CMETA_PLUGIN_DECLARE(math_module, "my_math", (1u, 0u, 0u),
    MATH_EXPORTS, CMETA_PLUGIN_PASSIVE());
```

The script-visible name comes from `export_id`. A Host application loads and
starts the plugin with `cmeta_plugin_registry_*`, then explicitly attaches it:

```c
status = toad_script_context_register_plugin(ctx, &registry, plugin, result);
/* Check status before compiling or using the registered functions. */
```

It can now compile a closed module with `TOAD_SCRIPT_MODULE_EXPLICIT_CALLS`:

```javascript
func calculate(x) { return my_double(x); };
export("calculate");
```

See [the complete two-module example](../examples/native_modules/README.md) for
C and C++ producers, host lifecycle, CMake targets and a runnable CTest command.
The exported header-only `Toad::PluginABI` target supplies the producer headers
and canonical `CMeta::PluginABI` without linking the Engine or Script runtime.
The explicit attachment API and producer target are available starting with
Toad 0.1.3; SDK consumers must select 0.1.3 or newer to use them.
Legacy scripts may still use `import("my_math")` in their existing loader path;
closed Host modules do not authorize ambient imports.

### Current scalar binding boundary

Toad currently binds the finite CMeta scalar universe directly:

- `bool`
- `int`
- `long`
- `float`
- `double`
- zero through 16 `IN` scalar parameters
- scalar or `void` return

Pointer/object/aggregate/opaque and OUT/INOUT signatures require an explicit
future language binding rather than guessed dynamic invocation.

Integer-valued canonical bindings preserve exact Engine value types instead of
passing through legacy integer-to-double normalization.

The explicit Host route accepts BOOL only for `bool`, and INT64 within native
range for `int`/`long`. For `float`/`double`, it accepts finite NUMBER or INT64
within [-2^53, 2^53]; float conversion rounds to native float precision and rejects
overflow. A scalar native return preserves its tag, a void return becomes NULL,
and a non-finite numeric return is a Host error. Type/conversion/invoke errors
return `HOST_ERROR` and fault that instance under the existing Host lifecycle;
destroy it before creating another instance. Native functions must not let C++
exceptions cross their C ABI boundary.

### Design choice and compatibility

The existing import adapter couples native modules to Engine environments and
cannot be used directly by applications that own a closed Host callable set.
Requiring downstream authors to duplicate manifests/thunks or to route through
that adapter would preserve the boilerplate and ownership problem. Adding a
second generic module ABI would duplicate CMeta's publication and lease rules.

The selected design reuses CMeta declarations for producers and the existing
Host callback registry for script calls. Shared private scalar admission and
conversion serve both loader paths; exact Host bindings reuse Engine's existing
value-preserving native flag, so the numeric MIR call path cannot erase integer
tags before admission. Ordinary Host callback coercion and legacy imports keep
their previous behavior. Modules requiring resources or aggregate protocols
continue through their existing interface/DataBind owner instead of claiming to
be passive scalar modules. The tradeoff is deliberate finite scalar scope and
a caller-owned registry that must remain alive until all retained Toad objects
release the context. No background loader, hot-reload protocol or purity claim
is introduced.

## CFlow admission

Plugin Functions use the same CMeta/CFlow path as other graphable callables.

A PURE compatible reflected Function may be admitted to a CFlow node. Its
`FunctionDesc` supplies effects/properties and its `FunctionAbi` supplies the
native ABI contract.

```text
Plugin Function
  -> retained lease
  -> FunctionDesc / FunctionAbi
  -> pre-bound exact adapter
  -> CFlow projection
  -> Graph / Plan
```

Functions marked STATEFUL, IO, ASYNC, MAY_FAIL where incompatible, or UNKNOWN
remain conservative graph barriers. Toad does not maintain a parallel
plugin purity registry.

The plugin lease must remain live for every graph/plan/run that can reach
plugin-owned descriptors, code, traits, context, or interface values.

## Canonical Interface exports

Use an Interface export for long-lived stateful or polymorphic providers.

Examples include:

- database/resource providers,
- network/session providers,
- device runtimes,
- other capabilities naturally represented as `{self, vtable}`.

The Interface descriptor is the native semantic contract. Script adapters
should convert Toad values at the language boundary instead of exposing
Engine types in the provider Interface.

## Migration helpers in this repository

`TOAD_PLUGIN_MODULE`, `TOAD_PLUGIN_MODULE_WITH_UNLOAD`, and
`TOAD_PLUGIN_STATEFUL` publish capabilities through the current Salts Plugin
manifest and are retained only to migrate existing modules incrementally.

They are **not** a second loader or ABI.

For new native capability design:

1. prefer canonical FUNCTION exports for ordinary operations;
2. prefer canonical INTERFACE exports for stateful providers;
3. use generated DataBind/CMeta contracts where a logical Service/Component
   contract already exists;
4. keep script namespace and conversion logic in Toad.

## Hot-path rule

Plugin discovery and reflection are control-plane work.

After import/binding, normal calls use an already-bound exact adapter or
provider vtable.

There must be no per-call:

- DSO symbol lookup,
- plugin registry lookup,
- manifest lookup,
- ABI negotiation,
- reflection catalog lookup.

## Lifecycle

Toad retains the Salts Plugin lease while plugin-owned callbacks,
descriptors, interfaces, or plan callables remain reachable.

Context teardown removes script-visible references before releasing the lease
and unloading the DSO.

## Building

Plugins should link against the installed/current Salts Plugin ABI and CMeta
headers rather than copying ABI structs into project-local headers.

In this repository, use the normal CMake dependency graph and
`CMeta::Plugin` / `CMeta::PluginABI` targets as appropriate.

## Troubleshooting

### Plugin fails at open stage

Check that the platform DSO exists in the configured plugin/package location
and that all of its dynamic dependencies are available.

### Plugin fails at symbol/ABI stage

Ensure the DSO publishes `cmeta_plugin_query` and was rebuilt against the
current Salts Plugin ABI.

There is no fallback to an older Toad-private plugin ABI.

### Function export fails initialization

Check:

- valid FunctionDesc and FunctionAbi,
- exact adapter is non-null,
- parameter directions/carriers are supported by the current Toad
  binding,
- `export_id` does not conflict with an existing environment function.

## See also

- `engine/include/toad/plugin.h` — Toad migration adapters over Salts
  Plugin Interface publication.
- `script/include/toad/plugin_loader.h` — Toad binding adapter.
- issue #22 — Plugin ABI/CMeta convergence tracking.
- issue #84 — TurboDB::Orm database integration with explicit user driver configuration.
