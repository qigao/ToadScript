/**
 * @file internal.h
 * @brief Internal Toad definitions
 */

#ifndef TOAD_SCRIPT_INTERNAL_H
#define TOAD_SCRIPT_INTERNAL_H

#include <toad/module.h>
#include <toad/plugin_loader.h>
#include "memory/buffer.h"
#include "salts/coro_executor.h"
#include <toad/script.h>
#include <mir.h>
#include <cstl.h>
#include <stdatomic.h>

typedef struct ts_timer_scheduler_s ts_timer_scheduler_t;
typedef struct ts_task_scheduler_s ts_task_scheduler_t;
struct ts_mir_owned_string_block_s;
#ifndef TS_HOST_CALL_BUDGET_T_DEFINED
#define TS_HOST_CALL_BUDGET_T_DEFINED
typedef struct ts_host_call_budget_s ts_host_call_budget_t;
#endif

typedef struct imported_module_s {
  char *name;
  toad_node_t *expr;
  toad_value_t exports;
  int has_exports;
  int isolated;
  struct imported_module_s *next;
} imported_module_t;

#define TS_MAX_PLUGINS 16
#define TS_JIT_CACHE_SIZE 128 // 从 64 扩展到 128，提升缓存命中率

/**
 * @brief JIT 统计数据结构
 */
typedef struct {
  uint64_t compile_count;         // 编译次数
  uint64_t exec_count;            // 执行次数
  uint64_t cache_hit_count;       // 缓存命中次数
  uint64_t cache_miss_count;      // 缓存未命中次数
  uint64_t total_compile_time_us; // 总编译时间（微秒）
  uint64_t total_exec_time_us;    // 总执行时间（微秒）
  uint64_t var_sync_count;        // 变量同步次数
} ts_jit_stats_t;

/* Verify that the internal ts_jit_stats_t and the public toad_script_jit_stats_t
 * have identical layout.  Both structs are identical by definition; this assert
 * catches any accidental divergence before the pointer cast in
 * toad_script_get_jit_stats() can produce undefined behaviour. */
#ifdef __cplusplus
static_assert(sizeof(ts_jit_stats_t) == sizeof(toad_script_jit_stats_t),
              "ts_jit_stats_t and toad_script_jit_stats_t must have the same size");
#else
_Static_assert(sizeof(ts_jit_stats_t) == sizeof(toad_script_jit_stats_t),
               "ts_jit_stats_t and toad_script_jit_stats_t must have the same size");
#endif

struct toad_script_ctx_s {
  atomic_uint ref_count;
  atomic_int closing;
  /* Immutable TLS address captured by the creating thread. Host ABI entry
   * points compare against this single context-owner fact source. */
  const void *owner_thread_token;
  vec_t host_functions;
  size_t active_host_modules;
  size_t host_callback_depth;
  /* Borrowed only for the synchronous owner-thread call. It is the context-wide
   * reentrancy fact source used by Host callback adapters. */
  struct toad_script_instance_s *active_host_instance;
  uint64_t next_instance_generation;
  /* Instance initialization resolves immutable export slots once. Steady
   * calls index this table and never repeat a function-name lookup. */
  toad_func_t **host_export_functions;
  size_t host_export_function_count;
  /* Borrowed only while one synchronous Host export call is executing. */
  ts_host_call_budget_t *active_host_budget;
  /* Optional context-wide cooperative interrupt used by legacy run APIs. */
  toad_script_interrupt_fn interrupt;
  void *interrupt_user_data;
  toad_env_t env;
  toad_node_t *expr;
  char *expr_source;
  /* 1 when ctx->expr is already tracked in compiled_asts[] (owned by that array).
   * In that case toad_script_free and ts_prepare_expr must NOT call toad_free(ctx->expr)
   * directly — compiled_asts[] cleanup handles it. */
  int expr_in_compiled_asts;
  imported_module_t *imports;
  mem_pool_t scratch_arena;
  char *current_script_dir;
  const char *current_import_name;
  toad_value_t current_import_exports;
  int current_import_has_exports;
  toad_env_t *current_import_env;
  char error_msg[1024];
  toad_script_error_code_t error_code;
  toad_script_memory_policy_t memory_policy;
  size_t peak_context_bytes;
  int memory_exhausted;

  /* The context owns one serialized Executor shard for all script callbacks. */
  coro_executor_t *executor;
  ts_timer_scheduler_t *timer_scheduler;
  ts_task_scheduler_t *task_scheduler;

  /* JIT compiling tracking to prevent memory leak and data race */
  int mir_mod_idx;
  toad_node_t **compiled_asts;
  size_t compiled_ast_count;
  size_t compiled_ast_capacity;

