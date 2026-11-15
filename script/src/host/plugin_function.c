#include "plugin_function.h"
#include "../../../engine/src/runtime_internal.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static toad_plugin_scalar_kind_t plugin_scalar_kind(const cmeta_type_desc *type) {
  if (!type) return TOAD_PLUGIN_SCALAR_UNSUPPORTED;
  if (cmeta_type_equal(type, &cmeta_type_void)) return TOAD_PLUGIN_SCALAR_VOID;
  if (cmeta_type_equal(type, &cmeta_type_bool)) return TOAD_PLUGIN_SCALAR_BOOL;
  if (cmeta_type_equal(type, &cmeta_type_int)) return TOAD_PLUGIN_SCALAR_INT;
  if (cmeta_type_equal(type, &cmeta_type_long)) return TOAD_PLUGIN_SCALAR_LONG;
  if (cmeta_type_equal(type, &cmeta_type_float)) return TOAD_PLUGIN_SCALAR_FLOAT;
  if (cmeta_type_equal(type, &cmeta_type_double)) return TOAD_PLUGIN_SCALAR_DOUBLE;
  return TOAD_PLUGIN_SCALAR_UNSUPPORTED;
}

int ts_plugin_function_scalar_binding(const cmeta_plugin_export *entry,
                                      toad_plugin_function_binding_t *binding) {
  const cmeta_function_desc *desc;
  const cmeta_function_abi_desc *abi;
  toad_plugin_scalar_kind_t return_kind;

  if (!entry || !binding || entry->kind != CMETA_PLUGIN_EXPORT_FUNCTION) return 0;

  desc = entry->value.function.desc;
  abi = entry->value.function.abi;
  if (!desc || !abi || !entry->value.function.invoke || !cmeta_function_desc_valid(desc) ||
      !cmeta_function_abi_desc_valid(abi) || desc->param_count != abi->param_count ||
      desc->param_count > TOAD_PLUGIN_MAX_SCALAR_PARAMS)
    return 0;

  return_kind = plugin_scalar_kind(desc->return_type);
  if (return_kind == TOAD_PLUGIN_SCALAR_UNSUPPORTED) return 0;
  if (return_kind == TOAD_PLUGIN_SCALAR_VOID) {
    if (abi->return_carrier != CMETA_ABI_VOID || desc->result_flags != CMETA_RESULT_UNKNOWN)
      return 0;
  } else {
    if (abi->return_carrier != CMETA_ABI_SCALAR || desc->result_flags != CMETA_RESULT_VALUE)
      return 0;
  }

  memset(binding, 0, sizeof(*binding));
  binding->entry = entry;
  binding->invoke = entry->value.function.invoke;
  binding->context = entry->value.function.context;
  binding->param_count = desc->param_count;
  binding->return_kind = return_kind;

  for (size_t i = 0u; i < desc->param_count; ++i) {
    const cmeta_param_desc *param = cmeta_function_param(desc, i);
    toad_plugin_scalar_kind_t kind;
    if (!param || param->flags != CMETA_PARAM_IN ||
        cmeta_function_param_abi(abi, i) != CMETA_ABI_SCALAR)
      return 0;
    kind = plugin_scalar_kind(param->type);
    if (kind == TOAD_PLUGIN_SCALAR_UNSUPPORTED || kind == TOAD_PLUGIN_SCALAR_VOID) return 0;
    binding->param_kinds[i] = kind;
  }
  return 1;
}

static int plugin_numeric_value(const toad_value_t *arg, double *out) {
  if (!arg || !out) return 0;
  if (arg->type == TOAD_VAL_NUMBER) {
    *out = arg->data.number;
    return 1;
  }
  if (arg->type == TOAD_VAL_INTEGER) {
    *out = (double)arg->data.integer;
    return 1;
  }
  if (arg->type == TOAD_VAL_BOOL) {
    *out = arg->data.boolean ? 1.0 : 0.0;
    return 1;
  }
  return 0;
}

int ts_plugin_scalar_from_engine(const toad_value_t *arg, toad_plugin_scalar_kind_t kind,
                                 toad_plugin_scalar_storage_t *storage, void **address) {
  double number;

  if (!arg || !storage || !address) return 0;

  switch (kind) {
  case TOAD_PLUGIN_SCALAR_BOOL:
    if (arg->type == TOAD_VAL_BOOL) {
      storage->boolean_value = arg->data.boolean != 0;
    } else if (arg->type == TOAD_VAL_INTEGER) {
      if (arg->data.integer != 0 && arg->data.integer != 1) return 0;
      storage->boolean_value = arg->data.integer != 0;
    } else if (arg->type == TOAD_VAL_NUMBER) {
      if (!isfinite(arg->data.number) || (arg->data.number != 0.0 && arg->data.number != 1.0))
        return 0;
      storage->boolean_value = arg->data.number != 0.0;
    } else {
      return 0;
    }
    *address = &storage->boolean_value;
    return 1;

  case TOAD_PLUGIN_SCALAR_INT:
    if (arg->type == TOAD_VAL_INTEGER) {
      if (arg->data.integer < (int64_t)INT_MIN || arg->data.integer > (int64_t)INT_MAX) return 0;
      storage->int_value = (int)arg->data.integer;
    } else {
      if (!plugin_numeric_value(arg, &number) || !isfinite(number) || number < (double)INT_MIN ||
          number >= (double)INT_MAX + 1.0 || trunc(number) != number)
        return 0;
      storage->int_value = (int)number;
    }
    *address = &storage->int_value;
    return 1;

  case TOAD_PLUGIN_SCALAR_LONG:
    if (arg->type == TOAD_VAL_INTEGER) {
#if LONG_MAX < INT64_MAX
      if (arg->data.integer < (int64_t)LONG_MIN || arg->data.integer > (int64_t)LONG_MAX) return 0;
#endif
      storage->long_value = (long)arg->data.integer;
    } else {
      if (!plugin_numeric_value(arg, &number) || !isfinite(number) || number < (double)LONG_MIN ||
          number >= (double)LONG_MAX + 1.0 || trunc(number) != number)
        return 0;
      storage->long_value = (long)number;
    }
    *address = &storage->long_value;
    return 1;

  case TOAD_PLUGIN_SCALAR_FLOAT:
    if (!plugin_numeric_value(arg, &number) || !isfinite(number) || number < -(double)FLT_MAX ||
        number > (double)FLT_MAX)
      return 0;
    storage->float_value = (float)number;
    *address = &storage->float_value;
    return 1;

  case TOAD_PLUGIN_SCALAR_DOUBLE:
    if (!plugin_numeric_value(arg, &number) || !isfinite(number)) return 0;
    storage->double_value = number;
    *address = &storage->double_value;
    return 1;

  default:
    return 0;
  }
}

