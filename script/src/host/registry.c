#include "internal.h"

#include "../internal.h"
#include <toad/engine.h>
#include "../../../engine/src/runtime_internal.h"
#include "core/vstr.h"
#include <cstl.h>

#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TS_HOST_REGISTRY_CAPACITY = 256,
  TS_HOST_MAX_VALUE_DEPTH = 64,
  TS_HOST_MAX_VALUE_NODES = 65536,
  TS_HOST_MAX_STRING_BYTES = 4 * 1024 * 1024,
};

typedef struct ts_host_arg_view_state_s {
  mem_pool_t *arena;
  size_t nodes;
  size_t bytes;
} ts_host_arg_view_state_t;

static toad_value_t ts_host_registry_null_value(void) {
  toad_value_t value = {0};
  value.type = TOAD_VAL_NULL;
  return value;
}

static toad_script_status_t ts_host_registry_vec_status(stl_status status) {
  if (status == STL_OK) return TOAD_SCRIPT_STATUS_OK;
  if (status == STL_OUT_OF_MEMORY) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (status == STL_CAPACITY_EXCEEDED) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  return TOAD_SCRIPT_STATUS_INVALID_STATE;
}

static toad_script_status_t ts_host_registry_charge(ts_host_arg_view_state_t *state, size_t count,
                                                     size_t element_size) {
  size_t amount;
  if (count != 0 && element_size > SIZE_MAX / count) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  amount = count * element_size;
  if (state->bytes > TS_HOST_MAX_STRING_BYTES || amount > TS_HOST_MAX_STRING_BYTES - state->bytes)
    return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->bytes += amount;
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t ts_host_registry_validate_name(toad_script_string_view_t name) {
  if (!name.data && name.size != 0) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (name.size == 0) return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  if (name.size > TS_HOST_MAX_STRING_BYTES) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  if (memchr(name.data, '\0', name.size) != NULL) return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  if (!vstr_utf8_valid(vstr_from_buf(name.data, name.size)))
    return TOAD_SCRIPT_STATUS_INVALID_UTF8;
  return TOAD_SCRIPT_STATUS_OK;
}

static int ts_host_registry_name_equal(const ts_host_function_entry_t *entry,
                                       toad_script_string_view_t name) {
  size_t entry_size = tstr_len(entry->name);
  return entry_size == name.size && memcmp(entry->name, name.data, name.size) == 0;
}

static toad_script_status_t ts_host_registry_begin_api(toad_script_ctx_t *ctx,
                                                        toad_script_result_t *result) {
  toad_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_check_context(result, ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  return ts_host_result_reset_checked(result);
}

static toad_script_status_t ts_host_registry_fail(toad_script_result_t *result,
                                                   toad_script_status_t status,
                                                   const char *message) {
  toad_script_error_info_t error = {
      .struct_size = sizeof(error),
      .status = status,
      .phase = TOAD_SCRIPT_ERROR_PHASE_NONE,
      .message = {message, strlen(message)},
  };
  (void)ts_host_result_set_error(result, &error, TS_HOST_MAX_STRING_BYTES);
  return status;
}

static toad_script_status_t
ts_host_registry_validate_descriptor(const toad_script_host_function_descriptor_t *descriptor,
                                     toad_script_host_function_t callback) {
  toad_script_status_t status;
  if (!descriptor || descriptor->struct_size != sizeof(*descriptor) || !callback ||
      descriptor->min_arity > descriptor->max_arity || descriptor->reserved0 != 0)
    return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  for (size_t i = 0; i < sizeof(descriptor->reserved) / sizeof(descriptor->reserved[0]); ++i) {
    if (descriptor->reserved[i] != 0) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  }
  status = ts_host_registry_validate_name(descriptor->name);
  return status;
}

toad_script_status_t ts_host_registry_init(toad_script_ctx_t *ctx) {
  stl_status status;
  if (!ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = vec_init_bytes(&ctx->host_functions, sizeof(ts_host_function_entry_t),
                          alignof(ts_host_function_entry_t), TS_HOST_REGISTRY_CAPACITY);
  if (status != STL_OK) return ts_host_registry_vec_status(status);
  ctx->active_host_modules = 0;
  ctx->host_callback_depth = 0;
  ctx->active_host_instance = NULL;
  ctx->next_instance_generation = 1;
  return TOAD_SCRIPT_STATUS_OK;
}

void ts_host_registry_destroy(toad_script_ctx_t *ctx) {
  if (!ctx || !ctx->host_functions.initialized) return;
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    ts_host_function_entry_t *entry = (ts_host_function_entry_t *)vec_at(&ctx->host_functions, i);
    if (entry) tstr_freep(&entry->name);
  }
  vec_destroy(&ctx->host_functions);
  ctx->active_host_modules = 0;
  ctx->host_callback_depth = 0;
  ctx->active_host_instance = NULL;
}

toad_script_status_t ts_host_check_owner_thread(const toad_script_ctx_t *ctx) {
  return ts_host_context_check_thread(ctx);
}

toad_script_status_t ts_host_registry_acquire_module(toad_script_ctx_t *ctx) {
  toad_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules == SIZE_MAX) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  ctx->active_host_modules++;
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t ts_host_registry_release_module(toad_script_ctx_t *ctx) {
  toad_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules == 0) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  ctx->active_host_modules--;
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t ts_host_registry_find_slot(const toad_script_ctx_t *ctx,
                                                 toad_script_string_view_t name,
                                                 size_t *out_slot) {
  toad_script_status_t status;
  status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!out_slot) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_slot = SIZE_MAX;
  status = ts_host_registry_validate_name(name);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    const ts_host_function_entry_t *entry =
        (const ts_host_function_entry_t *)vec_at_const(&ctx->host_functions, i);
    if (entry && ts_host_registry_name_equal(entry, name)) {
      *out_slot = i;
      return TOAD_SCRIPT_STATUS_OK;
    }
  }
  return TOAD_SCRIPT_STATUS_NOT_FOUND;
}

