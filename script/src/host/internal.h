#ifndef TOAD_SCRIPT_HOST_INTERNAL_H
#define TOAD_SCRIPT_HOST_INTERNAL_H

#include <toad/module.h>
#include "memory/buffer.h"
#include "api_internal.h"
#include "core/tstr.h"
#include <cstl.h>

typedef struct ts_mir_artifact_s ts_mir_artifact_t;
#ifndef TS_HOST_CALL_BUDGET_T_DEFINED
#define TS_HOST_CALL_BUDGET_T_DEFINED
typedef struct ts_host_call_budget_s ts_host_call_budget_t;
#endif

typedef struct ts_host_export_entry_s {
  tstr name;
  uint32_t arity;
  uint32_t line;
  uint32_t column;
  toad_node_t *declaration_node;
  toad_node_t *function_node;
} ts_host_export_entry_t;

typedef struct ts_host_export_table_s {
  vec_t entries;
} ts_host_export_table_t;

struct toad_script_module_s {
  toad_script_ctx_t *ctx;
  tstr source;
  tstr module_name;
  toad_node_t *ast;
  ts_host_export_table_t exports;
  ts_mir_artifact_t *artifact;
  size_t parse_count;
  size_t lower_count;
  size_t ref_count;
  int accepting_instances;
};

typedef enum ts_host_instance_state_e {
  TS_HOST_INSTANCE_READY = 1,
  TS_HOST_INSTANCE_CALLING = 2,
  TS_HOST_INSTANCE_CALLING_HOST = 3,
  TS_HOST_INSTANCE_FAULTED = 4,
  TS_HOST_INSTANCE_DESTROYED = 5,
} ts_host_instance_state_t;

typedef struct ts_host_runtime_diagnostic_s {
  toad_script_status_t status;
  int32_t error_code;
  toad_script_error_phase_t phase;
  uint32_t line;
  uint32_t column;
  uint32_t length;
  int32_t cause_code;
  toad_script_string_view_t function_name;
  tstr message;
  int present;
} ts_host_runtime_diagnostic_t;

struct ts_host_call_budget_s {
  uint32_t steps_left;
  uint32_t loops_left;
  uint32_t callbacks_left;
  uint32_t recursion_left;
  uint32_t recursion_limit;
  uint32_t interrupt_step_accumulator;
  size_t stack_bytes_left;
  size_t stack_bytes_limit;
  size_t result_bytes_left;
  size_t retained_bytes_limit;
  toad_script_interrupt_fn interrupt;
  void *interrupt_user_data;
  toad_script_status_t failure_status;
  toad_script_error_phase_t failure_phase;
  const char *failure_message;
};

struct toad_script_instance_s {
  toad_script_module_t *module;
  toad_script_ctx_t *runtime_ctx;
  toad_script_instance_options_t limits;
  uint32_t generation;
  toad_script_execution_mode_t mode;
  ts_host_instance_state_t state;
  ts_host_runtime_diagnostic_t diagnostic;
  size_t initializer_count;
};

typedef struct ts_host_function_entry_s {
  toad_script_ctx_t *ctx;
  tstr name;
  uint32_t min_arity;
  uint32_t max_arity;
  toad_script_host_function_t callback;
  void *user_data;
} ts_host_function_entry_t;

typedef enum ts_host_builder_state_e {
  TS_HOST_BUILDER_EMPTY = 0,
  TS_HOST_BUILDER_VALUE = 1,
  TS_HOST_BUILDER_ERROR = 2,
} ts_host_builder_state_t;

struct toad_script_host_result_builder_s {
  toad_script_ctx_t *ctx;
  ts_host_builder_state_t state;
  toad_script_status_t terminal_status;
  toad_value_t value;
  int32_t cause_code;
  tstr error_message;
};

typedef struct ts_host_value_limits_s {
  uint32_t max_depth;
  size_t max_nodes;
  size_t max_bytes;
} ts_host_value_limits_t;

/* Single-owner result. Getter views are invalidated by reset, the next write,
 * or destroy; every operation must run on the owning context's thread. */
struct toad_script_result_s {
  toad_script_ctx_t *ctx;
  mem_pool_t view_arena;
  toad_script_value_view_t value;
  toad_script_error_info_t error;
  int has_value;
  int has_error;
};