void *ts_plugin_scalar_return_storage(toad_plugin_scalar_kind_t kind,
                                      toad_plugin_scalar_storage_t *storage) {
  if (!storage) return NULL;
  switch (kind) {
  case TOAD_PLUGIN_SCALAR_VOID:
    return NULL;
  case TOAD_PLUGIN_SCALAR_BOOL:
    return &storage->boolean_value;
  case TOAD_PLUGIN_SCALAR_INT:
    return &storage->int_value;
  case TOAD_PLUGIN_SCALAR_LONG:
    return &storage->long_value;
  case TOAD_PLUGIN_SCALAR_FLOAT:
    return &storage->float_value;
  case TOAD_PLUGIN_SCALAR_DOUBLE:
    return &storage->double_value;
  default:
    return NULL;
  }
}

toad_value_t ts_plugin_scalar_to_engine(toad_plugin_scalar_kind_t kind,
                                        const toad_plugin_scalar_storage_t *storage) {
  toad_value_t value = {0};
  if (!storage && kind != TOAD_PLUGIN_SCALAR_VOID) return toad_val_num(0.0);

  switch (kind) {
  case TOAD_PLUGIN_SCALAR_VOID:
    value.type = TOAD_VAL_NULL;
    return value;
  case TOAD_PLUGIN_SCALAR_BOOL:
    return toad_val_bool(storage->boolean_value);
  case TOAD_PLUGIN_SCALAR_INT:
    return toad_val_int((int64_t)storage->int_value);
  case TOAD_PLUGIN_SCALAR_LONG:
    return toad_val_int((int64_t)storage->long_value);
  case TOAD_PLUGIN_SCALAR_FLOAT:
    return toad_val_num((double)storage->float_value);
  case TOAD_PLUGIN_SCALAR_DOUBLE:
    return toad_val_num(storage->double_value);
  default:
    return toad_val_num(0.0);
  }
}

toad_value_t ts_plugin_function_scalar_call(size_t argc, toad_value_t *args, toad_env_t *env,
                                            void *user_data) {
  toad_plugin_function_binding_t *binding = (toad_plugin_function_binding_t *)user_data;
  toad_plugin_scalar_storage_t params[TOAD_PLUGIN_MAX_SCALAR_PARAMS];
  void *param_addresses[TOAD_PLUGIN_MAX_SCALAR_PARAMS];
  toad_plugin_scalar_storage_t result_storage;
  void *result_address;

  memset(params, 0, sizeof(params));
  memset(param_addresses, 0, sizeof(param_addresses));
  memset(&result_storage, 0, sizeof(result_storage));

  if (!binding || !binding->invoke || argc != binding->param_count || (argc > 0u && !args)) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "invalid canonical plugin function invocation");
    }
    return toad_val_num(0.0);
  }

  for (size_t i = 0u; i < argc; ++i) {
    if (!ts_plugin_scalar_from_engine(&args[i], binding->param_kinds[i], &params[i],
                                      &param_addresses[i])) {
      if (env) {
        env->aborted = 1;
        snprintf(env->error_msg, sizeof(env->error_msg),
                 "plugin function '%s' argument %zu does not match canonical scalar type",
                 binding->entry && binding->entry->export_id ? binding->entry->export_id
                                                             : "<unknown>",
                 i);
      }
      return toad_val_num(0.0);
    }
  }

  result_address = ts_plugin_scalar_return_storage(binding->return_kind, &result_storage);
  if (binding->return_kind != TOAD_PLUGIN_SCALAR_VOID && !result_address) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "plugin function '%s' has an unsupported return type",
               binding->entry && binding->entry->export_id ? binding->entry->export_id
                                                           : "<unknown>");
    }
    return toad_val_num(0.0);
  }

  if (!binding->invoke(binding->context, result_address, argc > 0u ? param_addresses : NULL,
                       argc)) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "plugin function '%s' exact adapter rejected invocation",
               binding->entry && binding->entry->export_id ? binding->entry->export_id
                                                           : "<unknown>");
    }
    return toad_val_num(0.0);
  }

  return ts_plugin_scalar_to_engine(binding->return_kind, &result_storage);
}