toad_script_status_t ts_host_registry_get_slot(const toad_script_ctx_t *ctx, size_t slot,
                                                const ts_host_function_entry_t **out_entry) {
  toad_script_status_t status;
  status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!out_entry) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_entry = NULL;
  if (slot >= vec_size(&ctx->host_functions)) return TOAD_SCRIPT_STATUS_NOT_FOUND;
  *out_entry = (const ts_host_function_entry_t *)vec_at_const(&ctx->host_functions, slot);
  return *out_entry ? TOAD_SCRIPT_STATUS_OK : TOAD_SCRIPT_STATUS_INVALID_STATE;
}

toad_script_status_t toad_script_context_register_host_function(
    toad_script_ctx_t *ctx, const toad_script_host_function_descriptor_t *descriptor,
    toad_script_host_function_t callback, void *user_data, toad_script_result_t *result) {
  ts_host_function_entry_t entry = {0};
  toad_script_status_t status = ts_host_registry_begin_api(ctx, result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules != 0)
    return ts_host_registry_fail(result, TOAD_SCRIPT_STATUS_INVALID_STATE,
                                 "host registry is frozen while modules are active");
  status = ts_host_registry_validate_descriptor(descriptor, callback);
  if (status != TOAD_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "invalid host function descriptor");
  status = ts_host_registry_find_slot(ctx, descriptor->name, &(size_t){0});
  if (status == TOAD_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, TOAD_SCRIPT_STATUS_INVALID_STATE,
                                 "host function name is already registered");
  if (status != TOAD_SCRIPT_STATUS_NOT_FOUND)
    return ts_host_registry_fail(result, status, "failed to search host registry");
  if (vec_size(&ctx->host_functions) >= TS_HOST_REGISTRY_CAPACITY)
    return ts_host_registry_fail(result, TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED,
                                 "host registry capacity exceeded");

  entry.ctx = ctx;
  entry.name = tstr_dup_len(descriptor->name.data, descriptor->name.size);
  if (!entry.name)
    return ts_host_registry_fail(result, TOAD_SCRIPT_STATUS_OUT_OF_MEMORY,
                                 "failed to copy host function name");
  entry.min_arity = descriptor->min_arity;
  entry.max_arity = descriptor->max_arity;
  entry.callback = callback;
  entry.user_data = user_data;
  status = ts_host_registry_vec_status(vec_push(&ctx->host_functions, &entry));
  if (status != TOAD_SCRIPT_STATUS_OK) {
    tstr_free(entry.name);
    return ts_host_registry_fail(result, status, "failed to append host registry slot");
  }
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t toad_script_context_unregister_host_function(toad_script_ctx_t *ctx,
                                                                    toad_script_string_view_t name,
                                                                    toad_script_result_t *result) {
  ts_host_function_entry_t removed = {0};
  size_t slot;
  toad_script_status_t status = ts_host_registry_begin_api(ctx, result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules != 0)
    return ts_host_registry_fail(result, TOAD_SCRIPT_STATUS_INVALID_STATE,
                                 "host registry is frozen while modules are active");
  status = ts_host_registry_validate_name(name);
  if (status != TOAD_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "invalid host function name");
  status = ts_host_registry_find_slot(ctx, name, &slot);
  if (status != TOAD_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "host function is not registered");
  status = ts_host_registry_vec_status(vec_erase(&ctx->host_functions, slot, &removed));
  if (status != TOAD_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "failed to remove host registry slot");
  tstr_free(removed.name);
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t ts_host_arg_view_from_engine(ts_host_arg_view_state_t *state,
                                                          const toad_value_t *source,
                                                          toad_script_value_view_t *target,
                                                          size_t depth) {
  toad_script_status_t status = TOAD_SCRIPT_STATUS_OK;
  if (!source || !target) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (depth > TS_HOST_MAX_VALUE_DEPTH || state->nodes == TS_HOST_MAX_VALUE_NODES)
    return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->nodes++;
  memset(target, 0, sizeof(*target));

  switch (source->type) {
  case TOAD_VAL_NULL:
    target->kind = TOAD_SCRIPT_VALUE_NULL;
    return TOAD_SCRIPT_STATUS_OK;
  case TOAD_VAL_BOOL:
    if (source->data.boolean != 0 && source->data.boolean != 1)
      return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
    target->kind = TOAD_SCRIPT_VALUE_BOOL;
    target->as.boolean = (uint8_t)source->data.boolean;
    return TOAD_SCRIPT_STATUS_OK;
  case TOAD_VAL_INTEGER:
    target->kind = TOAD_SCRIPT_VALUE_INT64;
    target->as.integer = source->data.integer;
    return TOAD_SCRIPT_STATUS_OK;
  case TOAD_VAL_NUMBER:
    target->kind = TOAD_SCRIPT_VALUE_NUMBER;
    target->as.number = source->data.number;
    return TOAD_SCRIPT_STATUS_OK;
  case TOAD_VAL_STRING:
    if (!source->data.string.data && source->data.string.len != 0)
      return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
    status = ts_host_registry_charge(state, source->data.string.len, 1);
    if (status != TOAD_SCRIPT_STATUS_OK) return status;
    if (!vstr_utf8_valid(source->data.string)) return TOAD_SCRIPT_STATUS_INVALID_UTF8;
    target->kind = TOAD_SCRIPT_VALUE_STRING;
    target->as.string.data = source->data.string.data;
    target->as.string.size = source->data.string.len;
    return TOAD_SCRIPT_STATUS_OK;
  case TOAD_VAL_VECTOR: {
    toad_script_value_view_t *items = NULL;
    if (!source->data.vector.data && source->data.vector.size != 0)
      return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (source->data.vector.size > TS_HOST_MAX_VALUE_NODES - state->nodes)
      return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_registry_charge(state, source->data.vector.size, sizeof(*items));
    if (status != TOAD_SCRIPT_STATUS_OK) return status;
    if (source->data.vector.size != 0) {
      items = (toad_script_value_view_t *)mem_alloc_array(state->arena, sizeof(*items),
                                                           source->data.vector.size);
      if (!items) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->data.vector.size; ++i) {
        memset(&items[i], 0, sizeof(items[i]));
        items[i].kind = TOAD_SCRIPT_VALUE_NUMBER;
        items[i].as.number = source->data.vector.data[i];
        state->nodes++;
      }
    }
    target->kind = TOAD_SCRIPT_VALUE_ARRAY;
    target->as.array.items = items;
    target->as.array.count = source->data.vector.size;
    return TOAD_SCRIPT_STATUS_OK;
  }
  case TOAD_VAL_LIST: {
    toad_script_value_view_t *items = NULL;
    if (!source->data.list.items && source->data.list.count != 0)
      return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (source->data.list.count > TS_HOST_MAX_VALUE_NODES - state->nodes)
      return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_registry_charge(state, source->data.list.count, sizeof(*items));
    if (status != TOAD_SCRIPT_STATUS_OK) return status;
    if (source->data.list.count != 0) {
      items = (toad_script_value_view_t *)mem_alloc_array(state->arena, sizeof(*items),
                                                           source->data.list.count);
      if (!items) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->data.list.count; ++i) {
        status =
            ts_host_arg_view_from_engine(state, &source->data.list.items[i], &items[i], depth + 1);
        if (status != TOAD_SCRIPT_STATUS_OK) return status;
      }
    }
    target->kind = TOAD_SCRIPT_VALUE_ARRAY;
    target->as.array.items = items;
    target->as.array.count = source->data.list.count;
    return TOAD_SCRIPT_STATUS_OK;
  }
  case TOAD_VAL_MAP:
  case TOAD_VAL_OBJECT: {
    size_t count = toad_map_count(source);
    toad_script_record_entry_view_t *entries = NULL;
    toad_map_iter_t iterator;
    const char *key = NULL;
    toad_value_t child;
    size_t index = 0;
    if (count > TS_HOST_MAX_VALUE_NODES - state->nodes) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_registry_charge(state, count, sizeof(*entries));
    if (status != TOAD_SCRIPT_STATUS_OK) return status;
    if (count != 0) {
      entries = (toad_script_record_entry_view_t *)mem_alloc_array(state->arena, sizeof(*entries),
                                                                    count);
      if (!entries) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
    }
    iterator = toad_map_iter_begin(source);
    while (index < count && toad_map_iter_next(&iterator, &key, &child)) {
      size_t key_size;
      if (!key) return TOAD_SCRIPT_STATUS_INVALID_STATE;
      key_size = strlen(key);
      if (key_size == 0) return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
      status = ts_host_registry_charge(state, key_size, 1);
      if (status != TOAD_SCRIPT_STATUS_OK) return status;
      if (!vstr_utf8_valid(vstr_from_buf(key, key_size))) return TOAD_SCRIPT_STATUS_INVALID_UTF8;
      entries[index].key.data = key;
      entries[index].key.size = key_size;
      status = ts_host_arg_view_from_engine(state, &child, &entries[index].value, depth + 1);
      if (status != TOAD_SCRIPT_STATUS_OK) return status;
      index++;
    }
    if (index != count) return TOAD_SCRIPT_STATUS_INVALID_STATE;
    target->kind = TOAD_SCRIPT_VALUE_RECORD;
    target->as.record.entries = entries;
    target->as.record.count = count;
    return TOAD_SCRIPT_STATUS_OK;
  }
  default:
    return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  }
}

