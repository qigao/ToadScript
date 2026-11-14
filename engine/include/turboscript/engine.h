/**
 * @file engine.h
 * @brief TurboScript engine parser and interpreter API.
 */

#ifndef TBS_ENGINE_H
#define TBS_ENGINE_H

#include "core/platform.h"
#include <turboscript/module.h>
#include <turboscript/types.h>
#include "memory/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif


// Parse the input string into an AST (simplified API, creates internal arena if needed)
TBS_C_API tbs_node_t *tbs_parse(const char *input, size_t length);

// Extended parse API allowing custom arena and error reporting
TBS_C_API tbs_node_t *tbs_parse_ext(const char *input, size_t length, mem_pool_t *arena, int *error,
                                          char *error_msg, size_t error_msg_len);

// Validate AST (undefined variables, duplicate params, simple type checks)
TBS_C_API int tbs_validate(tbs_node_t *root, tbs_env_t *env, char *error_msg, size_t msg_len);

// Clean up (frees the arena)
TBS_C_API void tbs_free(tbs_node_t *node);

// Evaluate AST
TBS_C_API tbs_value_t tbs_eval(const tbs_node_t *node, tbs_env_t *env);

// Complexity checks
TBS_C_API size_t tbs_node_count(const tbs_node_t *node);
TBS_C_API size_t tbs_node_depth(const tbs_node_t *node);

// Node allocation (internal/parser use)
TBS_C_API tbs_node_t *tbs_node_create(mem_pool_t *arena, tbs_node_type_t type);

// Environment management
TBS_C_API void tbs_env_init(tbs_env_t *env);
TBS_C_API void tbs_env_free(tbs_env_t *env);

/** Retain a captured snapshot environment (no-op for non-snapshot scopes). */
TBS_C_API void tbs_env_retain(tbs_env_t *env);

/**
 * Create a captured snapshot that copies only the named variables instead of
 * the whole environment. Closures capture their free variables this way so
 * repeated closure assignment does not chain-capture the previous closure.
 * Pass NULL/0 for names to copy everything (class closures, module imports).
 */
TBS_C_API tbs_env_t *tbs_env_snapshot_names(tbs_env_t *env,
                                                  const char *const *names,
                                                  size_t name_count);

/**
 * Collect variable names referenced by @p body (recursively, including nested
 * function bodies) excluding @p arg_params. Callers use the result to build a
 * capturing snapshot. Returns a NULL-terminated string array or NULL on OOM;
 * the array and strings are allocated in @p arena.
 */
TBS_C_API char **tbs_collect_closure_free_vars(const tbs_node_t *body,
                                                  tbs_node_t *const *arg_params,
                                                  size_t arg_count,
                                                  mem_pool_t *arena);

/** Release a captured snapshot environment; freed when the last reference drops. */
TBS_C_API void tbs_env_release(tbs_env_t *env);

/**
 * Reclaim snapshot environments on @p root closure chain that were created by
 * the calling thread and are no longer referenced by any function value or
 * method. Call after a script execution boundary (or a timer/task callback)
 * so short-lived closures do not accumulate. Thread-local ownership makes
 * concurrent sweeps from the host and the event-loop thread safe.
 */
TBS_C_API void tbs_env_sweep_closures(tbs_env_t *root);
/**
 * Store a value in an environment owner domain. Borrowed and cross-domain
 * values are copied. An owned mem_buffer payload already belonging to this
 * environment may be adopted; callers must then treat the argument as moved.
 */
TBS_C_API void tbs_env_set(tbs_env_t *env, const char *name, tbs_value_t value);
TBS_C_API void tbs_env_set_local(tbs_env_t *env, const char *name, tbs_value_t value);
/** Return a borrowed value invalidated by replacement or environment release. */
TBS_C_API tbs_value_t tbs_env_get(tbs_env_t *env, const char *name);
/** Return an independently owned value; destroy it unless ownership is moved. */
TBS_C_API tbs_value_t tbs_value_clone_to_env(tbs_value_t value,
                                                    tbs_env_t *dst_env);
TBS_C_API int tbs_env_has(tbs_env_t *env, const char *name);
TBS_C_API void tbs_env_register_func(tbs_env_t *env, const char *name, tbs_native_fn fn,
                                        void *user_data);