/* Translate the context's immutable owner fact into the host ABI status. */
toad_script_status_t ts_host_context_check_thread(const toad_script_ctx_t *ctx);

/* Host registry control plane. The context owns every entry and its tstr name.
 * Module acquisition freezes mutation until the matching final release. */
toad_script_status_t ts_host_registry_init(toad_script_ctx_t *ctx);
void ts_host_registry_destroy(toad_script_ctx_t *ctx);
toad_script_status_t ts_host_check_owner_thread(const toad_script_ctx_t *ctx);
toad_script_status_t ts_host_registry_acquire_module(toad_script_ctx_t *ctx);
toad_script_status_t ts_host_registry_release_module(toad_script_ctx_t *ctx);
toad_script_status_t ts_host_registry_find_slot(const toad_script_ctx_t *ctx,
                                                 toad_script_string_view_t name, size_t *out_slot);
toad_script_status_t ts_host_registry_get_slot(const toad_script_ctx_t *ctx, size_t slot,
                                                const ts_host_function_entry_t **out_entry);
toad_script_status_t ts_host_registry_bind_runtime(toad_script_ctx_t *ctx,
                                                    toad_env_t *runtime_ctx);
double ts_host_registry_invoke_numeric_slot(toad_script_ctx_t *registry_owner_ctx,
                                            toad_script_ctx_t *runtime_ctx, size_t slot,
                                            size_t arg_count, const double *numeric_args);
/* Internal deterministic fault seam; production callers use bind_runtime. */
toad_script_status_t ts_host_registry_bind_runtime_test_fault(toad_script_ctx_t *ctx,
                                                               toad_env_t *runtime_ctx,
                                                               size_t allocation_index);

/* The call boundary is the sole owner of call-state and callback-depth
 * transitions. Registry callbacks only enter/leave through these helpers. */
toad_script_status_t ts_host_instance_begin_call(toad_script_instance_t *instance);
toad_script_status_t ts_host_instance_enter_callback(toad_script_ctx_t *ctx,
                                                      toad_script_instance_t **out_instance);
void ts_host_instance_leave_callback(toad_script_ctx_t *ctx, toad_script_instance_t *instance);
toad_script_status_t ts_host_instance_finish_call(toad_script_instance_t *instance,
                                                   toad_script_status_t status);
void ts_host_instance_record_callback_error(toad_script_instance_t *instance,
                                            toad_script_status_t status, int32_t cause_code,
                                            toad_script_string_view_t function_name,
                                            toad_script_string_view_t message, uint32_t line,
                                            uint32_t column);
toad_script_status_t ts_host_call_budget_init(ts_host_call_budget_t *budget,
                                               const toad_script_instance_t *instance,
                                               const toad_script_call_options_t *options);
toad_script_status_t ts_host_call_budget_attach(toad_script_ctx_t *runtime_ctx,
                                                 ts_host_call_budget_t *budget);
void ts_host_call_budget_detach(toad_script_ctx_t *runtime_ctx, ts_host_call_budget_t *budget);
int ts_host_safe_point(toad_script_ctx_t *runtime_ctx, toad_safe_point_kind_t kind, size_t cost);
toad_script_status_t ts_host_call_budget_status(const ts_host_call_budget_t *budget);
toad_script_error_phase_t ts_host_call_budget_phase(const ts_host_call_budget_t *budget);
const char *ts_host_call_budget_message(const ts_host_call_budget_t *budget);

/* Checked boundaries underlying the locked void APIs. They never mutate on
 * failure; the public wrappers turn WRONG_THREAD into a contract violation. */
toad_script_status_t ts_host_result_reset_checked(toad_script_result_t *result);
toad_script_status_t ts_host_result_destroy_checked(toad_script_result_t *result);

/* Validate a borrowed tree without retaining any input pointer. Record keys
 * are non-empty, valid UTF-8, and U+0000-free; string values remain length-based. */
toad_script_status_t ts_host_validate_value_view(const toad_script_value_view_t *value,
                                                  const ts_host_value_limits_t *limits);
