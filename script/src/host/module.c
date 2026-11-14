#include "internal.h"

#include "../mir/internal.h"
#include "../internal.h"
#include <toad/engine.h>
#include "core/vstr.h"

#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TS_HOST_MODULE_SOURCE_LIMIT = 4 * 1024 * 1024,
  TS_HOST_MODULE_AST_LIMIT = 100000,
  TS_HOST_MODULE_IMPORT_LIMIT = 64,
  TS_HOST_MODULE_EXPORT_LIMIT = 256,
  TS_HOST_MODULE_STRING_LIMIT = 4 * 1024 * 1024,
};

typedef struct ts_host_ast_usage_s {
  size_t nodes;
  size_t imports;
  size_t string_bytes;
} ts_host_ast_usage_t;

static int ts_host_options_valid(const toad_script_module_options_t *options);

static toad_script_status_t ts_host_vec_status(stl_status status) {
  if (status == STL_OK) return TOAD_SCRIPT_STATUS_OK;
  if (status == STL_OUT_OF_MEMORY) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (status == STL_CAPACITY_EXCEEDED) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  return TOAD_SCRIPT_STATUS_INVALID_STATE;
}

static int ts_host_charge(size_t *value, size_t amount, size_t limit) {
  if (*value > limit || amount > limit - *value) return 0;
  *value += amount;
  return 1;
}