static const char *ts_host_status_name(toad_script_status_t status) {
  switch (status) {
  case TOAD_SCRIPT_STATUS_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case TOAD_SCRIPT_STATUS_OUT_OF_MEMORY:
    return "OUT_OF_MEMORY";
  case TOAD_SCRIPT_STATUS_WRONG_THREAD:
    return "WRONG_THREAD";
  case TOAD_SCRIPT_STATUS_INVALID_STATE:
    return "INVALID_STATE";
  case TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED:
    return "LIMIT_EXCEEDED";
  case TOAD_SCRIPT_STATUS_INVALID_UTF8:
    return "INVALID_UTF8";
  case TOAD_SCRIPT_STATUS_HOST_ERROR:
    return "HOST_ERROR";
  default:
    return "HOST_ERROR";
  }
}

static void ts_host_runtime_throw_view(toad_script_ctx_t *owner_ctx, toad_env_t *runtime_ctx,
                                       toad_script_status_t status, int32_t cause_code,
                                       toad_script_string_view_t message,
                                       const char *function_name) {
  toad_value_t error_value = ts_host_registry_null_value();
  const char *status_name = ts_host_status_name(status);
  int printable_size;
  if (!message.data) message = (toad_script_string_view_t){"host callback failed", 20};
  if (!function_name) function_name = "<host>";
  printable_size = (int)(message.size < 128 ? message.size : 128);
  if (cause_code != 0) {
    (void)snprintf(runtime_ctx->error_msg, sizeof(runtime_ctx->error_msg),
                   "%s: %.*s (function=%s, cause=%d)", status_name, printable_size, message.data,
                   function_name, cause_code);
  } else {
    (void)snprintf(runtime_ctx->error_msg, sizeof(runtime_ctx->error_msg), "%s: %.*s (function=%s)",
                   status_name, printable_size, message.data, function_name);
  }
  toad_value_destroy(&runtime_ctx->error_value);
  if (toad_value_copy_to_env(toad_val_str(vstr_from_cstr(runtime_ctx->error_msg)), runtime_ctx,
                               &error_value) == 0)
    runtime_ctx->error_value = error_value;
  runtime_ctx->error_line = runtime_ctx->last_line;
  runtime_ctx->error_column = runtime_ctx->last_column;
  runtime_ctx->flow = toad_FLOW_THROW;
  if (owner_ctx && owner_ctx->active_host_instance) {
    ts_host_instance_record_callback_error(
        owner_ctx->active_host_instance, status, cause_code,
        (toad_script_string_view_t){function_name, strlen(function_name)}, message,
        runtime_ctx->error_line > 0 ? (uint32_t)runtime_ctx->error_line : 0,
        runtime_ctx->error_column > 0 ? (uint32_t)runtime_ctx->error_column : 0);
  }
}