toad_script_status_t ts_host_validate_value_views(const toad_script_value_view_t *values,
                                                   size_t count,
                                                   const ts_host_value_limits_t *limits);

/* Convert into target_ctx's engine ownership domain. On success the caller
 * owns out_value and must destroy it before target_ctx; on failure out_value
 * is TOAD_VAL_NULL and owns no resource. */
toad_script_status_t ts_host_value_from_view(toad_script_ctx_t *target_ctx,
                                              const toad_script_value_view_t *value,
                                              const ts_host_value_limits_t *limits,
                                              toad_value_t *out_value);
toad_script_status_t ts_host_value_from_view_in_env(toad_env_t *target_env,
                                                     const toad_script_value_view_t *value,
                                                     const ts_host_value_limits_t *limits,
                                                     toad_value_t *out_value);

/* Every write invalidates prior getter views first. Publication happens only
 * after complete validation and a successful result-arena deep copy. */
toad_script_status_t ts_host_result_store_view(toad_script_result_t *result,
                                                const toad_script_value_view_t *value,
                                                const ts_host_value_limits_t *limits);
toad_script_status_t ts_host_result_store_engine(toad_script_result_t *result,
                                                  const toad_value_t *value,
                                                  const ts_host_value_limits_t *limits);

/* Validate every scalar/reserved field and deep-copy all error views into the
 * result arena under max_bytes. */
toad_script_status_t ts_host_result_set_error(toad_script_result_t *result,
                                               const toad_script_error_info_t *error,
                                               size_t max_bytes);

/* Later module/instance entry points use this before touching a result. */
toad_script_status_t ts_host_result_check_context(const toad_script_result_t *result,
                                                   const toad_script_ctx_t *ctx);

/* Internal test/instance seams over immutable module-owned state. */
size_t ts_host_module_export_count(const toad_script_module_t *module);
const char *ts_host_module_export_name(const toad_script_module_t *module, size_t index);
uint32_t ts_host_module_export_arity(const toad_script_module_t *module, size_t index);
size_t ts_host_module_parse_count(const toad_script_module_t *module);
size_t ts_host_module_lower_count(const toad_script_module_t *module);
const char *ts_host_module_source(const toad_script_module_t *module);
const char *ts_host_module_name(const toad_script_module_t *module);
toad_script_status_t ts_host_module_execute_numeric(toad_script_module_t *module,
                                                     size_t export_index, int use_jit,
                                                     const double *args, size_t arg_count,
                                                     double *out_result);
toad_script_status_t ts_host_module_execute_numeric_with_runtime(
    toad_script_module_t *module, toad_script_ctx_t *runtime_ctx, size_t export_index,
    int use_jit, const double *args, size_t arg_count, double *out_result);
toad_script_status_t ts_host_module_execute_initializer(toad_script_module_t *module,
                                                         int use_jit);
toad_script_status_t
ts_host_module_execute_initializer_with_runtime(toad_script_module_t *module,
                                                toad_script_ctx_t *runtime_ctx, int use_jit);
void ts_host_module_retain(toad_script_module_t *module);
void ts_host_module_release(toad_script_module_t *module);
toad_script_status_t
ts_host_module_compile_test_parse_oom(toad_script_ctx_t *ctx, toad_script_string_view_t source,
                                      const toad_script_module_options_t *options,
                                      toad_script_result_t *result,
                                      toad_script_module_t **out_module);
toad_script_status_t ts_host_module_test_ast_usage(toad_node_t *root,
                                                    const toad_script_module_options_t *options);

/* Narrow Task 4 seams prove fixed mode and isolated runtime state. Generic
 * value marshalling remains outside this layer until Task 5. */
toad_script_execution_mode_t ts_host_instance_mode(const toad_script_instance_t *instance);
uint32_t ts_host_instance_generation(const toad_script_instance_t *instance);
size_t ts_host_instance_initializer_count(const toad_script_instance_t *instance);
toad_script_status_t ts_host_instance_execute_numeric(toad_script_instance_t *instance,
                                                       toad_script_export_handle_t handle,
                                                       const double *args, size_t arg_count,
                                                       double *out_result);

#endif /* TOAD_SCRIPT_HOST_INTERNAL_H */