static void ts_host_parse_span(const char *message, const char *source,
                               uint32_t *line_out, uint32_t *column_out) {
  unsigned line = 0;
  unsigned column = 0;
  const char *line_text = message ? strstr(message, "line ") : NULL;
  if (line_text) {
    (void)sscanf(line_text, "line %u, col %u", &line, &column);
  }
  if (line == 0 && source) {
    line = 1;
    column = 1;
    for (const char *p = source; *p; ++p) {
      if (*p == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
  }
  *line_out = line;
  *column_out = column;
}

static int ts_host_ast_usage(toad_node_t *node, ts_host_ast_usage_t *usage,
                             const toad_script_module_options_t *options) {
  if (!node) return 1;
  if (!ts_host_charge(&usage->nodes, 1, options->max_ast_nodes)) return 0;
  switch (node->type) {
  case TOAD_NODE_NUMBER:
  case TOAD_NODE_INTEGER:
  case TOAD_NODE_VARIABLE:
  case TOAD_NODE_NULL:
  case TOAD_NODE_REST_PARAMETER:
  case TOAD_NODE_THIS:
    return 1;
  case TOAD_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i)
      if (!ts_host_ast_usage(node->data.block.statements[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_STRING:
    return ts_host_charge(&usage->string_bytes, node->data.string.value.len,
                          options->max_string_bytes);
  case TOAD_NODE_TEMPLATE_STRING:
    return ts_host_charge(&usage->string_bytes, node->data.template_string.len,
                          options->max_string_bytes);
  case TOAD_NODE_FUNCTION_EXPRESSION:
  case TOAD_NODE_FUNCTION_DEFINITION:
    for (size_t i = 0; i < node->data.func_def.arg_count; ++i)
      if (!ts_host_ast_usage(node->data.func_def.arg_params[i], usage, options)) return 0;
    return ts_host_ast_usage(node->data.func_def.body, usage, options);
  case TOAD_NODE_FUNCTION_CALL:
    if (node->data.function.name && strcmp(node->data.function.name, "import") == 0 &&
        !ts_host_charge(&usage->imports, 1, options->max_imports)) return 0;
    for (size_t i = 0; i < node->data.function.arg_count; ++i)
      if (!ts_host_ast_usage(node->data.function.args[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_BINARY_OP:
    return ts_host_ast_usage(node->data.binary.left, usage, options) &&
           ts_host_ast_usage(node->data.binary.right, usage, options);
  case TOAD_NODE_ASSIGNMENT:
  case TOAD_NODE_CONSTANT_DECL:
    return ts_host_ast_usage(node->data.assignment.value, usage, options);
  case TOAD_NODE_FLOW:
    return ts_host_ast_usage(node->data.flow.value, usage, options);
  case TOAD_NODE_IF:
    return ts_host_ast_usage(node->data.if_stmt.condition, usage, options) &&
           ts_host_ast_usage(node->data.if_stmt.if_branch, usage, options) &&
           ts_host_ast_usage(node->data.if_stmt.else_branch, usage, options);
  case TOAD_NODE_WHILE:
    return ts_host_ast_usage(node->data.while_loop.condition, usage, options) &&
           ts_host_ast_usage(node->data.while_loop.body, usage, options);
  case TOAD_NODE_FOR:
    return ts_host_ast_usage(node->data.for_loop.init, usage, options) &&
           ts_host_ast_usage(node->data.for_loop.condition, usage, options) &&
           ts_host_ast_usage(node->data.for_loop.post, usage, options) &&
           ts_host_ast_usage(node->data.for_loop.body, usage, options);
  case TOAD_NODE_VECTOR:
    for (size_t i = 0; i < node->data.vector.count; ++i)
      if (!ts_host_ast_usage(node->data.vector.elements[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_INDEX:
    return ts_host_ast_usage(node->data.index_access.array, usage, options) &&
           ts_host_ast_usage(node->data.index_access.index, usage, options);
  case TOAD_NODE_SLICE:
    return ts_host_ast_usage(node->data.slice.array, usage, options) &&
           ts_host_ast_usage(node->data.slice.start, usage, options) &&
           ts_host_ast_usage(node->data.slice.end, usage, options);
  case TOAD_NODE_MEMBER_CALL:
    if (!ts_host_ast_usage(node->data.member_call.object, usage, options)) return 0;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i)
      if (!ts_host_ast_usage(node->data.member_call.args[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_SWITCH:
    if (!ts_host_ast_usage(node->data.switch_stmt.value, usage, options)) return 0;
    for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i)
      if (!ts_host_ast_usage(node->data.switch_stmt.cases[i], usage, options)) return 0;
    return ts_host_ast_usage(node->data.switch_stmt.default_case, usage, options);
  case TOAD_NODE_DO_WHILE:
    return ts_host_ast_usage(node->data.do_while.body, usage, options) &&
           ts_host_ast_usage(node->data.do_while.condition, usage, options);
  case TOAD_NODE_MAP_LITERAL:
    for (size_t i = 0; i < node->data.map_literal.count; ++i)
      if (!ts_host_ast_usage(node->data.map_literal.values[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_MEMBER_ACCESS:
    return ts_host_ast_usage(node->data.member_access.object, usage, options);
  case TOAD_NODE_MEMBER_SET:
    return ts_host_ast_usage(node->data.member_set.object, usage, options) &&
           ts_host_ast_usage(node->data.member_set.value, usage, options);
  case TOAD_NODE_FOR_IN:
    return ts_host_ast_usage(node->data.for_in.collection, usage, options) &&
           ts_host_ast_usage(node->data.for_in.body, usage, options);
  case TOAD_NODE_SPREAD:
    return ts_host_ast_usage(node->data.spread.child, usage, options);
  case TOAD_NODE_DESTRUCTURING_ASSIGNMENT:
    return ts_host_ast_usage(node->data.destructuring.targets, usage, options) &&
           ts_host_ast_usage(node->data.destructuring.value, usage, options);
  case TOAD_NODE_TRY_CATCH:
    return ts_host_ast_usage(node->data.try_catch.try_body, usage, options) &&
           ts_host_ast_usage(node->data.try_catch.catch_body, usage, options);
  case TOAD_NODE_THROW:
    return ts_host_ast_usage(node->data.throw_stmt.value, usage, options);
  case TOAD_NODE_YIELD:
    return ts_host_ast_usage(node->data.yield_expr.value, usage, options);
  case TOAD_NODE_GENERATOR_FUNCTION:
    for (size_t i = 0; i < node->data.generator_def.arg_count; ++i)
      if (!ts_host_ast_usage(node->data.generator_def.arg_params[i], usage, options)) return 0;
    return ts_host_ast_usage(node->data.generator_def.body, usage, options);
  case TOAD_NODE_CLASS_DEF:
    if (!ts_host_ast_usage(node->data.class_def.constructor, usage, options)) return 0;
    for (size_t i = 0; i < node->data.class_def.method_count; ++i)
      if (!ts_host_ast_usage(node->data.class_def.methods[i], usage, options)) return 0;
    for (size_t i = 0; i < node->data.class_def.static_method_count; ++i)
      if (!ts_host_ast_usage(node->data.class_def.static_methods[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_METHOD:
    for (size_t i = 0; i < node->data.method.arg_count; ++i)
      if (!ts_host_ast_usage(node->data.method.arg_params[i], usage, options)) return 0;
    return ts_host_ast_usage(node->data.method.body, usage, options);
  case TOAD_NODE_FIELD_DECL:
    return ts_host_ast_usage(node->data.field_decl.initializer, usage, options);
  case TOAD_NODE_NEW:
    for (size_t i = 0; i < node->data.new_expr.arg_count; ++i)
      if (!ts_host_ast_usage(node->data.new_expr.args[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_SUPER:
    for (size_t i = 0; i < node->data.super_expr.arg_count; ++i)
      if (!ts_host_ast_usage(node->data.super_expr.args[i], usage, options)) return 0;
    return 1;
  case TOAD_NODE_INSTANCEOF:
    return ts_host_ast_usage(node->data.instanceof_expr.object, usage, options) &&
           ts_host_ast_usage(node->data.instanceof_expr.class_expr, usage, options);
  }
  return 0;
}

toad_script_status_t ts_host_module_test_ast_usage(
    toad_node_t *root, const toad_script_module_options_t *options) {
  ts_host_ast_usage_t usage = {0};
  if (!root || !ts_host_options_valid(options))
    return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  return ts_host_ast_usage(root, &usage, options)
             ? TOAD_SCRIPT_STATUS_OK
             : TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
}

static toad_script_status_t ts_host_module_fail(
    toad_script_result_t *result, toad_script_status_t status,
    toad_script_string_view_t module_name, const char *message,
    uint32_t line, uint32_t column) {
  toad_script_error_info_t error = {
      .struct_size = sizeof(error), .status = status,
      .phase = TOAD_SCRIPT_ERROR_PHASE_COMPILE, .line = line,
      .column = column, .length = 1, .module_name = module_name,
      .message = {message, strlen(message)}};
  (void)ts_host_result_set_error(result, &error, TS_HOST_MODULE_STRING_LIMIT);
  return status;
}

static void ts_host_export_table_destroy(ts_host_export_table_t *table) {
  if (!table || !table->entries.initialized) return;
  for (size_t i = 0; i < vec_size(&table->entries); ++i) {
    ts_host_export_entry_t *entry = (ts_host_export_entry_t *)vec_at(&table->entries, i);
    if (entry) tstr_freep(&entry->name);
  }
  vec_destroy(&table->entries);
}

static toad_node_t *ts_host_find_top_function(toad_node_t *root,
                                                const char *name, size_t size) {
  if (!root || root->type != TOAD_NODE_BLOCK) return NULL;
  for (size_t i = 0; i < root->data.block.count; ++i) {
    toad_node_t *node = root->data.block.statements[i];
    if (node && node->type == TOAD_NODE_FUNCTION_DEFINITION &&
        node->data.func_def.name && strlen(node->data.func_def.name) == size &&
        memcmp(node->data.func_def.name, name, size) == 0) return node;
  }
  return NULL;
}

static toad_script_status_t ts_host_scan_exports(
    toad_node_t *root, const toad_script_module_options_t *options,
    ts_host_export_table_t *table, toad_node_t **bad_node) {
  stl_status vec_status = vec_init_bytes(&table->entries, sizeof(ts_host_export_entry_t),
                                         alignof(ts_host_export_entry_t),
                                         TS_HOST_MODULE_EXPORT_LIMIT);
  if (vec_status != STL_OK) return ts_host_vec_status(vec_status);
  if (!root || root->type != TOAD_NODE_BLOCK) return TOAD_SCRIPT_STATUS_OK;

  /* H/E <= 256 makes this O(AST + E^2) scan bounded and deterministic. */
  for (size_t i = 0; i < root->data.block.count; ++i) {
    toad_node_t *node = root->data.block.statements[i];
    if (!node || node->type != TOAD_NODE_FUNCTION_CALL ||
        !node->data.function.name || strcmp(node->data.function.name, "export") != 0)
      continue;
    *bad_node = node;
    if (node->data.function.arg_count != 1 || !node->data.function.args ||
        !node->data.function.args[0] ||
        node->data.function.args[0]->type != TOAD_NODE_STRING)
      return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
    vstr name = node->data.function.args[0]->data.string.value;
    if (!name.data || name.len == 0 || memchr(name.data, '\0', name.len) ||
        !vstr_utf8_valid(name)) return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
    if (vec_size(&table->entries) >= options->max_exports)
      return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    for (size_t j = 0; j < vec_size(&table->entries); ++j) {
      const ts_host_export_entry_t *prior =
          (const ts_host_export_entry_t *)vec_at_const(&table->entries, j);
      if (prior && tstr_len(prior->name) == name.len &&
          memcmp(prior->name, name.data, name.len) == 0)
        return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
    }
    toad_node_t *function = ts_host_find_top_function(root, name.data, name.len);
    if (!function || function->data.func_def.arg_count > 16)
      return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
    for (size_t j = 0; j < function->data.func_def.arg_count; ++j)
      if (!function->data.func_def.arg_params[j] ||
          function->data.func_def.arg_params[j]->type != TOAD_NODE_VARIABLE)
        return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
    ts_host_export_entry_t entry = {
        .name = tstr_dup_len(name.data, name.len),
        .arity = (uint32_t)function->data.func_def.arg_count,
        .line = (uint32_t)(node->line > 0 ? node->line : 0),
        .column = (uint32_t)(node->column > 0 ? node->column : 0),
        .declaration_node = node, .function_node = function};
    if (!entry.name) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
    toad_script_status_t status = ts_host_vec_status(vec_push(&table->entries, &entry));
    if (status != TOAD_SCRIPT_STATUS_OK) {
      tstr_free(entry.name);
      return status;
    }
  }
  *bad_node = NULL;
  return TOAD_SCRIPT_STATUS_OK;
}

void toad_script_module_options_init(toad_script_module_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->max_source_bytes = TS_HOST_MODULE_SOURCE_LIMIT;
  options->max_ast_nodes = TS_HOST_MODULE_AST_LIMIT;
  options->max_imports = TS_HOST_MODULE_IMPORT_LIMIT;
  options->max_exports = TS_HOST_MODULE_EXPORT_LIMIT;
  options->max_string_bytes = TS_HOST_MODULE_STRING_LIMIT;
}

static int ts_host_options_valid(const toad_script_module_options_t *options) {
  if (!options || options->struct_size != sizeof(*options) || options->reserved0 != 0)
    return 0;
  for (size_t i = 0; i < 4; ++i) if (options->reserved[i] != 0) return 0;
  return options->max_source_bytes <= TS_HOST_MODULE_SOURCE_LIMIT &&
         options->max_ast_nodes <= TS_HOST_MODULE_AST_LIMIT &&
         options->max_imports <= TS_HOST_MODULE_IMPORT_LIMIT &&
         options->max_exports <= TS_HOST_MODULE_EXPORT_LIMIT &&
         options->max_string_bytes <= TS_HOST_MODULE_STRING_LIMIT;
}

static toad_script_status_t ts_host_module_compile_impl(
    toad_script_ctx_t *ctx, toad_script_string_view_t source,
    const toad_script_module_options_t *options, toad_script_result_t *result,
    toad_script_module_t **out_module, int force_parse_oom) {
  toad_script_module_t *module = NULL;
  toad_node_t *bad_node = NULL;
  ts_host_ast_usage_t usage = {0};
  toad_env_t validation_env;
  int validation_env_ready = 0;
  int registry_acquired = 0;
  uint32_t failure_line = 0;
  uint32_t failure_column = 0;
  toad_script_status_t status;
  if (!ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_check_context(result, ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_reset_checked(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!out_module) return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_INVALID_ARGUMENT,
                                               (toad_script_string_view_t){0},
                                               "module output is required", 0, 0);
  *out_module = NULL;
  if (atomic_load_explicit(&ctx->closing, memory_order_acquire))
    return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_INVALID_STATE,
                               (toad_script_string_view_t){0},
                               "context is closing", 0, 0);
  if (!ts_host_options_valid(options) || (!source.data && source.size != 0))
    return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_INVALID_ARGUMENT,
                               (toad_script_string_view_t){0},
                               "invalid module compile arguments", 0, 0);
  if (source.size > options->max_source_bytes)
    return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED,
                               options->module_name, "source byte limit exceeded", 0, 0);
  if (source.size == 0 || memchr(source.data, '\0', source.size))
    return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_VALIDATION_ERROR,
                               options->module_name, "source must be nonempty and NUL-free", 0, 0);
  if (!vstr_utf8_valid(vstr_from_buf(source.data, source.size)) ||
      (!options->module_name.data && options->module_name.size != 0) ||
      (options->module_name.size &&
       (memchr(options->module_name.data, '\0', options->module_name.size) ||
       !vstr_utf8_valid(vstr_from_buf(options->module_name.data, options->module_name.size)))))
    return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_INVALID_UTF8,
                               options->module_name, "module text is not valid UTF-8", 0, 0);
  if (options->module_name.size > options->max_string_bytes)
    return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED,
                               options->module_name, "module name byte limit exceeded", 0, 0);

  module = (toad_script_module_t *)calloc(1, sizeof(*module));
  if (!module) return ts_host_module_fail(result, TOAD_SCRIPT_STATUS_OUT_OF_MEMORY,
                                           options->module_name, "module allocation failed", 0, 0);
  module->ctx = ctx;
  module->ref_count = 1;
  module->accepting_instances = 1;
  module->source = tstr_dup_len(source.data, source.size);
  module->module_name = tstr_dup_len(options->module_name.data ? options->module_name.data : "",
                                     options->module_name.size);
  if (!module->source || !module->module_name) { status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY; goto fail; }
  module->parse_count++;
  if (force_parse_oom) {
    module->ast = toad_script_parse_with_error_test_oom(ctx, module->source);
  } else {
    module->ast = toad_script_parse_with_error(ctx, module->source);
  }
  if (!module->ast) {
    status = ctx->error_code == TOAD_SCRIPT_ERROR_OOM
                 ? TOAD_SCRIPT_STATUS_OUT_OF_MEMORY
                 : TOAD_SCRIPT_STATUS_PARSE_ERROR;
    goto fail;
  }
  usage.string_bytes = options->module_name.size;
  if (!ts_host_ast_usage(module->ast, &usage, options)) {
    status = TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED; goto fail;
  }
  status = ts_host_scan_exports(module->ast, options, &module->exports, &bad_node);
  if (status != TOAD_SCRIPT_STATUS_OK) goto fail;
  status = ts_host_registry_acquire_module(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) goto fail;
  registry_acquired = 1;
  toad_env_init(&validation_env);
  validation_env_ready = 1;
  validation_env.parent = &ctx->env;
  status = ts_host_registry_bind_runtime(ctx, &validation_env);
  if (status != TOAD_SCRIPT_STATUS_OK) goto fail;
  if (toad_validate(module->ast, &validation_env, ctx->error_msg,
                      sizeof(ctx->error_msg)) != 0) {
    status = TOAD_SCRIPT_STATUS_VALIDATION_ERROR; goto fail;
  }
  toad_env_free(&validation_env);
  validation_env_ready = 0;
  module->lower_count++;
  if (ts_mir_artifact_compile(ctx, module->ast, &module->exports,
                              &module->artifact) != 0) {
    status = ctx->error_code == TOAD_SCRIPT_ERROR_OOM
                 ? TOAD_SCRIPT_STATUS_OUT_OF_MEMORY
                 : TOAD_SCRIPT_STATUS_UNSUPPORTED_BACKEND_SEMANTIC;
    goto fail;
  }
  ts_context_retain(ctx);
  *out_module = module;
  return TOAD_SCRIPT_STATUS_OK;

fail:
  if (bad_node) {
    failure_line = bad_node->line > 0 ? (uint32_t)bad_node->line : 0;
    failure_column = bad_node->column > 0 ? (uint32_t)bad_node->column : 0;
  } else if (status == TOAD_SCRIPT_STATUS_PARSE_ERROR && module) {
    ts_host_parse_span(ctx->error_msg, module->source, &failure_line,
                       &failure_column);
  }
  if (validation_env_ready) toad_env_free(&validation_env);
  if (registry_acquired) (void)ts_host_registry_release_module(ctx);
  if (module) {
    ts_mir_artifact_destroy(module->artifact);
    if (module->ast) toad_free(module->ast);
    ts_host_export_table_destroy(&module->exports);
    tstr_freep(&module->source);
    tstr_freep(&module->module_name);
    free(module);
  }
  return ts_host_module_fail(result, status, options->module_name,
                             status == TOAD_SCRIPT_STATUS_PARSE_ERROR
                                 ? "module parse failed"
                                 : status == TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED
                                       ? "module compile limit exceeded"
                                       : "module validation or lowering failed",
                             failure_line, failure_column);
}

toad_script_status_t toad_script_module_compile(
    toad_script_ctx_t *ctx, toad_script_string_view_t source,
    const toad_script_module_options_t *options, toad_script_result_t *result,
    toad_script_module_t **out_module) {
  return ts_host_module_compile_impl(ctx, source, options, result, out_module, 0);
}

toad_script_status_t ts_host_module_compile_test_parse_oom(
    toad_script_ctx_t *ctx, toad_script_string_view_t source,
    const toad_script_module_options_t *options, toad_script_result_t *result,
    toad_script_module_t **out_module) {
  return ts_host_module_compile_impl(ctx, source, options, result, out_module, 1);
}

toad_script_status_t toad_script_module_destroy(toad_script_module_t *module,
                                                   toad_script_result_t *result) {
  toad_script_status_t status;
  toad_script_ctx_t *ctx;
  if (!module) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  ctx = module->ctx;
  status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_check_context(result, ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_reset_checked(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!module->accepting_instances) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  module->accepting_instances = 0;
  ts_host_module_release(module);
  return TOAD_SCRIPT_STATUS_OK;
}

void ts_host_module_retain(toad_script_module_t *module) {
  if (!module || module->ref_count == SIZE_MAX) abort();
  module->ref_count++;
}

void ts_host_module_release(toad_script_module_t *module) {
  toad_script_ctx_t *ctx;
  toad_script_status_t status;
  if (!module || module->ref_count == 0) abort();
  module->ref_count--;
  if (module->ref_count != 0) return;
  ctx = module->ctx;
  status = ts_host_registry_release_module(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) abort();
  ts_mir_artifact_destroy(module->artifact);
  toad_free(module->ast);
  ts_host_export_table_destroy(&module->exports);
  tstr_freep(&module->source);
  tstr_freep(&module->module_name);
  free(module);
  ts_context_release(ctx);
}

size_t ts_host_module_export_count(const toad_script_module_t *module) {
  return module ? vec_size(&module->exports.entries) : 0;
}
static const ts_host_export_entry_t *ts_host_module_entry(const toad_script_module_t *module,
                                                          size_t index) {
  return module ? (const ts_host_export_entry_t *)vec_at_const(&module->exports.entries, index) : NULL;
}
const char *ts_host_module_export_name(const toad_script_module_t *module, size_t index) {
  const ts_host_export_entry_t *entry = ts_host_module_entry(module, index);
  return entry ? entry->name : NULL;
}
uint32_t ts_host_module_export_arity(const toad_script_module_t *module, size_t index) {
  const ts_host_export_entry_t *entry = ts_host_module_entry(module, index);
  return entry ? entry->arity : UINT32_MAX;
}
size_t ts_host_module_parse_count(const toad_script_module_t *module) { return module ? module->parse_count : 0; }
size_t ts_host_module_lower_count(const toad_script_module_t *module) { return module ? module->lower_count : 0; }
const char *ts_host_module_source(const toad_script_module_t *module) { return module ? module->source : NULL; }
const char *ts_host_module_name(const toad_script_module_t *module) { return module ? module->module_name : NULL; }
toad_script_status_t ts_host_module_execute_numeric(
    toad_script_module_t *module, size_t export_index, int use_jit,
    const double *args, size_t arg_count, double *out_result) {
  return ts_host_module_execute_numeric_with_runtime(
      module, module ? module->ctx : NULL, export_index, use_jit, args,
      arg_count, out_result);
}

toad_script_status_t ts_host_module_execute_numeric_with_runtime(
    toad_script_module_t *module, toad_script_ctx_t *runtime_ctx,
    size_t export_index, int use_jit, const double *args, size_t arg_count,
    double *out_result) {
  if (!module || !runtime_ctx || (!args && arg_count) || !out_result)
    return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (ts_host_check_owner_thread(module->ctx) != TOAD_SCRIPT_STATUS_OK)
    return TOAD_SCRIPT_STATUS_WRONG_THREAD;
  const ts_host_export_entry_t *entry = ts_host_module_entry(module, export_index);
  if (!entry || arg_count > 16 || arg_count != entry->arity)
    return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  return ts_mir_artifact_execute_numeric(module->artifact, runtime_ctx, export_index,
                                         use_jit, args, arg_count, out_result) == 0
             ? TOAD_SCRIPT_STATUS_OK
             : TOAD_SCRIPT_STATUS_RUNTIME_ERROR;
}

toad_script_status_t ts_host_module_execute_initializer(
    toad_script_module_t *module, int use_jit) {
  return ts_host_module_execute_initializer_with_runtime(
      module, module ? module->ctx : NULL, use_jit);
}

toad_script_status_t ts_host_module_execute_initializer_with_runtime(
    toad_script_module_t *module, toad_script_ctx_t *runtime_ctx,
    int use_jit) {
  if (!module || !runtime_ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (ts_host_check_owner_thread(module->ctx) != TOAD_SCRIPT_STATUS_OK)
    return TOAD_SCRIPT_STATUS_WRONG_THREAD;
  return ts_mir_artifact_execute_initializer(module->artifact, runtime_ctx,
                                             use_jit) == 0
             ? TOAD_SCRIPT_STATUS_OK
             : TOAD_SCRIPT_STATUS_RUNTIME_ERROR;
}