static void ts_host_runtime_throw(toad_script_ctx_t *owner_ctx, toad_env_t *runtime_ctx,
                                  toad_script_status_t status, int32_t cause_code,
                                  const char *message, const char *function_name) {
  if (!message) message = "host callback failed";
  ts_host_runtime_throw_view(owner_ctx, runtime_ctx, status, cause_code,
                             (toad_script_string_view_t){message, strlen(message)}, function_name);
}

toad_script_status_t
toad_script_host_result_set_value(toad_script_host_result_builder_t *builder,
                                   const toad_script_value_view_t *value) {
  ts_host_value_limits_t limits = {
      TS_HOST_MAX_VALUE_DEPTH,
      TS_HOST_MAX_VALUE_NODES,
      TS_HOST_MAX_STRING_BYTES,
  };
  if (!builder) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (builder->state != TS_HOST_BUILDER_EMPTY) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  builder->state = TS_HOST_BUILDER_VALUE;
  builder->terminal_status = ts_host_value_from_view(builder->ctx, value, &limits, &builder->value);
  return builder->terminal_status;
}

toad_script_status_t
toad_script_host_result_set_error(toad_script_host_result_builder_t *builder, int32_t cause_code,
                                   toad_script_string_view_t message) {
  if (!builder) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (builder->state != TS_HOST_BUILDER_EMPTY) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  builder->state = TS_HOST_BUILDER_ERROR;
  builder->cause_code = cause_code;
  if (!message.data && message.size != 0) {
    builder->terminal_status = TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
    return builder->terminal_status;
  }
  if (message.size > TS_HOST_MAX_STRING_BYTES) {
    builder->terminal_status = TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    return builder->terminal_status;
  }
  if (!vstr_utf8_valid(vstr_from_buf(message.data, message.size))) {
    builder->terminal_status = TOAD_SCRIPT_STATUS_INVALID_UTF8;
    return builder->terminal_status;
  }
  builder->error_message = tstr_dup_len(message.data ? message.data : "", message.size);
  if (!builder->error_message) {
    builder->terminal_status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
    return builder->terminal_status;
  }
  builder->terminal_status = TOAD_SCRIPT_STATUS_OK;
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_value_t ts_host_registry_callback_adapter(size_t arg_count, toad_value_t *args,
                                                        toad_env_t *runtime_ctx,
                                                        void *user_data) {
  const ts_host_function_entry_t *entry = (const ts_host_function_entry_t *)user_data;
  toad_script_host_result_builder_t builder = {0};
  ts_host_arg_view_state_t view_state = {0};
  toad_script_value_view_t *views = NULL;
  mem_pool_t view_arena;
  toad_script_status_t status;
  toad_script_instance_t *active_instance = NULL;
  toad_value_t result = ts_host_registry_null_value();

  if (!entry || !entry->ctx || !runtime_ctx) return result;
  status = ts_host_check_owner_thread(entry->ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) {
    ts_host_runtime_throw(entry->ctx, runtime_ctx, status, 0,
                          "host callback ran outside its owner thread", entry->name);
    return result;
  }
  if (arg_count < (size_t)entry->min_arity || arg_count > (size_t)entry->max_arity) {
    ts_host_runtime_throw(entry->ctx, runtime_ctx, TOAD_SCRIPT_STATUS_HOST_ERROR, 0,
                          "host function arity mismatch", entry->name);
    return result;
  }
  if (arg_count != 0 && !args) {
    ts_host_runtime_throw(entry->ctx, runtime_ctx, TOAD_SCRIPT_STATUS_INVALID_ARGUMENT, 0,
                          "host arguments have no backing storage", entry->name);
    return result;
  }
  if (mem_init(&view_arena, 0) != 0) {
    ts_host_runtime_throw(entry->ctx, runtime_ctx, TOAD_SCRIPT_STATUS_OUT_OF_MEMORY, 0,
                          "failed to initialize host argument arena", entry->name);
    return result;
  }
  view_state.arena = &view_arena;
  if (arg_count != 0) {
    status = ts_host_registry_charge(&view_state, arg_count, sizeof(*views));
    if (status == TOAD_SCRIPT_STATUS_OK) {
      views = (toad_script_value_view_t *)mem_alloc_array(&view_arena, sizeof(*views), arg_count);
      if (!views) status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
    }
    for (size_t i = 0; status == TOAD_SCRIPT_STATUS_OK && i < arg_count; ++i)
      status = ts_host_arg_view_from_engine(&view_state, &args[i], &views[i], 1);
    if (status != TOAD_SCRIPT_STATUS_OK) {
      ts_host_runtime_throw(entry->ctx, runtime_ctx, status, 0, "failed to adapt host arguments",
                            entry->name);
      mem_destroy(&view_arena);
      return result;
    }
  }

  builder.ctx = entry->ctx;
  builder.state = TS_HOST_BUILDER_EMPTY;
  builder.terminal_status = TOAD_SCRIPT_STATUS_OK;
  builder.value = ts_host_registry_null_value();
  if (runtime_ctx->safe_point &&
      runtime_ctx->safe_point(runtime_ctx->safe_point_user_data, TOAD_SAFE_POINT_CALLBACK_BEFORE,
                              1) != 0) {
    runtime_ctx->aborted = 1;
    mem_destroy(&view_arena);
    return result;
  }
  status = ts_host_instance_enter_callback(entry->ctx, &active_instance);
  if (status != TOAD_SCRIPT_STATUS_OK) {
    ts_host_runtime_throw(entry->ctx, runtime_ctx, status, 0,
                          "host callback state transition failed", entry->name);
    mem_destroy(&view_arena);
    return result;
  }
  status = entry->callback(entry->user_data, views, arg_count, &builder);
  ts_host_instance_leave_callback(entry->ctx, active_instance);

  if (status == TOAD_SCRIPT_STATUS_OK && builder.terminal_status == TOAD_SCRIPT_STATUS_OK &&
      builder.state == TS_HOST_BUILDER_VALUE && runtime_ctx->safe_point &&
      runtime_ctx->safe_point(runtime_ctx->safe_point_user_data, TOAD_SAFE_POINT_CALLBACK_AFTER,
                              0) != 0) {
    runtime_ctx->aborted = 1;
    toad_value_destroy(&builder.value);
    tstr_free(builder.error_message);
    mem_destroy(&view_arena);
    return result;
  }

  if (status == TOAD_SCRIPT_STATUS_OK && builder.state == TS_HOST_BUILDER_EMPTY) {
    status = TOAD_SCRIPT_STATUS_HOST_ERROR;
    ts_host_runtime_throw(entry->ctx, runtime_ctx, status, 0,
                          "callback returned success without a result", entry->name);
  } else if (builder.terminal_status != TOAD_SCRIPT_STATUS_OK) {
    status = builder.terminal_status;
    ts_host_runtime_throw(entry->ctx, runtime_ctx, status, builder.cause_code,
                          "host result builder rejected its terminal write", entry->name);
  } else if (status != TOAD_SCRIPT_STATUS_OK) {
    toad_script_string_view_t message =
        builder.error_message
            ? (toad_script_string_view_t){builder.error_message, tstr_len(builder.error_message)}
            : (toad_script_string_view_t){"host callback failed", 20};
    ts_host_runtime_throw_view(entry->ctx, runtime_ctx, status, builder.cause_code, message,
                               entry->name);
  } else if (builder.state == TS_HOST_BUILDER_ERROR) {
    status = TOAD_SCRIPT_STATUS_HOST_ERROR;
    toad_script_string_view_t message =
        builder.error_message
            ? (toad_script_string_view_t){builder.error_message, tstr_len(builder.error_message)}
            : (toad_script_string_view_t){"host callback failed", 20};
    ts_host_runtime_throw_view(entry->ctx, runtime_ctx, status, builder.cause_code, message,
                               entry->name);
  } else {
    result = builder.value;
    builder.value = ts_host_registry_null_value();
  }

  toad_value_destroy(&builder.value);
  tstr_free(builder.error_message);
  mem_destroy(&view_arena);
  return result;
}

double ts_host_registry_invoke_numeric_slot(toad_script_ctx_t *registry_owner_ctx,
                                            toad_script_ctx_t *runtime_ctx, size_t slot,
                                            size_t arg_count, const double *numeric_args) {
  const ts_host_function_entry_t *entry = NULL;
  toad_value_t args[16];
  toad_value_t result;
  double numeric = 0.0;
  if (!registry_owner_ctx || !runtime_ctx || arg_count > 16 || (!numeric_args && arg_count) ||
      ts_host_registry_get_slot(registry_owner_ctx, slot, &entry) != TOAD_SCRIPT_STATUS_OK)
    return 0.0;
  for (size_t i = 0; i < arg_count; ++i)
    args[i] = toad_val_num(numeric_args[i]);
  result = ts_host_registry_callback_adapter(arg_count, args, &runtime_ctx->env, (void *)entry);
  if (result.type == TOAD_VAL_NUMBER) numeric = result.data.number;
  else if (result.type == TOAD_VAL_INTEGER) numeric = (double)result.data.integer;
  else if (result.type == TOAD_VAL_BOOL) numeric = result.data.boolean ? 1.0 : 0.0;
  toad_value_destroy(&result);
  return numeric;
}

static toad_script_status_t
ts_host_registry_bind_runtime_impl(toad_script_ctx_t *ctx, toad_env_t *runtime_ctx,
                                   toad_registration_fault_fn should_fail,
                                   void *fault_user_data) {
  toad_native_registration_t registrations[TS_HOST_REGISTRY_CAPACITY];
  toad_registration_status_t registration_status;
  toad_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!runtime_ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (ctx->active_host_modules == 0) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    const ts_host_function_entry_t *entry =
        (const ts_host_function_entry_t *)vec_at_const(&ctx->host_functions, i);
    if (!entry || toad_env_has_func(runtime_ctx, entry->name))
      return TOAD_SCRIPT_STATUS_INVALID_STATE;
  }
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    ts_host_function_entry_t *entry = (ts_host_function_entry_t *)vec_at(&ctx->host_functions, i);
    if (!entry) return TOAD_SCRIPT_STATUS_INVALID_STATE;
    registrations[i].name = entry->name;
    registrations[i].fn = ts_host_registry_callback_adapter;
    registrations[i].user_data = entry;
    registrations[i].flags = TOAD_NATIVE_FLAG_NONE;
  }
  registration_status = toad_env_register_funcs_checked_with_fault(
      runtime_ctx, registrations, vec_size(&ctx->host_functions), should_fail, fault_user_data);
  if (registration_status == TOAD_REGISTRATION_OK) return TOAD_SCRIPT_STATUS_OK;
  if (registration_status == TOAD_REGISTRATION_OUT_OF_MEMORY)
    return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (registration_status == TOAD_REGISTRATION_CONFLICT) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
}

toad_script_status_t ts_host_registry_bind_runtime(toad_script_ctx_t *ctx,
                                                    toad_env_t *runtime_ctx) {
  return ts_host_registry_bind_runtime_impl(ctx, runtime_ctx, NULL, NULL);
}

typedef struct ts_host_registration_fault_s {
  size_t allocation_index;
} ts_host_registration_fault_t;

static int ts_host_registration_should_fail(size_t allocation_index, void *user_data) {
  const ts_host_registration_fault_t *fault = (const ts_host_registration_fault_t *)user_data;
  return allocation_index == fault->allocation_index;
}

toad_script_status_t ts_host_registry_bind_runtime_test_fault(toad_script_ctx_t *ctx,
                                                               toad_env_t *runtime_ctx,
                                                               size_t allocation_index) {
  ts_host_registration_fault_t fault = {allocation_index};
  return ts_host_registry_bind_runtime_impl(ctx, runtime_ctx, ts_host_registration_should_fail,
                                            &fault);
}
