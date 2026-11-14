/**
 * @file engine.h
 * @brief Toad engine parser and interpreter API.
 */

#ifndef TOAD_ENGINE_H
#define TOAD_ENGINE_H

#include "core/platform.h"
#include <toad/module.h>
#include <toad/types.h>
#include "memory/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif


// Parse the input string into an AST (simplified API, creates internal arena if needed)
TOAD_C_API toad_node_t *toad_parse(const char *input, size_t length);

// Extended parse API allowing custom arena and error reporting
TOAD_C_API toad_node_t *toad_parse_ext(const char *input, size_t length, mem_pool_t *arena, int *error,
                                          char *error_msg, size_t error_msg_len);

// Validate AST (undefined variables, duplicate params, simple type checks)
TOAD_C_API int toad_validate(toad_node_t *root, toad_env_t *env, char *error_msg, size_t msg_len);

// Clean up (frees the arena)
TOAD_C_API void toad_free(toad_node_t *node);

// Evaluate AST
TOAD_C_API toad_value_t toad_eval(const toad_node_t *node, toad_env_t *env);

// Complexity checks
TOAD_C_API size_t toad_node_count(const toad_node_t *node);
TOAD_C_API size_t toad_node_depth(const toad_node_t *node);

// Node allocation (internal/parser use)
TOAD_C_API toad_node_t *toad_node_create(mem_pool_t *arena, toad_node_type_t type);

// Environment management
TOAD_C_API void toad_env_init(toad_env_t *env);
TOAD_C_API void toad_env_free(toad_env_t *env);

/** Retain a captured snapshot environment (no-op for non-snapshot scopes). */
TOAD_C_API void toad_env_retain(toad_env_t *env);

/**
 * Create a captured snapshot that copies only the named variables instead of
 * the whole environment. Closures capture their free variables this way so
 * repeated closure assignment does not chain-capture the previous closure.
 * Pass NULL/0 for names to copy everything (class closures, module imports).
 */
TOAD_C_API toad_env_t *toad_env_snapshot_names(toad_env_t *env,
                                                  const char *const *names,
                                                  size_t name_count);

/**
 * Collect variable names referenced by @p body (recursively, including nested
 * function bodies) excluding @p arg_params. Callers use the result to build a
 * capturing snapshot. Returns a NULL-terminated string array or NULL on OOM;
 * the array and strings are allocated in @p arena.
 */
TOAD_C_API char **toad_collect_closure_free_vars(const toad_node_t *body,
                                                  toad_node_t *const *arg_params,
                                                  size_t arg_count,
                                                  mem_pool_t *arena);

/** Release a captured snapshot environment; freed when the last reference drops. */
TOAD_C_API void toad_env_release(toad_env_t *env);

/**
 * Reclaim snapshot environments on @p root closure chain that were created by
 * the calling thread and are no longer referenced by any function value or
 * method. Call after a script execution boundary (or a timer/task callback)
 * so short-lived closures do not accumulate. Thread-local ownership makes
 * concurrent sweeps from the host and the event-loop thread safe.
 */
TOAD_C_API void toad_env_sweep_closures(toad_env_t *root);
/**
 * Store a value in an environment owner domain. Borrowed and cross-domain
 * values are copied. An owned mem_buffer payload already belonging to this
 * environment may be adopted; callers must then treat the argument as moved.
 */
TOAD_C_API void toad_env_set(toad_env_t *env, const char *name, toad_value_t value);
TOAD_C_API void toad_env_set_local(toad_env_t *env, const char *name, toad_value_t value);
/** Return a borrowed value invalidated by replacement or environment release. */
TOAD_C_API toad_value_t toad_env_get(toad_env_t *env, const char *name);
/** Return an independently owned value; destroy it unless ownership is moved. */
TOAD_C_API toad_value_t toad_value_clone_to_env(toad_value_t value,
                                                    toad_env_t *dst_env);
TOAD_C_API int toad_env_has(toad_env_t *env, const char *name);
TOAD_C_API void toad_env_register_func(toad_env_t *env, const char *name, toad_native_fn fn,
                                        void *user_data);
