#ifndef TOAD_SCRIPT_H
#define TOAD_SCRIPT_H

#include <toad/export.h>
#include "core/platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct toad_script_ctx_s toad_script_ctx_t;
typedef struct toad_script_compiled_s toad_script_compiled_t;
typedef struct toad_value_s toad_value_t;
typedef struct toad_env_s toad_env_t;

/**
 * @brief Borrowed iterator over a Toad map value.
 *
 * The iterator and returned key pointers become invalid when the source map is
 * modified or released. Callers must not retain them across those operations.
 */
typedef struct {
  void *storage;
  size_t position;
  size_t bound;
} toad_script_value_map_iterator_t;

#define TOAD_SCRIPT_VERSION_MAJOR 3
#define TOAD_SCRIPT_VERSION_MINOR 0
#define TOAD_SCRIPT_VERSION_PATCH 10
#define TOAD_SCRIPT_VERSION_STRING "3.0.10"
#define TOAD_SCRIPT_NATIVE_ABI_VERSION 3

typedef enum {
  TOAD_SCRIPT_INIT_DEFAULT = 0,
  TOAD_SCRIPT_INIT_BARE = 1,
} toad_script_init_flags_t;

/**
 * @brief Decide whether a native plugin may be loaded into a context.
 *
 * @p plugin_name and @p user_data are borrowed. The callback runs
 * synchronously on the thread that first requests the native plugin. Returning
 * false denies the load before the dynamic library is opened. Built-in modules
 * and script-module imports do not invoke this callback.
 */
typedef bool (*toad_script_plugin_authorizer_fn)(const char *plugin_name,
                                                  void *user_data);

/** Runtime memory profiles. Each profile has bounded, fail-fast defaults. */
typedef enum {
  TOAD_SCRIPT_MEMORY_BATCH = 0,
  TOAD_SCRIPT_MEMORY_INTERACTIVE,
  TOAD_SCRIPT_MEMORY_SERVICE,
  TOAD_SCRIPT_MEMORY_STREAMING,
  TOAD_SCRIPT_MEMORY_SANDBOX,
} toad_script_memory_profile_t;

/**
 * @brief Memory ownership and quota policy for one Toad context.
 *
 * Limits are boundary quotas in bytes. External-value limits are checked before
 * copying; task and context retention limits are checked at completion/run
 * boundaries. A zero limit is invalid and the policy is copied by the context.
 */
typedef struct {
  toad_script_memory_profile_t profile;
  size_t max_context_bytes;
  size_t max_task_bytes;
  size_t max_external_value_bytes;
  size_t scratch_trim_threshold_bytes;
} toad_script_memory_policy_t;

typedef struct {
  size_t context_bytes;
  size_t task_bytes;
  size_t scratch_bytes;
  size_t peak_context_bytes;
} toad_script_memory_stats_t;

typedef enum {
  TOAD_SCRIPT_ERROR_NONE = 0,
  TOAD_SCRIPT_ERROR_ARGUMENT,
  TOAD_SCRIPT_ERROR_PARSE,
  TOAD_SCRIPT_ERROR_VALIDATE,
  TOAD_SCRIPT_ERROR_RUNTIME,
  TOAD_SCRIPT_ERROR_IO,
  TOAD_SCRIPT_ERROR_PLUGIN,
  TOAD_SCRIPT_ERROR_JIT,
  TOAD_SCRIPT_ERROR_OOM,
  TOAD_SCRIPT_ERROR_STATE,
  TOAD_SCRIPT_ERROR_CANCELLED,
} toad_script_error_code_t;

/**
 * @brief Cooperative interruption callback for script execution.
 *
 * The callback is invoked synchronously from existing interpreter/JIT safe
 * points on the context owner thread. Return non-zero to stop execution.
 */
typedef int (*toad_script_interrupt_fn)(void *user_data);

/**
 * @brief Initialize a new Turbo Script context.
 * @param flags TOAD_SCRIPT_INIT_DEFAULT for full init, TOAD_SCRIPT_INIT_BARE for plugin-based
 * init.
 */
