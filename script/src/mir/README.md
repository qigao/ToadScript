# MIR Backend Modules

This directory contains MIR backend code that has been split out of
`../mir.c`.

Current module ownership:

- `internal.h`: shared internal MIR backend types and declarations.
- `cache.c`: JIT cache helpers.
- `compiler.c`: compiler state, register allocation, frame storage,
  pointer caches, and prologue/epilogue emission.
- `constant.c`: compile-time constant folding helpers.
- `oop.c`: OOP runtime bridges and OOP analysis helpers.
- `runtime.c`: MIR external prototypes, external symbol loading,
  and general runtime bridge functions.
- `value_runtime.c`: dynamic value evaluation bridges used when
  MIR lowering must preserve non-numeric values or execute AST interpreter paths.

`../mir.c` still owns the main AST lowering and compile/execute entry
points. Move additional code only as coherent ownership groups; avoid keeping
partially wired modules in the build.