  /* Plugin handles */
  toad_script_plugin_authorizer_fn plugin_authorizer;
  void *plugin_authorizer_data;
  toad_plugin_handle_t *plugins[TS_MAX_PLUGINS];
  char *loaded_names[TS_MAX_PLUGINS];
  size_t plugin_count;
  struct ts_host_plugin_registration_s *host_plugins[TS_MAX_PLUGINS];
  size_t host_plugin_count;

  /* MIR JIT compiler context */
  MIR_context_t mir_ctx;
  void *mir_last_fn;       /* Phase 15: cached JIT function pointer */
  int mir_gen_initialized; /* Phase 15: gen_init called once */
  MIR_context_t mir_interp_ctx;
  MIR_item_t mir_interp_last_func;
  int mir_interp_externals_loaded;
  /* Owns string addresses embedded in ordinary MIR/JIT modules. Blocks are
   * released only after both context-owned MIR engines are finished. */
  struct ts_mir_owned_string_block_s *mir_owned_string_blocks;

  /* Isolated context for script-level mir.load/mir.call to prevent interface clash and duplicate
   * linking */
  MIR_context_t script_mir_ctx;
  int script_mir_linked;

  struct {
    uint64_t hash;
    void *fn_ptr;
    uint32_t access_count; // LRU 访问计数
    char *script;          // Copy of script string to prevent hash collisions
  } jit_cache[TS_JIT_CACHE_SIZE];

  /* JIT 统计信息 */
  ts_jit_stats_t jit_stats;
  int jit_stats_enabled; // 是否启用统计
};

struct toad_script_compiled_s {
  char *source;
};

toad_node_t *toad_script_parse_with_error(toad_script_ctx_t *ctx, const char *script);
/* Internal deterministic seam: fails the parser arena allocation. */
toad_node_t *toad_script_parse_with_error_test_oom(toad_script_ctx_t *ctx, const char *script);

/* Built-in module accessors */
void toad_script_register_modules(void);
void toad_script_register_mir(struct toad_script_ctx_s *ctx);
toad_value_t toad_script_mir_eval_node(const toad_node_t *node, toad_env_t *env);
int toad_script_mir_exec_script_body(toad_func_t *func, toad_env_t *local_env,
                                      toad_env_t *caller_env, toad_value_t *out);

void ts_mir_owned_string_blocks_destroy(struct ts_mir_owned_string_block_s *block);

/* Internal JIT API - for testing and advanced use */
TOAD_SCRIPT_C_API int toad_script_compile_mir(toad_script_ctx_t *ctx, const char *script);
TOAD_SCRIPT_C_API int toad_script_compile_mir_ast(toad_script_ctx_t *ctx, toad_node_t *ast,
                                                    const char *script);
TOAD_SCRIPT_C_API int toad_script_exec_jit(toad_script_ctx_t *ctx);
TOAD_SCRIPT_C_API int toad_script_compile_mir_interp(toad_script_ctx_t *ctx, const char *script);
TOAD_SCRIPT_C_API int toad_script_compile_mir_interp_ast(toad_script_ctx_t *ctx,
                                                           toad_node_t *ast, const char *script);
TOAD_SCRIPT_C_API int toad_script_exec_mir_interp(toad_script_ctx_t *ctx);
TOAD_SCRIPT_C_API int toad_script_exec_mir_interp_result(toad_script_ctx_t *ctx,
                                                           double *result_out);
TOAD_SCRIPT_C_API int toad_script_run_mir_interp(toad_script_ctx_t *ctx, const char *script);

/* Internal REPL helper */
int toad_run(toad_script_ctx_t *ctx, const char *script);

/* Context references protect accepted executor tasks during deferred shutdown. */
void ts_context_retain(toad_script_ctx_t *ctx);
void ts_context_release(toad_script_ctx_t *ctx);
int ts_context_is_owner_thread(const toad_script_ctx_t *ctx);

/* Returns the caller environment owned by the currently running managed task,
 * or the root environment when execution is not inside one. */
toad_env_t *ts_task_execution_env(toad_script_ctx_t *ctx);

/* Point-in-time bytes retained by managed task environments. */
size_t ts_task_memory_used(toad_script_ctx_t *ctx);
size_t ts_task_memory_max_used(toad_script_ctx_t *ctx);

/* Apply scratch reclamation and hard context quota checks after execution. */
int ts_memory_finish_run(toad_script_ctx_t *ctx, int result);

#endif /* TOAD_SCRIPT_INTERNAL_H */