TOAD_C_API int toad_env_has_func(toad_env_t *env, const char *name);
TOAD_C_API void toad_env_set_constant(toad_env_t *env, const char *name, toad_value_t value);
TOAD_C_API void toad_env_add_module(toad_env_t *env, const toad_module_t *mod);

// OOP - Class registration
TOAD_C_API void toad_env_register_class(toad_env_t *env, const char *name, struct toad_class_s *klass);
TOAD_C_API struct toad_class_s *toad_env_lookup_class(toad_env_t *env, const char *name);

TOAD_C_API int toad_env_last_line(const toad_env_t *env);
TOAD_C_API int toad_env_last_column(const toad_env_t *env);

// Global Registry
TOAD_C_API void toad_registry_init(void);
TOAD_C_API void toad_registry_add_module(const toad_module_t *mod);
TOAD_C_API toad_builtin_fn toad_registry_find(const char *name);

/**
 * @brief Full internal call dispatch (user functions + module registry).
 * Called by the evaluator for NODE_FUNCTION_CALL.
 */
TOAD_C_API toad_value_t toad_call_internal(const char *name, size_t argc, toad_value_t *args,
                                              toad_env_t *env);

/**
 * Invoke a builtin with an ephemeral scratch pool and promote its result into
 * @p env. Builtins must not retain scratch pointers after returning.
 */
TOAD_C_API toad_value_t toad_call_builtin(toad_builtin_fn fn, size_t argc,
                                             toad_value_t *args, toad_env_t *env);

TOAD_C_API toad_builtin_fn toad_find_builtin(const char *name, toad_env_t *env);

/**
 * Resolve one builtin/module entry and copy its canonical CMeta admission
 * descriptor. The returned reflection owns no registry storage; its parameter
 * array is embedded in @p out and remains valid while @p out remains alive.
 *
 * This is intended for parse/compile admission. Hot execution should retain
 * the resolved invoke pointer/descriptor and must not re-query the registry.
 */
TOAD_C_API int toad_find_builtin_reflection(
    const char *name, toad_env_t *env, toad_function_reflection_t *out);

/* OOP runtime entry points used by JIT lowering. These operate on already-lowered
 * names and numeric arguments instead of evaluating arbitrary AST expressions. */
