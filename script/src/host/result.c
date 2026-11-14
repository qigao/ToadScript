#include "internal.h"

#include "../internal.h"
#include "core/vstr.h"
#include <cstl.h>

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define TS_HOST_FNV_OFFSET_BASIS UINT64_C(14695981039346656037)
#define TS_HOST_FNV_PRIME UINT64_C(1099511628211)

typedef struct ts_host_validation_state_s {
  const ts_host_value_limits_t *limits;
  size_t nodes;
  size_t bytes;
  hash_set_t active_containers;
} ts_host_validation_state_t;

static toad_script_value_view_t ts_host_null_view(void);

toad_script_status_t ts_host_context_check_thread(const toad_script_ctx_t *ctx) {
  if (!ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (!ts_context_is_owner_thread(ctx)) return TOAD_SCRIPT_STATUS_WRONG_THREAD;
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t ts_host_result_check_thread(const toad_script_result_t *result) {
  if (!result) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  return ts_host_context_check_thread(result->ctx);
}

static void ts_host_result_clear_unchecked(toad_script_result_t *result) {
  mem_reset(&result->view_arena);
  result->value = ts_host_null_view();
  memset(&result->error, 0, sizeof(result->error));
  result->has_value = 0;
  result->has_error = 0;
}

static void ts_host_result_contract_violation(void) {
  /* The locked void ABI cannot report WRONG_THREAD. Continuing would either
   * leak retained ownership or free another thread's arena, so terminate. */
  abort();
}

static toad_script_value_view_t ts_host_null_view(void) {
  toad_script_value_view_t value = {0};
  value.kind = TOAD_SCRIPT_VALUE_NULL;
  return value;
}

static toad_value_t ts_host_toad_null(void) {
  toad_value_t value = {0};
  value.type = TOAD_VAL_NULL;
  return value;
}

static toad_script_status_t ts_host_stl_status(stl_status status) {
  if (status == STL_OK) return TOAD_SCRIPT_STATUS_OK;
  if (status == STL_OUT_OF_MEMORY) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (status == STL_CAPACITY_EXCEEDED) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
}

static toad_script_status_t ts_host_charge_bytes(ts_host_validation_state_t *state, size_t count,
                                                  size_t element_size) {
  size_t bytes;
  if (count != 0 && element_size > SIZE_MAX / count) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  bytes = count * element_size;
  if (bytes > state->limits->max_bytes - state->bytes) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->bytes += bytes;
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t ts_host_validate_string(ts_host_validation_state_t *state,
                                                     toad_script_string_view_t value,
                                                     int require_nonempty,
                                                     int reject_embedded_nul) {
  toad_script_status_t status;
  if (!value.data && value.size != 0) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (require_nonempty && value.size == 0) return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  /* engine map keys are C strings, so ABI record keys must have one exact
   * representation after conversion. Ordinary string values remain length-based. */
  if (reject_embedded_nul && memchr(value.data, '\0', value.size) != NULL)
    return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  if (value.size == SIZE_MAX) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  status = ts_host_charge_bytes(state, value.size + 1, 1);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!vstr_utf8_valid(vstr_from_buf(value.data, value.size)))
    return TOAD_SCRIPT_STATUS_INVALID_UTF8;
  return TOAD_SCRIPT_STATUS_OK;
}

static size_t ts_host_string_hash(const void *key, size_t key_size, void *context) {
  const toad_script_string_view_t *view = (const toad_script_string_view_t *)key;
  uint64_t hash = TS_HOST_FNV_OFFSET_BASIS;
  (void)key_size;
  (void)context;
  for (size_t i = 0; i < view->size; ++i) {
    hash ^= (uint8_t)view->data[i];
    hash *= TS_HOST_FNV_PRIME;
  }
  return (size_t)hash;
}

static bool ts_host_string_equal(const void *left, const void *right, size_t key_size,
                                 void *context) {
  const toad_script_string_view_t *lhs = (const toad_script_string_view_t *)left;
  const toad_script_string_view_t *rhs = (const toad_script_string_view_t *)right;
  (void)key_size;
  (void)context;
  return lhs->size == rhs->size && (lhs->size == 0 || memcmp(lhs->data, rhs->data, lhs->size) == 0);
}

static toad_script_status_t ts_host_validate_node(ts_host_validation_state_t *state,
                                                   const toad_script_value_view_t *value,
                                                   size_t depth) {
  toad_script_status_t status = TOAD_SCRIPT_STATUS_OK;
  const toad_script_value_view_t *identity = value;
  int active = 0;

  if (!value) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (depth > state->limits->max_depth || state->nodes == state->limits->max_nodes)
    return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->nodes++;
  if (value->reserved != 0) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;

  switch (value->kind) {
  case TOAD_SCRIPT_VALUE_NULL:
  case TOAD_SCRIPT_VALUE_INT64:
  case TOAD_SCRIPT_VALUE_NUMBER:
    return TOAD_SCRIPT_STATUS_OK;
  case TOAD_SCRIPT_VALUE_BOOL:
    return value->as.boolean <= 1 ? TOAD_SCRIPT_STATUS_OK : TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  case TOAD_SCRIPT_VALUE_STRING:
    return ts_host_validate_string(state, value->as.string, 0, 0);
  case TOAD_SCRIPT_VALUE_ARRAY:
    if (!value->as.array.items && value->as.array.count != 0)
      return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (value->as.array.count > state->limits->max_nodes - state->nodes)
      return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_charge_bytes(state, value->as.array.count, sizeof(toad_script_value_view_t));
    break;
  case TOAD_SCRIPT_VALUE_RECORD:
    if (!value->as.record.entries && value->as.record.count != 0)
      return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (value->as.record.count > state->limits->max_nodes - state->nodes)
      return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_charge_bytes(state, value->as.record.count,
                                  sizeof(toad_script_record_entry_view_t));
    break;
  default:
    return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  }
  if (status != TOAD_SCRIPT_STATUS_OK) return status;

  if (hash_set_contains(&state->active_containers, &identity))
    return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  status = ts_host_stl_status(hash_set_add(&state->active_containers, &identity));
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  active = 1;

  if (value->kind == TOAD_SCRIPT_VALUE_ARRAY) {
    for (size_t i = 0; i < value->as.array.count; ++i) {
      status = ts_host_validate_node(state, &value->as.array.items[i], depth + 1);
      if (status != TOAD_SCRIPT_STATUS_OK) break;
    }
  } else {
    hash_set_t keys = {0};
    stl_status init_status = STL_OK;
    if (value->as.record.count != 0) {
      init_status = hash_set_init_bytes(&keys, sizeof(toad_script_string_view_t),
                                        alignof(toad_script_string_view_t), value->as.record.count,
                                        ts_host_string_hash, ts_host_string_equal, NULL);
      if (init_status != STL_OK) status = ts_host_stl_status(init_status);
    }
    for (size_t i = 0; status == TOAD_SCRIPT_STATUS_OK && i < value->as.record.count; ++i) {
      const toad_script_record_entry_view_t *entry = &value->as.record.entries[i];
      status = ts_host_validate_string(state, entry->key, 1, 1);
      if (status != TOAD_SCRIPT_STATUS_OK) break;
      if (hash_set_contains(&keys, &entry->key)) {
        status = TOAD_SCRIPT_STATUS_DUPLICATE_RECORD_KEY;
        break;
      }
      init_status = hash_set_add(&keys, &entry->key);
      if (init_status != STL_OK) {
        status = ts_host_stl_status(init_status);
        break;
      }
      status = ts_host_validate_node(state, &entry->value, depth + 1);
    }
    hash_set_destroy(&keys);
  }

  if (active && hash_set_remove(&state->active_containers, &identity) != STL_OK &&
      status == TOAD_SCRIPT_STATUS_OK)
    status = TOAD_SCRIPT_STATUS_INVALID_STATE;
  return status;
}

toad_script_status_t ts_host_validate_value_view(const toad_script_value_view_t *value,
                                                  const ts_host_value_limits_t *limits) {
  if (!value) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  return ts_host_validate_value_views(value, 1, limits);
}

toad_script_status_t ts_host_validate_value_views(const toad_script_value_view_t *values,
                                                   size_t count,
                                                   const ts_host_value_limits_t *limits) {
  ts_host_validation_state_t state = {0};
  stl_status init_status;
  toad_script_status_t status = TOAD_SCRIPT_STATUS_OK;
  if ((!values && count != 0) || !limits || limits->max_depth == 0 || limits->max_nodes == 0 ||
      limits->max_bytes == 0)
    return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;

  state.limits = limits;
  init_status =
      hash_set_init_bytes(&state.active_containers, sizeof(const toad_script_value_view_t *),
                          alignof(const toad_script_value_view_t *), limits->max_nodes, hash_bytes,
                          hash_key_equal, NULL);
  if (init_status != STL_OK) return ts_host_stl_status(init_status);
  for (size_t i = 0; i < count; ++i) {
    status = ts_host_validate_node(&state, &values[i], 1);
    if (status != TOAD_SCRIPT_STATUS_OK) break;
  }
  hash_set_destroy(&state.active_containers);
  return status;
}

static char *ts_host_copy_string(mem_pool_t *arena, toad_script_string_view_t source) {
  char *copy = (char *)mem_alloc(arena, source.size + 1);
  if (!copy) return NULL;
  if (source.size != 0) memcpy(copy, source.data, source.size);
  copy[source.size] = '\0';
  return copy;
}

static toad_script_status_t ts_host_copy_view(mem_pool_t *arena,
                                               const toad_script_value_view_t *source,
                                               toad_script_value_view_t *target) {
  toad_script_value_view_t copy = *source;
  if (source->kind == TOAD_SCRIPT_VALUE_STRING) {
    copy.as.string.data = ts_host_copy_string(arena, source->as.string);
    if (!copy.as.string.data) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  } else if (source->kind == TOAD_SCRIPT_VALUE_ARRAY) {
    toad_script_value_view_t *items = NULL;
    if (source->as.array.count != 0) {
      items = (toad_script_value_view_t *)mem_alloc_array(arena, sizeof(*items),
                                                           source->as.array.count);
      if (!items) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->as.array.count; ++i) {
        toad_script_status_t status =
            ts_host_copy_view(arena, &source->as.array.items[i], &items[i]);
        if (status != TOAD_SCRIPT_STATUS_OK) return status;
      }
    }
    copy.as.array.items = items;
  } else if (source->kind == TOAD_SCRIPT_VALUE_RECORD) {
    toad_script_record_entry_view_t *entries = NULL;
    if (source->as.record.count != 0) {
      entries = (toad_script_record_entry_view_t *)mem_alloc_array(arena, sizeof(*entries),
                                                                    source->as.record.count);
      if (!entries) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->as.record.count; ++i) {
        entries[i].key = source->as.record.entries[i].key;
        entries[i].key.data = ts_host_copy_string(arena, source->as.record.entries[i].key);
        if (!entries[i].key.data) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
        {
          toad_script_status_t status =
              ts_host_copy_view(arena, &source->as.record.entries[i].value, &entries[i].value);
          if (status != TOAD_SCRIPT_STATUS_OK) return status;
        }
      }
    }
    copy.as.record.entries = entries;
  }
  *target = copy;
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t ts_host_build_toad_value(mem_pool_t *key_arena,
                                                        const toad_script_value_view_t *source,
                                                        toad_value_t *target) {
  toad_value_t value = ts_host_toad_null();
  toad_script_status_t status = TOAD_SCRIPT_STATUS_OK;

  switch (source->kind) {
  case TOAD_SCRIPT_VALUE_NULL:
    break;
  case TOAD_SCRIPT_VALUE_BOOL:
    value = toad_val_bool(source->as.boolean);
    break;
  case TOAD_SCRIPT_VALUE_INT64:
    value = toad_val_int(source->as.integer);
    break;
  case TOAD_SCRIPT_VALUE_NUMBER:
    value = toad_val_num(source->as.number);
    break;
  case TOAD_SCRIPT_VALUE_STRING:
    value = toad_val_str(vstr_from_buf(source->as.string.data, source->as.string.size));
    break;
  case TOAD_SCRIPT_VALUE_ARRAY:
    value = toad_val_list_empty();
    for (size_t i = 0; i < source->as.array.count; ++i) {
      toad_value_t child = ts_host_toad_null();
      status = ts_host_build_toad_value(key_arena, &source->as.array.items[i], &child);
      if (status == TOAD_SCRIPT_STATUS_OK && toad_list_push(&value, child) != 0)
        status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      toad_value_destroy(&child);
      if (status != TOAD_SCRIPT_STATUS_OK) break;
    }
    break;
  case TOAD_SCRIPT_VALUE_RECORD:
    value = toad_val_map();
    if (!value.data.map.htab) {
      status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      break;
    }
    for (size_t i = 0; i < source->as.record.count; ++i) {
      const toad_script_record_entry_view_t *entry = &source->as.record.entries[i];
      toad_value_t child = ts_host_toad_null();
      char *key = ts_host_copy_string(key_arena, entry->key);
      if (!key) {
        status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
        break;
      }
      status = ts_host_build_toad_value(key_arena, &entry->value, &child);
      if (status == TOAD_SCRIPT_STATUS_OK && toad_map_set(&value, key, child) != 0)
        status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      toad_value_destroy(&child);
      if (status != TOAD_SCRIPT_STATUS_OK) break;
    }
    break;
  default:
    status = TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
    break;
  }

  if (status != TOAD_SCRIPT_STATUS_OK) {
    toad_value_destroy(&value);
    return status;
  }
  *target = value;
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t ts_host_value_from_view(toad_script_ctx_t *target_ctx,
                                              const toad_script_value_view_t *value,
                                              const ts_host_value_limits_t *limits,
                                              toad_value_t *out_value) {
  toad_script_status_t status;
  if (!out_value) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_value = ts_host_toad_null();
  if (!target_ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_context_check_thread(target_ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  return ts_host_value_from_view_in_env(&target_ctx->env, value, limits, out_value);
}

toad_script_status_t ts_host_value_from_view_in_env(toad_env_t *target_env,
                                                     const toad_script_value_view_t *value,
                                                     const ts_host_value_limits_t *limits,
                                                     toad_value_t *out_value) {
  toad_value_t temporary = ts_host_toad_null();
  mem_pool_t key_arena;
  toad_script_status_t status;
  if (!out_value) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_value = ts_host_toad_null();
  if (!target_env) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;

  status = ts_host_validate_value_view(value, limits);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (mem_init(&key_arena, 0) != 0) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  status = ts_host_build_toad_value(&key_arena, value, &temporary);
  if (status == TOAD_SCRIPT_STATUS_OK &&
      toad_value_copy_to_env(temporary, target_env, out_value) != 0)
    status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (status != TOAD_SCRIPT_STATUS_OK) *out_value = ts_host_toad_null();
  toad_value_destroy(&temporary);
  mem_destroy(&key_arena);
  return status;
}

typedef struct ts_host_toad_view_state_s {
  mem_pool_t *arena;
  const ts_host_value_limits_t *limits;
  size_t nodes;
  size_t bytes;
} ts_host_toad_view_state_t;

static toad_script_status_t ts_host_toad_view_charge(ts_host_toad_view_state_t *state,
                                                        size_t count, size_t element_size) {
  size_t amount;
  if (count != 0 && element_size > SIZE_MAX / count) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  amount = count * element_size;
  if (state->bytes > state->limits->max_bytes || amount > state->limits->max_bytes - state->bytes)
    return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->bytes += amount;
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t ts_host_toad_to_view(ts_host_toad_view_state_t *state,
                                                    const toad_value_t *source,
                                                    toad_script_value_view_t *target,
                                                    size_t depth) {
  toad_script_status_t status;
  if (!state || !source || !target) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (depth > state->limits->max_depth || state->nodes == state->limits->max_nodes)
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
      return TOAD_SCRIPT_STATUS_INVALID_STATE;
    if (source->data.string.len == SIZE_MAX) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_toad_view_charge(state, source->data.string.len + 1U, 1U);
    if (status != TOAD_SCRIPT_STATUS_OK) return status;
    if (!vstr_utf8_valid(source->data.string)) return TOAD_SCRIPT_STATUS_INVALID_UTF8;
    target->kind = TOAD_SCRIPT_VALUE_STRING;
    target->as.string =
        (toad_script_string_view_t){source->data.string.data, source->data.string.len};
    return TOAD_SCRIPT_STATUS_OK;
  case TOAD_VAL_VECTOR: {
    toad_script_value_view_t *items = NULL;
    if (!source->data.vector.data && source->data.vector.size != 0)
      return TOAD_SCRIPT_STATUS_INVALID_STATE;
    if (source->data.vector.size > state->limits->max_nodes - state->nodes)
      return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_toad_view_charge(state, source->data.vector.size, sizeof(*items));
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
    target->as.array = (toad_script_array_view_t){items, source->data.vector.size};
    return TOAD_SCRIPT_STATUS_OK;
  }
  case TOAD_VAL_LIST: {
    toad_script_value_view_t *items = NULL;
    if (!source->data.list.items && source->data.list.count != 0)
      return TOAD_SCRIPT_STATUS_INVALID_STATE;
    if (source->data.list.count > state->limits->max_nodes - state->nodes)
      return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_toad_view_charge(state, source->data.list.count, sizeof(*items));
    if (status != TOAD_SCRIPT_STATUS_OK) return status;
    if (source->data.list.count != 0) {
      items = (toad_script_value_view_t *)mem_alloc_array(state->arena, sizeof(*items),
                                                           source->data.list.count);
      if (!items) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->data.list.count; ++i) {
        status = ts_host_toad_to_view(state, &source->data.list.items[i], &items[i], depth + 1U);
        if (status != TOAD_SCRIPT_STATUS_OK) return status;
      }
    }
    target->kind = TOAD_SCRIPT_VALUE_ARRAY;
    target->as.array = (toad_script_array_view_t){items, source->data.list.count};
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
    if (count > state->limits->max_nodes - state->nodes) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_toad_view_charge(state, count, sizeof(*entries));
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
      if (key_size == SIZE_MAX) return TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
      status = ts_host_toad_view_charge(state, key_size + 1U, 1U);
      if (status != TOAD_SCRIPT_STATUS_OK) return status;
      if (!vstr_utf8_valid(vstr_from_buf(key, key_size))) return TOAD_SCRIPT_STATUS_INVALID_UTF8;
      entries[index].key = (toad_script_string_view_t){key, key_size};
      status = ts_host_toad_to_view(state, &child, &entries[index].value, depth + 1U);
      if (status != TOAD_SCRIPT_STATUS_OK) return status;
      index++;
    }
    if (index != count) return TOAD_SCRIPT_STATUS_INVALID_STATE;
    target->kind = TOAD_SCRIPT_VALUE_RECORD;
    target->as.record = (toad_script_record_view_t){entries, count};
    return TOAD_SCRIPT_STATUS_OK;
  }
  default:
    return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  }
}

toad_script_status_t toad_script_result_create(toad_script_ctx_t *ctx,
                                                 toad_script_result_t **out_result) {
  toad_script_result_t *result;
  toad_script_status_t status;
  if (!out_result) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_result = NULL;
  if (!ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_context_check_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  result = (toad_script_result_t *)calloc(1, sizeof(*result));
  if (!result) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (mem_init(&result->view_arena, 0) != 0) {
    free(result);
    return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  }
  result->ctx = ctx;
  result->value = ts_host_null_view();
  ts_context_retain(ctx);
  *out_result = result;
  return TOAD_SCRIPT_STATUS_OK;
}

void toad_script_result_reset(toad_script_result_t *result) {
  toad_script_status_t status = ts_host_result_reset_checked(result);
  if (status == TOAD_SCRIPT_STATUS_WRONG_THREAD) ts_host_result_contract_violation();
}

void toad_script_result_destroy(toad_script_result_t *result) {
  toad_script_status_t status = ts_host_result_destroy_checked(result);
  if (status == TOAD_SCRIPT_STATUS_WRONG_THREAD) ts_host_result_contract_violation();
}

toad_script_status_t ts_host_result_reset_checked(toad_script_result_t *result) {
  toad_script_status_t status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  ts_host_result_clear_unchecked(result);
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t ts_host_result_destroy_checked(toad_script_result_t *result) {
  toad_script_ctx_t *ctx;
  toad_script_status_t status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  ctx = result->ctx;
  mem_destroy(&result->view_arena);
  memset(result, 0, sizeof(*result));
  free(result);
  ts_context_release(ctx);
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t toad_script_result_get_value(const toad_script_result_t *result,
                                                    toad_script_value_view_t *out_value) {
  toad_script_status_t status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!out_value) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (!result->has_value) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  *out_value = result->value;
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t toad_script_result_get_error(const toad_script_result_t *result,
                                                    toad_script_error_info_t *out_error) {
  toad_script_status_t status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!out_error) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (!result->has_error) return TOAD_SCRIPT_STATUS_INVALID_STATE;
  *out_error = result->error;
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t ts_host_result_store_view(toad_script_result_t *result,
                                                const toad_script_value_view_t *value,
                                                const ts_host_value_limits_t *limits) {
  toad_script_value_view_t copy = ts_host_null_view();
  toad_script_status_t status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  ts_host_result_clear_unchecked(result);
  status = ts_host_validate_value_view(value, limits);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_copy_view(&result->view_arena, value, &copy);
  if (status != TOAD_SCRIPT_STATUS_OK) {
    ts_host_result_clear_unchecked(result);
    return status;
  }
  result->value = copy;
  result->has_value = 1;
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t ts_host_result_store_engine(toad_script_result_t *result,
                                                  const toad_value_t *value,
                                                  const ts_host_value_limits_t *limits) {
  mem_pool_t temporary_arena;
  ts_host_toad_view_state_t state;
  toad_script_value_view_t view = ts_host_null_view();
  toad_script_status_t status;
  if (!result || !value || !limits) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  ts_host_result_clear_unchecked(result);
  if (mem_init(&temporary_arena, 0) != 0) return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  memset(&state, 0, sizeof(state));
  state.arena = &temporary_arena;
  state.limits = limits;
  status = ts_host_toad_to_view(&state, value, &view, 1U);
  if (status == TOAD_SCRIPT_STATUS_OK) status = ts_host_result_store_view(result, &view, limits);
  mem_destroy(&temporary_arena);
  if (status != TOAD_SCRIPT_STATUS_OK) ts_host_result_clear_unchecked(result);
  return status;
}

static toad_script_status_t ts_host_validate_error_info(const toad_script_error_info_t *error,
                                                         size_t max_bytes) {
  ts_host_value_limits_t limits = {1, 1, max_bytes};
  ts_host_validation_state_t state = {.limits = &limits};
  toad_script_status_t status;

  if (!error || error->struct_size < sizeof(*error) || max_bytes == 0 ||
      error->status < TOAD_SCRIPT_STATUS_INVALID_ARGUMENT ||
      error->status > TOAD_SCRIPT_STATUS_INTERRUPTED ||
      error->phase > TOAD_SCRIPT_ERROR_PHASE_INTERRUPT)
    return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  for (size_t i = 0; i < sizeof(error->reserved) / sizeof(error->reserved[0]); ++i) {
    if (error->reserved[i] != 0) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  }

  status = ts_host_validate_string(&state, error->module_name, 0, 0);
  if (status == TOAD_SCRIPT_STATUS_OK)
    status = ts_host_validate_string(&state, error->function_name, 0, 0);
  if (status == TOAD_SCRIPT_STATUS_OK)
    status = ts_host_validate_string(&state, error->message, 0, 0);
  return status;
}

toad_script_status_t ts_host_result_set_error(toad_script_result_t *result,
                                               const toad_script_error_info_t *error,
                                               size_t max_bytes) {
  toad_script_error_info_t copy;
  toad_script_status_t status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  ts_host_result_clear_unchecked(result);
  status = ts_host_validate_error_info(error, max_bytes);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;

  copy = *error;
  copy.module_name.data = ts_host_copy_string(&result->view_arena, error->module_name);
  copy.function_name.data = ts_host_copy_string(&result->view_arena, error->function_name);
  copy.message.data = ts_host_copy_string(&result->view_arena, error->message);
  if (!copy.module_name.data || !copy.function_name.data || !copy.message.data) {
    ts_host_result_clear_unchecked(result);
    return TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
  }
  result->error = copy;
  result->has_error = 1;
  return TOAD_SCRIPT_STATUS_OK;
}

toad_script_status_t ts_host_result_check_context(const toad_script_result_t *result,
                                                   const toad_script_ctx_t *ctx) {
  toad_script_status_t status = ts_host_result_check_thread(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!ctx) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (result->ctx != ctx) return TOAD_SCRIPT_STATUS_CONTEXT_MISMATCH;
  return TOAD_SCRIPT_STATUS_OK;
}