TOAD_SCRIPT_C_API toad_script_ctx_t *toad_script_init(toad_script_init_flags_t flags);

/**
 * @brief Initialize a context with an optional native-plugin authorization policy.
 *
 * A NULL @p authorizer preserves the allow-all behavior of toad_script_init().
 * The callback and borrowed @p user_data must remain valid for the context
 * lifetime. TOAD_SCRIPT_INIT_DEFAULT requests the native `parser` plugin during
 * initialization, so the policy is consulted for `parser` before this function
 * returns. Denying that optional plugin does not make context initialization
 * fail.
 */
TOAD_SCRIPT_C_API toad_script_ctx_t *toad_script_init_with_plugin_authorizer(
    toad_script_init_flags_t flags, toad_script_plugin_authorizer_fn authorizer,
    void *user_data);

/** Fill @p policy with the bounded defaults for @p profile. */
TOAD_SCRIPT_C_API int toad_script_memory_policy_init(toad_script_memory_profile_t profile,
                                              toad_script_memory_policy_t *policy);

/**
 * @brief Replace the context memory policy.
 *
 * This operation fails while managed tasks or timers are active, after a hard
 * memory limit has been exceeded, or when the new limits are already below
 * current usage. There is no compatibility/unbounded mode.
 */
TOAD_SCRIPT_C_API int toad_script_set_memory_policy(toad_script_ctx_t *ctx,
                                             const toad_script_memory_policy_t *policy);

/** Return a point-in-time memory usage snapshot. */
TOAD_SCRIPT_C_API int toad_script_get_memory_stats(toad_script_ctx_t *ctx,
                                            toad_script_memory_stats_t *stats);

/**
 * @brief Install or clear a cooperative interrupt probe for legacy run APIs.
 *
 * The callback and borrowed user_data must remain valid until replaced,
 * cleared, or the context is freed. Passing NULL clears the probe. The hook
 * applies to toad_script_run(), toad_script_run_jit(), compiled execution,
 * and child execution environments that inherit the context safe points.
 *
 * An interrupt maps to TOAD_SCRIPT_ERROR_CANCELLED and causes the active run
 * to return failure without creating a polling thread.
 *
 * @return 0 on success, -1 for an invalid context state.
 */
TOAD_SCRIPT_C_API int toad_script_set_interrupt(toad_script_ctx_t *ctx,
                                                  toad_script_interrupt_fn interrupt,
                                                  void *user_data);

/**
 * @brief Get Turbo Script ABI/API version string.
 */
TOAD_SCRIPT_C_API const char *toad_script_version(void);

/**
 * @brief Configure the bounded timer job registry.
 *
 * This may only be changed while no timer jobs are active. Terminal job status
 * is discarded when the capacity changes.
 *
 * @return 0 on success, or -1 on invalid state, zero capacity, or allocation failure.
 */
TOAD_SCRIPT_C_API int toad_script_set_timer_capacity(toad_script_ctx_t *ctx, size_t capacity);

/** @brief Return the number of scheduled, queued, or running timer jobs. */
TOAD_SCRIPT_C_API size_t toad_script_timer_active_count(toad_script_ctx_t *ctx);

/** @brief Return the number of retained timer jobs in the failed state. */
TOAD_SCRIPT_C_API size_t toad_script_timer_failed_count(toad_script_ctx_t *ctx);

/**
 * @brief Configure the bounded managed-task registry.
 *
 * This may only be changed while no task is active. Retained terminal results
 * are discarded. Script code must call task.release(id) before a terminal slot
 * can be reused during normal operation.
 *
 * @return 0 on success, or -1 on invalid state, capacity, or allocation failure.
 */
TOAD_SCRIPT_C_API int toad_script_set_task_capacity(toad_script_ctx_t *ctx, size_t capacity);

/** @brief Return the number of scheduled, running, or waiting managed tasks. */
TOAD_SCRIPT_C_API size_t toad_script_task_active_count(toad_script_ctx_t *ctx);