TOAD_C_API toad_value_t toad_oop_define_class(const toad_node_t *node, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_alias_class(const char *target_name, const char *source_name,
                                                toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_instantiate_numeric(const char *class_name, size_t argc,
                                                        const double *argv, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_instantiate_class_value(toad_value_t class_value,
                                                            const char *class_name, size_t argc,
                                                            toad_value_t *args,
                                                            toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_numeric(const char *object_name,
                                                        const char *method_name, size_t argc,
                                                        const double *argv, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_checked_numeric(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, const double *argv, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_checked_values(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, toad_value_t *args, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_checked_value_nodes(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    const toad_node_t *call_node, toad_env_t *env);
TOAD_C_API toad_value_t toad_member_call_checked_values(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, toad_value_t *args, toad_env_t *env);
TOAD_C_API toad_value_t toad_member_call_checked_numeric(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, const double *argv, toad_env_t *env);
TOAD_C_API toad_value_t toad_member_call_checked_value_nodes(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    const toad_node_t *call_node, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_mono_numeric(const char *object_name,
                                                             const char *expected_class_name,
                                                             const char *method_name, size_t argc,
                                                             const double *argv,
                                                             toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_mono_checked_numeric(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const toad_node_t *object_node, size_t argc, const double *argv, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_mono_checked_value_nodes(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const toad_node_t *object_node, const toad_node_t *call_node, toad_env_t *env);
typedef struct toad_oop_method_cache_s {
  struct toad_class_s *last_class;
  toad_func_t *method;
  uint64_t arg_signature;
  size_t argc;
  int is_static;
} toad_oop_method_cache_t;
typedef struct toad_oop_field_cache_s {
  void *owner;
  struct toad_class_s *field_owner;
  uint64_t version;
  toad_value_t *slot;
  int access_level;
  int is_static;
} toad_oop_field_cache_t;
TOAD_C_API toad_value_t toad_oop_call_method_cached_numeric(
    const char *object_name, const char *method_name, toad_oop_method_cache_t *cache,
    size_t argc, const double *argv, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_method_cached_checked_numeric(
    const char *object_name, const char *method_name, toad_oop_method_cache_t *cache,
    const toad_node_t *object_node, size_t argc, const double *argv, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_call_bound_method(toad_value_t bound_method, size_t argc,
                                                      toad_value_t *args, toad_env_t *env);
TOAD_C_API toad_value_t toad_call_function_value(toad_value_t function_value, size_t argc,
                                                    toad_value_t *args, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_get_member(const char *object_name, const char *member_name,
                                               toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_get_member_checked(
    const char *object_name, const char *member_name, const toad_node_t *object_node,
    toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_get_member_cached_checked(
    const char *object_name, const char *member_name, toad_oop_field_cache_t *cache,
    const toad_node_t *object_node, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_set_member_numeric(const char *object_name,
                                                       const char *member_name, double value,
                                                       toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_set_member_checked_numeric(
    const char *object_name, const char *member_name, double value,
    const toad_node_t *object_node, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_set_member_cached_checked_numeric(
    const char *object_name, const char *member_name, double value,
    toad_oop_field_cache_t *cache, const toad_node_t *object_node, toad_env_t *env);
TOAD_C_API toad_value_t toad_oop_instanceof_name(const char *object_name,
                                                    const char *class_name, toad_env_t *env);

TOAD_C_API toad_node_t *toad_node_copy(const toad_node_t *src, mem_pool_t *dest_arena);
TOAD_C_API toad_node_t *toad_node_create(mem_pool_t *arena, toad_node_type_t type);
TOAD_C_API void eval_destructure(toad_node_t *target, toad_value_t rhs, toad_env_t *env,
                                int is_constant);
TOAD_C_API void toad_env_init_child(toad_env_t *env, toad_env_t *parent);

/* =========================================================================
 * Coroutine API
 * ========================================================================= */

/**
 * @brief 创建协程实例
 * @param env 父环境
 * @param generator_node 生成器函数节点（TOAD_NODE_GENERATOR_FUNCTION）
 * @param args 初始化参数
 * @param arg_count 参数数量
 * @return 协程实例，或 NULL
 */
TOAD_C_API toad_coroutine_t *toad_coroutine_create(
    toad_env_t *env,
    toad_node_t *generator_node,
    toad_value_t *args,
    size_t arg_count
);

/**
 * @brief 恢复协程执行（next）
 * @param coro 协程实例
 * @return 1 如果有值，0 如果完成，-1 如果错误
 */
TOAD_C_API int toad_coroutine_next(toad_coroutine_t *coro);

/**
 * @brief 向协程发送值（send）
 * @param coro 协程实例
 * @param value 发送的值
 * @return 1 如果有值，0 如果完成，-1 如果错误
 */
TOAD_C_API int toad_coroutine_send(toad_coroutine_t *coro, toad_value_t value);

/**
 * @brief 获取协程当前值
 * @param coro 协程实例
 * @return 当前 yield 的值
 */
TOAD_C_API toad_value_t toad_coroutine_value(const toad_coroutine_t *coro);

/**
 * @brief 检查协程是否完成
 * @param coro 协程实例
 * @return 1 如果完成，0 如果未完成
 */
TOAD_C_API int toad_coroutine_done(const toad_coroutine_t *coro);

/**
 * @brief 增加协程引用计数
 * @param coro 协程实例
 */
TOAD_C_API void toad_coroutine_retain(toad_coroutine_t *coro);

/**
 * @brief 减少协程引用计数，为 0 时释放
 * @param coro 协程实例
 */
TOAD_C_API void toad_coroutine_release(toad_coroutine_t *coro);

#ifdef __cplusplus
}
#endif

#endif // TOAD_ENGINE_H