TBS_C_API int tbs_env_has_func(tbs_env_t *env, const char *name);
TBS_C_API void tbs_env_set_constant(tbs_env_t *env, const char *name, tbs_value_t value);
TBS_C_API void tbs_env_add_module(tbs_env_t *env, const tbs_module_t *mod);

// OOP - Class registration
TBS_C_API void tbs_env_register_class(tbs_env_t *env, const char *name, struct tbs_class_s *klass);
TBS_C_API struct tbs_class_s *tbs_env_lookup_class(tbs_env_t *env, const char *name);

TBS_C_API int tbs_env_last_line(const tbs_env_t *env);
TBS_C_API int tbs_env_last_column(const tbs_env_t *env);

// Global Registry
TBS_C_API void tbs_registry_init(void);
TBS_C_API void tbs_registry_add_module(const tbs_module_t *mod);
TBS_C_API tbs_builtin_fn tbs_registry_find(const char *name);

/**
 * @brief Full internal call dispatch (user functions + module registry).
 * Called by the evaluator for NODE_FUNCTION_CALL.
 */
TBS_C_API tbs_value_t tbs_call_internal(const char *name, size_t argc, tbs_value_t *args,
                                              tbs_env_t *env);

/**
 * Invoke a builtin with an ephemeral scratch pool and promote its result into
 * @p env. Builtins must not retain scratch pointers after returning.
 */
TBS_C_API tbs_value_t tbs_call_builtin(tbs_builtin_fn fn, size_t argc,
                                             tbs_value_t *args, tbs_env_t *env);

TBS_C_API tbs_builtin_fn tbs_find_builtin(const char *name, tbs_env_t *env);

/**
 * Resolve one builtin/module entry and copy its canonical CMeta admission
 * descriptor. The returned reflection owns no registry storage; its parameter
 * array is embedded in @p out and remains valid while @p out remains alive.
 *
 * This is intended for parse/compile admission. Hot execution should retain
 * the resolved invoke pointer/descriptor and must not re-query the registry.
 */
TBS_C_API int tbs_find_builtin_reflection(
    const char *name, tbs_env_t *env, tbs_function_reflection_t *out);

/* OOP runtime entry points used by JIT lowering. These operate on already-lowered
 * names and numeric arguments instead of evaluating arbitrary AST expressions. */