/** @brief Return the number of retained managed tasks in the failed state. */
TOAD_SCRIPT_C_API size_t toad_script_task_failed_count(toad_script_ctx_t *ctx);

/**
 * @brief Free a Turbo Script context.
 */
TOAD_SCRIPT_C_API void toad_script_free(toad_script_ctx_t *ctx);

/**
 * @brief Run a script from a string using the MIR interpreter backend.
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure.
 */
TOAD_SCRIPT_C_API int toad_script_run(toad_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script using the MIR JIT backend for maximum performance.
 *
 * Toad uses one MIR lowering pipeline for both execution modes:
 * toad_script_run() executes the generated MIR with MIR's interpreter, while
 * toad_script_run_jit() lowers the same script to MIR and asks MIR to generate native code.
 *
 * Supported coverage includes numeric expressions, control flow, user functions,
 * strings/templates, maps/vectors/lists, destructuring, try/catch/throw, async/await
 * syntax helpers, parser/data bindings, matrix/statistics helpers, and the current OOP
 * model through MIR runtime helpers and OOP callsite caches.
 *
 * Unsupported syntax or runtime forms must fail with a compile/runtime error from
 * the MIR pipeline so JIT and MIR interpreter semantics stay visible and testable.
 *
 * Performance tips:
 * - Pre-bind vectors with toad_script_bind_vec() for native pointer access where possible.
 * - Keep hot loops numeric when maximum throughput matters.
 * - Compile once and execute repeatedly when a script is reused.
 *
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure (check toad_script_get_error_code()).
 */
TOAD_SCRIPT_C_API int toad_script_run_jit(toad_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script and print the result to stdout (for REPL use).
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure.
 */
TOAD_SCRIPT_C_API int toad_script_run_and_print(toad_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script from a file using the MIR interpreter backend.
 * @param ctx Context.
 * @param filename File path.
 * @return 0 on success, <0 on failure.
 */
TOAD_SCRIPT_C_API int toad_script_run_file(toad_script_ctx_t *ctx, const char *filename);

/**
 * @brief Compile a script into a reusable source-backed object for MIR interpreter execution.
 * @return Compiled object, or NULL on parse error.
 */
TOAD_SCRIPT_C_API toad_script_compiled_t *toad_script_compile(toad_script_ctx_t *ctx,
                                                        const char *script);

/**
 * @brief Execute a previously compiled script.
 */
TOAD_SCRIPT_C_API int toad_script_exec(toad_script_ctx_t *ctx, toad_script_compiled_t *compiled);

/**
 * @brief Free a compiled script object.
 */
TOAD_SCRIPT_C_API void toad_script_compiled_free(toad_script_compiled_t *compiled);

/**
 * @brief Get error message of last operation.
 */
TOAD_SCRIPT_C_API const char *toad_script_get_error(toad_script_ctx_t *ctx);

/**
 * @brief Get categorized error code of last operation.
 */
TOAD_SCRIPT_C_API toad_script_error_code_t toad_script_get_error_code(toad_script_ctx_t *ctx);

/**
 * @brief Bind a number variable into the script environment.
 */
TOAD_SCRIPT_C_API void toad_script_bind_num(toad_script_ctx_t *ctx, const char *name, double value);

/**
 * @brief Bind a string variable into the script environment.
 */
TOAD_SCRIPT_C_API void toad_script_bind_str(toad_script_ctx_t *ctx, const char *name, const char *value);

/**
 * @brief Bind a double[] vector into the script environment.
 */
TOAD_SCRIPT_C_API int toad_script_bind_vec(toad_script_ctx_t *ctx, const char *name, const double *data,
                          size_t len);

/**
 * @brief Native function signature for user-registered functions.
 *
 * @p env is the exact invocation environment and owns returned arena-backed
 * values. Native ABI v3 includes managed toad_value_t ownership metadata;
 * callbacks compiled for older layouts are not adapted.
 */
typedef toad_value_t (*toad_script_func_t)(size_t arg_count, toad_value_t *args,
                                              toad_env_t *env, void *user_data);

/**
 * @brief Register a native C function callable from script.
 */
TOAD_SCRIPT_C_API void toad_script_bind_func(toad_script_ctx_t *ctx, const char *name, toad_script_func_t fn,
                            void *user_data);

/**
 * @brief Get a number variable from the script environment.
 * Returns 0.0 if not found or not a number.
 */
TOAD_SCRIPT_C_API double toad_script_get_num(toad_script_ctx_t *ctx, const char *name);

/**
 * @brief Get a string variable from the script environment.
 * @return Pointer to null-terminated string, or NULL if not found/not a string.
 */
TOAD_SCRIPT_C_API const char *toad_script_get_str(toad_script_ctx_t *ctx, const char *name);

/**
 * @brief Get a vector variable from the script environment.
 * @return 0 on success, -1 if not found or not a vector.
 */
TOAD_SCRIPT_C_API int toad_script_get_vec(toad_script_ctx_t *ctx, const char *name, const double **data,
                         size_t *len);

/**
 * @brief Load a plugin by name from C code.
 * Equivalent to import("name") in script.
 * @return 0 on success, -1 on failure.
 */
TOAD_SCRIPT_C_API int toad_script_load_plugin(toad_script_ctx_t *ctx, const char *name);

/**
 * @brief Convert an toad_value_t to boolean (0.0 or 1.0).
 */
TOAD_SCRIPT_C_API bool toad_script_value_as_bool(toad_value_t val);

/**
 * @brief Create an empty map value owned by the Toad value runtime.
 */
TOAD_SCRIPT_C_API toad_value_t toad_script_value_map(void);

/**
 * @brief Set a map entry, copying the key and value according to Toad value semantics.
 */
TOAD_SCRIPT_C_API void toad_script_value_map_set(toad_value_t *map, const char *key,
                                          toad_value_t value);

/**
 * @brief Start iterating over a map without taking ownership of it.
 */
TOAD_SCRIPT_C_API toad_script_value_map_iterator_t
toad_script_value_map_iter_begin(const toad_value_t *map);

/**
 * @brief Read the next map entry.
 * @return 1 when an entry was produced, 0 when iteration is complete or invalid.
 */
TOAD_SCRIPT_C_API int toad_script_value_map_iter_next(toad_script_value_map_iterator_t *iterator,
                                               const char **key, toad_value_t *value);

/**
 * @brief Create a list value that borrows the supplied item storage.
 *
 * The returned value does not free @p items. The storage must remain valid for
 * as long as Toad can observe the returned list.
 */
TOAD_SCRIPT_C_API toad_value_t toad_script_value_list_borrowed(toad_value_t *items, size_t count);

/* ========================================================================
 * JIT Statistics API
 * ======================================================================== */

/**
 * @brief JIT 统计数据结构（公开版本）
 */
typedef struct {
  uint64_t compile_count;         ///< 编译次数
  uint64_t exec_count;            ///< 执行次数
  uint64_t cache_hit_count;       ///< 缓存命中次数
  uint64_t cache_miss_count;      ///< 缓存未命中次数
  uint64_t total_compile_time_us; ///< 总编译时间（微秒）
  uint64_t total_exec_time_us;    ///< 总执行时间（微秒）
  uint64_t var_sync_count;        ///< 变量同步次数
} toad_script_jit_stats_t;

/**
 * @brief 启用或禁用 JIT 统计收集
 *
 * 启用统计会带来极小的性能开销（< 1%）。
 *
 * @param ctx Context
 * @param enable 1 启用，0 禁用
 */
TOAD_SCRIPT_C_API void toad_script_enable_jit_stats(toad_script_ctx_t *ctx, int enable);

/**
 * @brief 获取 JIT 统计数据
 *
 * @param ctx Context
 * @return 统计数据指针（只读），如果统计未启用则返回 NULL
 */
TOAD_SCRIPT_C_API const toad_script_jit_stats_t *toad_script_get_jit_stats(toad_script_ctx_t *ctx);

/**
 * @brief 重置 JIT 统计计数器
 *
 * @param ctx Context
 */
TOAD_SCRIPT_C_API void toad_script_reset_jit_stats(toad_script_ctx_t *ctx);

/**
 * @brief 打印 JIT 统计信息到文件流（用于调试）
 *
 * 输出格式化的统计报告，包括：
 * - 编译和执行次数
 * - 缓存命中率
 * - 平均编译/执行时间
 * - 通用回退计数（当前应为 0）和变量同步次数
 *
 * @param ctx Context
 * @param fp 文件流（如 stdout, stderr 或文件）
 */
TOAD_SCRIPT_C_API void toad_script_print_jit_stats(toad_script_ctx_t *ctx, FILE *fp);

/**
 * Host ABI v1: opaque modules, mode-fixed instances and context-owned results.
 * All operations run synchronously on the context's owner thread. Register host
 * callbacks before compiling a module; the registry is frozen while modules
 * retain it. Destroy instances, modules and results before freeing the context.
 * Export handles belong to the instance that resolves them; zero is invalid.
 * Source, argument and callback views are borrowed only for the receiving call.
 * Result getter views remain valid until that result is reset, written or freed.
 * Initialize options with the matching initializer and keep reserved fields zero.
 * Initialize export/error info to zero with struct_size set to sizeof the struct.
 * Non-OK status never authorizes use of an output handle or value. Use the result
 * error getter for diagnostics when the operation could validate that result.
 * Interpreter and JIT use the same ABI; selecting JIT does not permit fallback.
 * See docs/superpowers/specs/2026-08-25-toad-host-module-abi-design.md.
 */
#define TOAD_SCRIPT_HOST_ABI_VERSION 1u

typedef int32_t toad_script_status_t;
typedef uint32_t toad_script_value_kind_t;
typedef uint32_t toad_script_error_phase_t;
typedef uint32_t toad_script_execution_mode_t;
typedef uint64_t toad_script_export_handle_t;

typedef struct toad_script_module_s toad_script_module_t;
typedef struct toad_script_instance_s toad_script_instance_t;
typedef struct toad_script_result_s toad_script_result_t;
typedef struct toad_script_host_result_builder_s toad_script_host_result_builder_t;

typedef struct toad_script_string_view_s {
  const char *data;
  size_t size;
} toad_script_string_view_t;

typedef struct toad_script_value_view_s toad_script_value_view_t;
typedef struct toad_script_record_entry_view_s toad_script_record_entry_view_t;

typedef struct toad_script_array_view_s {
  const toad_script_value_view_t *items;
  size_t count;
} toad_script_array_view_t;

typedef struct toad_script_record_view_s {
  const toad_script_record_entry_view_t *entries;
  size_t count;
} toad_script_record_view_t;

struct toad_script_value_view_s {
  toad_script_value_kind_t kind;
  uint32_t reserved;
  union {
    uint8_t boolean;
    int64_t integer;
    double number;
    toad_script_string_view_t string;
    toad_script_array_view_t array;
    toad_script_record_view_t record;
  } as;
};

struct toad_script_record_entry_view_s {
  toad_script_string_view_t key;
  toad_script_value_view_t value;
};

typedef struct toad_script_module_options_s {
  uint32_t struct_size;
  uint32_t reserved0;
  toad_script_string_view_t module_name;
  size_t max_source_bytes;
  size_t max_ast_nodes;
  size_t max_imports;
  size_t max_exports;
  size_t max_string_bytes;
  uint64_t reserved[4];
} toad_script_module_options_t;

typedef struct toad_script_instance_options_s {
  uint32_t struct_size;
  toad_script_execution_mode_t mode;
  size_t max_retained_bytes;
  size_t max_stack_bytes;
  uint32_t max_recursion;
  uint32_t max_globals;
  uint32_t max_value_depth;
  uint32_t reserved0;
  size_t max_value_nodes;
  size_t max_result_bytes;
  uint64_t reserved[4];
} toad_script_instance_options_t;

typedef struct toad_script_call_options_s {
  uint32_t struct_size;
  uint32_t max_recursion;
  uint32_t max_steps;
  uint32_t max_loop_iterations;
  uint32_t max_host_callbacks;
  uint32_t reserved0;
  size_t max_result_bytes;
  toad_script_interrupt_fn interrupt;
  void *interrupt_user_data;
  uint64_t reserved[4];
} toad_script_call_options_t;

typedef struct toad_script_export_info_s {
  uint32_t struct_size;
  uint32_t min_arity;
  uint32_t max_arity;
  uint32_t reserved0;
  toad_script_string_view_t name;
  uint64_t reserved[4];
} toad_script_export_info_t;

typedef struct toad_script_error_info_s {
  uint32_t struct_size;
  toad_script_status_t status;
  int32_t error_code;
  toad_script_error_phase_t phase;
  uint32_t line;
  uint32_t column;
  uint32_t length;
  int32_t cause_code;
  toad_script_string_view_t module_name;
  toad_script_string_view_t function_name;
  toad_script_string_view_t message;
  uint64_t reserved[4];
} toad_script_error_info_t;

typedef struct toad_script_host_function_descriptor_s {
  uint32_t struct_size;
  uint32_t min_arity;
  uint32_t max_arity;
  uint32_t reserved0;
  toad_script_string_view_t name;
  uint64_t reserved[4];
} toad_script_host_function_descriptor_t;

typedef toad_script_status_t (*toad_script_host_function_t)(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder);

#define TOAD_SCRIPT_STATUS_OK ((toad_script_status_t)0)
#define TOAD_SCRIPT_STATUS_INVALID_ARGUMENT ((toad_script_status_t)1)
#define TOAD_SCRIPT_STATUS_OUT_OF_MEMORY ((toad_script_status_t)2)
#define TOAD_SCRIPT_STATUS_WRONG_THREAD ((toad_script_status_t)3)
#define TOAD_SCRIPT_STATUS_INVALID_STATE ((toad_script_status_t)4)
#define TOAD_SCRIPT_STATUS_CONTEXT_MISMATCH ((toad_script_status_t)5)
#define TOAD_SCRIPT_STATUS_PARSE_ERROR ((toad_script_status_t)6)
#define TOAD_SCRIPT_STATUS_VALIDATION_ERROR ((toad_script_status_t)7)
#define TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED ((toad_script_status_t)8)
#define TOAD_SCRIPT_STATUS_INVALID_UTF8 ((toad_script_status_t)9)
#define TOAD_SCRIPT_STATUS_DUPLICATE_RECORD_KEY ((toad_script_status_t)10)
#define TOAD_SCRIPT_STATUS_NOT_FOUND ((toad_script_status_t)11)
#define TOAD_SCRIPT_STATUS_INVALID_EXPORT_HANDLE ((toad_script_status_t)12)
#define TOAD_SCRIPT_STATUS_UNSUPPORTED_BACKEND_SEMANTIC ((toad_script_status_t)13)
#define TOAD_SCRIPT_STATUS_RUNTIME_ERROR ((toad_script_status_t)14)
#define TOAD_SCRIPT_STATUS_HOST_ERROR ((toad_script_status_t)15)
#define TOAD_SCRIPT_STATUS_REENTRANT_CALL ((toad_script_status_t)16)
#define TOAD_SCRIPT_STATUS_INTERRUPTED ((toad_script_status_t)17)

#define TOAD_SCRIPT_VALUE_NULL ((toad_script_value_kind_t)0)
#define TOAD_SCRIPT_VALUE_BOOL ((toad_script_value_kind_t)1)
#define TOAD_SCRIPT_VALUE_INT64 ((toad_script_value_kind_t)2)
#define TOAD_SCRIPT_VALUE_NUMBER ((toad_script_value_kind_t)3)
#define TOAD_SCRIPT_VALUE_STRING ((toad_script_value_kind_t)4)
#define TOAD_SCRIPT_VALUE_ARRAY ((toad_script_value_kind_t)5)
#define TOAD_SCRIPT_VALUE_RECORD ((toad_script_value_kind_t)6)

#define TOAD_SCRIPT_ERROR_PHASE_NONE ((toad_script_error_phase_t)0)
#define TOAD_SCRIPT_ERROR_PHASE_COMPILE ((toad_script_error_phase_t)1)
#define TOAD_SCRIPT_ERROR_PHASE_INSTANTIATE ((toad_script_error_phase_t)2)
#define TOAD_SCRIPT_ERROR_PHASE_RESOLVE ((toad_script_error_phase_t)3)
#define TOAD_SCRIPT_ERROR_PHASE_CALL ((toad_script_error_phase_t)4)
#define TOAD_SCRIPT_ERROR_PHASE_HOST_CALLBACK ((toad_script_error_phase_t)5)
#define TOAD_SCRIPT_ERROR_PHASE_INTERRUPT ((toad_script_error_phase_t)6)

#define TOAD_SCRIPT_EXEC_INTERPRETER ((toad_script_execution_mode_t)1)
#define TOAD_SCRIPT_EXEC_JIT ((toad_script_execution_mode_t)2)

TOAD_SCRIPT_C_API void toad_script_module_options_init(toad_script_module_options_t *options);
TOAD_SCRIPT_C_API void toad_script_instance_options_init(toad_script_instance_options_t *options);
TOAD_SCRIPT_C_API void toad_script_call_options_init(toad_script_call_options_t *options);

TOAD_SCRIPT_C_API toad_script_status_t
toad_script_result_create(toad_script_ctx_t *ctx, toad_script_result_t **out_result);
TOAD_SCRIPT_C_API void toad_script_result_reset(toad_script_result_t *result);
TOAD_SCRIPT_C_API void toad_script_result_destroy(toad_script_result_t *result);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_result_get_value(
    const toad_script_result_t *result, toad_script_value_view_t *out_value);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_result_get_error(
    const toad_script_result_t *result, toad_script_error_info_t *out_error);

TOAD_SCRIPT_C_API toad_script_status_t
toad_script_module_compile(toad_script_ctx_t *ctx, toad_script_string_view_t source,
                            const toad_script_module_options_t *options,
                            toad_script_result_t *result, toad_script_module_t **out_module);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_module_destroy(toad_script_module_t *module,
                                                  toad_script_result_t *result);

TOAD_SCRIPT_C_API toad_script_status_t toad_script_instance_create(
    toad_script_module_t *module, const toad_script_instance_options_t *options,
    toad_script_result_t *result, toad_script_instance_t **out_instance);
TOAD_SCRIPT_C_API toad_script_status_t
toad_script_instance_destroy(toad_script_instance_t *instance, toad_script_result_t *result);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_instance_resolve_export(
    toad_script_instance_t *instance, toad_script_string_view_t name,
    toad_script_result_t *result, toad_script_export_handle_t *out_handle);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_instance_get_export_info(
    toad_script_instance_t *instance, toad_script_export_handle_t handle,
    toad_script_result_t *result, toad_script_export_info_t *out_info);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_instance_call(
    toad_script_instance_t *instance, toad_script_export_handle_t handle,
    const toad_script_value_view_t *args, size_t arg_count,
    const toad_script_call_options_t *options, toad_script_result_t *result);

TOAD_SCRIPT_C_API toad_script_status_t toad_script_context_register_host_function(
    toad_script_ctx_t *ctx, const toad_script_host_function_descriptor_t *descriptor,
    toad_script_host_function_t callback, void *user_data, toad_script_result_t *result);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_context_unregister_host_function(
    toad_script_ctx_t *ctx, toad_script_string_view_t name, toad_script_result_t *result);
TOAD_SCRIPT_C_API toad_script_status_t toad_script_host_result_set_value(
    toad_script_host_result_builder_t *builder, const toad_script_value_view_t *value);
TOAD_SCRIPT_C_API toad_script_status_t
toad_script_host_result_set_error(toad_script_host_result_builder_t *builder, int32_t cause_code,
                                   toad_script_string_view_t message);

#ifdef __cplusplus
}
#endif

#endif // TOAD_SCRIPT_H