TBS_C_API tbs_value_t tbs_oop_define_class(const tbs_node_t *node, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_alias_class(const char *target_name, const char *source_name,
                                                tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_instantiate_numeric(const char *class_name, size_t argc,
                                                        const double *argv, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_instantiate_class_value(tbs_value_t class_value,
                                                            const char *class_name, size_t argc,
                                                            tbs_value_t *args,
                                                            tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_numeric(const char *object_name,
                                                        const char *method_name, size_t argc,
                                                        const double *argv, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_checked_numeric(
    const char *object_name, const char *method_name, const tbs_node_t *object_node,
    size_t argc, const double *argv, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_checked_values(
    const char *object_name, const char *method_name, const tbs_node_t *object_node,
    size_t argc, tbs_value_t *args, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_checked_value_nodes(
    const char *object_name, const char *method_name, const tbs_node_t *object_node,
    const tbs_node_t *call_node, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_member_call_checked_values(
    const char *object_name, const char *method_name, const tbs_node_t *object_node,
    size_t argc, tbs_value_t *args, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_member_call_checked_numeric(
    const char *object_name, const char *method_name, const tbs_node_t *object_node,
    size_t argc, const double *argv, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_member_call_checked_value_nodes(
    const char *object_name, const char *method_name, const tbs_node_t *object_node,
    const tbs_node_t *call_node, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_mono_numeric(const char *object_name,
                                                             const char *expected_class_name,
                                                             const char *method_name, size_t argc,
                                                             const double *argv,
                                                             tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_mono_checked_numeric(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const tbs_node_t *object_node, size_t argc, const double *argv, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_mono_checked_value_nodes(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const tbs_node_t *object_node, const tbs_node_t *call_node, tbs_env_t *env);
typedef struct tbs_oop_method_cache_s {
  struct tbs_class_s *last_class;
  tbs_func_t *method;
  uint64_t arg_signature;
  size_t argc;
  int is_static;
} tbs_oop_method_cache_t;
typedef struct tbs_oop_field_cache_s {
  void *owner;
  struct tbs_class_s *field_owner;
  uint64_t version;
  tbs_value_t *slot;
  int access_level;
  int is_static;
} tbs_oop_field_cache_t;
TBS_C_API tbs_value_t tbs_oop_call_method_cached_numeric(
    const char *object_name, const char *method_name, tbs_oop_method_cache_t *cache,
    size_t argc, const double *argv, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_method_cached_checked_numeric(
    const char *object_name, const char *method_name, tbs_oop_method_cache_t *cache,
    const tbs_node_t *object_node, size_t argc, const double *argv, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_call_bound_method(tbs_value_t bound_method, size_t argc,
                                                      tbs_value_t *args, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_call_function_value(tbs_value_t function_value, size_t argc,
                                                    tbs_value_t *args, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_get_member(const char *object_name, const char *member_name,
                                               tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_get_member_checked(
    const char *object_name, const char *member_name, const tbs_node_t *object_node,
    tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_get_member_cached_checked(
    const char *object_name, const char *member_name, tbs_oop_field_cache_t *cache,
    const tbs_node_t *object_node, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_set_member_numeric(const char *object_name,
                                                       const char *member_name, double value,
                                                       tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_set_member_checked_numeric(
    const char *object_name, const char *member_name, double value,
    const tbs_node_t *object_node, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_set_member_cached_checked_numeric(
    const char *object_name, const char *member_name, double value,
    tbs_oop_field_cache_t *cache, const tbs_node_t *object_node, tbs_env_t *env);
TBS_C_API tbs_value_t tbs_oop_instanceof_name(const char *object_name,
                                                    const char *class_name, tbs_env_t *env);

TBS_C_API tbs_node_t *tbs_node_copy(const tbs_node_t *src, mem_pool_t *dest_arena);
TBS_C_API tbs_node_t *tbs_node_create(mem_pool_t *arena, tbs_node_type_t type);
TBS_C_API void eval_destructure(tbs_node_t *target, tbs_value_t rhs, tbs_env_t *env,
                                int is_constant);
TBS_C_API void tbs_env_init_child(tbs_env_t *env, tbs_env_t *parent);

/* =========================================================================
 * Coroutine API
 * ========================================================================= */

/**
 * @brief 创建协程实例
 * @param env 父环境
 * @param generator_node 生成器函数节点（TBS_NODE_GENERATOR_FUNCTION）
 * @param args 初始化参数
 * @param arg_count 参数数量
 * @return 协程实例，或 NULL
 */
TBS_C_API tbs_coroutine_t *tbs_coroutine_create(
    tbs_env_t *env,
    tbs_node_t *generator_node,
    tbs_value_t *args,
    size_t arg_count
);

/**
 * @brief 恢复协程执行（next）
 * @param coro 协程实例
 * @return 1 如果有值，0 如果完成，-1 如果错误
 */
TBS_C_API int tbs_coroutine_next(tbs_coroutine_t *coro);

/**
 * @brief 向协程发送值（send）
 * @param coro 协程实例
 * @param value 发送的值
 * @return 1 如果有值，0 如果完成，-1 如果错误
 */
TBS_C_API int tbs_coroutine_send(tbs_coroutine_t *coro, tbs_value_t value);

/**
 * @brief 获取协程当前值
 * @param coro 协程实例
 * @return 当前 yield 的值
 */
TBS_C_API tbs_value_t tbs_coroutine_value(const tbs_coroutine_t *coro);

/**
 * @brief 检查协程是否完成
 * @param coro 协程实例
 * @return 1 如果完成，0 如果未完成
 */
TBS_C_API int tbs_coroutine_done(const tbs_coroutine_t *coro);

/**
 * @brief 增加协程引用计数
 * @param coro 协程实例
 */
TBS_C_API void tbs_coroutine_retain(tbs_coroutine_t *coro);

/**
 * @brief 减少协程引用计数，为 0 时释放
 * @param coro 协程实例
 */
TBS_C_API void tbs_coroutine_release(tbs_coroutine_t *coro);

#ifdef __cplusplus
}
#endif

#endif // TBS_ENGINE_H
