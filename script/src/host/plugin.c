#include "../internal.h"
#include "internal.h"
#include "plugin_function.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct ts_host_plugin_registration_s {
  cmeta_plugin_registry *registry; /* borrowed; must outlive the context */
  cmeta_plugin_lease lease;
  size_t count;
  toad_plugin_function_binding_t bindings[];
};

static toad_script_status_t plugin_fail(toad_script_result_t *result, toad_script_status_t status,
                                        int cause, const char *message) {
  toad_script_error_info_t error = {.struct_size = sizeof(error),
                                    .status = status,
                                    .cause_code = cause,
                                    .phase = TOAD_SCRIPT_ERROR_PHASE_NONE,
                                    .message = {message, strlen(message)}};
  (void)ts_host_result_set_error(result, &error, 4096);
  return status;
}

static void release_registration(struct ts_host_plugin_registration_s *registration) {
  if (cmeta_plugin_lease_valid(registration->lease) &&
      cmeta_plugin_registry_release(registration->registry, &registration->lease) !=
          CMETA_PLUGIN_OK)
    abort(); /* Caller violated the registry/lease lifetime contract. */
  free(registration);
}

void ts_host_plugins_destroy(toad_script_ctx_t *ctx) {
  while (ctx->host_plugin_count) {
    size_t index = --ctx->host_plugin_count;
    release_registration(ctx->host_plugins[index]);
    ctx->host_plugins[index] = NULL;
  }
}

static int identifier(const char *name) {
  const unsigned char *p = (const unsigned char *)name;
  if (!p || !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) return 0;
  for (++p; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
          *p == '_'))
      return 0;
  return 1;
}

static toad_script_status_t callback_error(toad_script_host_result_builder_t *result,
                                           const char *message) {
  return toad_script_host_result_set_error(result, 0,
                                           (toad_script_string_view_t){message, strlen(message)});
}

static int scalar_argument(const toad_script_value_view_t *view, toad_plugin_scalar_kind_t kind,
                           toad_plugin_scalar_storage_t *storage, void **address) {
  toad_value_t value = {0};
  if (kind == TOAD_PLUGIN_SCALAR_BOOL) {
    if (view->kind != TOAD_SCRIPT_VALUE_BOOL) return 0;
    value.type = TOAD_VAL_BOOL;
    value.data.boolean = view->as.boolean;
  } else if (kind == TOAD_PLUGIN_SCALAR_INT || kind == TOAD_PLUGIN_SCALAR_LONG) {
    if (view->kind != TOAD_SCRIPT_VALUE_INT64) return 0;
    value.type = TOAD_VAL_INTEGER;
    value.data.integer = view->as.integer;
  } else {
    if (view->kind == TOAD_SCRIPT_VALUE_NUMBER) {
      value.type = TOAD_VAL_NUMBER;
      value.data.number = view->as.number;
    } else if (view->kind == TOAD_SCRIPT_VALUE_INT64 &&
               view->as.integer >= -INT64_C(9007199254740992) &&
               view->as.integer <= INT64_C(9007199254740992)) {
      value.type = TOAD_VAL_INTEGER;
      value.data.integer = view->as.integer;
    } else return 0;
  }
  return ts_plugin_scalar_from_engine(&value, kind, storage, address);
}

static toad_script_status_t invoke(void *opaque, const toad_script_value_view_t *args, size_t count,
                                   toad_script_host_result_builder_t *result) {
  const toad_plugin_function_binding_t *binding = opaque;
  toad_plugin_scalar_storage_t parameters[TOAD_PLUGIN_MAX_SCALAR_PARAMS] = {0}, returned = {0};
  void *addresses[TOAD_PLUGIN_MAX_SCALAR_PARAMS] = {0};
  toad_script_value_view_t value = {0};
  toad_value_t native;
  if (count != binding->param_count) return callback_error(result, "native module arity mismatch");
  for (size_t i = 0; i < count; ++i)
    if (!scalar_argument(&args[i], binding->param_kinds[i], &parameters[i], &addresses[i]))
      return callback_error(result,
                            "native module argument does not match its CMeta scalar contract");
  if (!binding->invoke(binding->context,
                       ts_plugin_scalar_return_storage(binding->return_kind, &returned),
                       count ? addresses : NULL, count))
    return callback_error(result, "native module exact adapter rejected invocation");
  native = ts_plugin_scalar_to_engine(binding->return_kind, &returned);
  switch (native.type) {
  case TOAD_VAL_NULL:
    value.kind = TOAD_SCRIPT_VALUE_NULL;
    break;
  case TOAD_VAL_BOOL:
    value.kind = TOAD_SCRIPT_VALUE_BOOL;
    value.as.boolean = native.data.boolean;
    break;
  case TOAD_VAL_INTEGER:
    value.kind = TOAD_SCRIPT_VALUE_INT64;
    value.as.integer = native.data.integer;
    break;
  case TOAD_VAL_NUMBER:
    if (!isfinite(native.data.number))
      return callback_error(result, "native module returned a non-finite number");
    value.kind = TOAD_SCRIPT_VALUE_NUMBER;
    value.as.number = native.data.number;
    break;
  default:
    return callback_error(result, "native module returned an unsupported scalar");
  }
  return toad_script_host_result_set_value(result, &value);
}

toad_script_status_t toad_script_context_register_plugin(toad_script_ctx_t *ctx,
                                                         cmeta_plugin_registry *registry,
                                                         cmeta_plugin_ref plugin,
                                                         toad_script_result_t *result) {
  const cmeta_plugin_manifest *manifest = NULL;
  struct ts_host_plugin_registration_s *registration;
  cmeta_plugin_lease lease = {0};
  cmeta_plugin_status native_status;
  size_t original, bytes;
  toad_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_check_context(result, ctx);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_reset_checked(result);
  if (status != TOAD_SCRIPT_STATUS_OK) return status;
  if (!registry)
    return plugin_fail(result, TOAD_SCRIPT_STATUS_INVALID_ARGUMENT, 0,
                       "plugin registry is required");
  if (atomic_load_explicit(&ctx->closing, memory_order_acquire))
    return plugin_fail(result, TOAD_SCRIPT_STATUS_INVALID_STATE, 0, "context is closing");
  if (ctx->active_host_modules)
    return plugin_fail(result, TOAD_SCRIPT_STATUS_INVALID_STATE, 0,
                       "host registry is frozen while modules are active");
  if (ctx->host_plugin_count == TS_MAX_PLUGINS)
    return plugin_fail(result, TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED, 0,
                       "attached plugin capacity exceeded");
  native_status = cmeta_plugin_registry_acquire(registry, plugin, &lease, &manifest);
  if (native_status != CMETA_PLUGIN_OK)
    return plugin_fail(result, TOAD_SCRIPT_STATUS_INVALID_STATE, (int)native_status,
                       cmeta_plugin_status_string(native_status));
  original = vec_size(&ctx->host_functions);
  status = TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  if (manifest->self || manifest->start || manifest->request_stop || manifest->is_quiescent ||
      manifest->destroy || !manifest->export_count)
    goto invalid_manifest;
  if (manifest->export_count > TS_HOST_REGISTRY_CAPACITY - original ||
      manifest->export_count >
          (SIZE_MAX - sizeof(*registration)) / sizeof(registration->bindings[0])) {
    status = TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED;
    goto invalid_manifest;
  }
  bytes = sizeof(*registration) + manifest->export_count * sizeof(registration->bindings[0]);
  registration = calloc(1, bytes);
  if (!registration) {
    status = TOAD_SCRIPT_STATUS_OUT_OF_MEMORY;
    goto invalid_manifest;
  }
  registration->registry = registry;
  registration->lease = lease;
  registration->count = manifest->export_count;
  for (size_t i = 0; i < registration->count; ++i) {
    const cmeta_plugin_export *entry = &manifest->exports[i];
    size_t slot;
    toad_script_string_view_t name;
    if (entry->kind != CMETA_PLUGIN_EXPORT_FUNCTION || !identifier(entry->export_id) ||
        cmeta_plugin_export_require_function(entry, entry->contract_id, entry->contract_version,
                                             0u) != CMETA_PLUGIN_OK ||
        !ts_plugin_function_scalar_binding(entry, &registration->bindings[i]))
      goto rollback;
    name = (toad_script_string_view_t){entry->export_id, strlen(entry->export_id)};
    status = ts_host_registry_find_slot(ctx, name, &slot);
    if (status == TOAD_SCRIPT_STATUS_OK) {
      status = TOAD_SCRIPT_STATUS_INVALID_STATE;
      goto rollback;
    }
    if (status != TOAD_SCRIPT_STATUS_NOT_FOUND) goto rollback;
    status = TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  }
  for (size_t i = 0; i < registration->count; ++i) {
    const toad_plugin_function_binding_t *binding = &registration->bindings[i];
    toad_script_host_function_descriptor_t descriptor = {0};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.name =
        (toad_script_string_view_t){binding->entry->export_id, strlen(binding->entry->export_id)};
    descriptor.min_arity = descriptor.max_arity = (uint32_t)binding->param_count;
    status = toad_script_context_register_host_function(ctx, &descriptor, invoke,
                                                        &registration->bindings[i], result);
    if (status != TOAD_SCRIPT_STATUS_OK) {
      ts_host_registry_truncate(ctx, original);
      release_registration(registration);
      return status;
    }
    ts_host_function_entry_t *row = vec_at(&ctx->host_functions, original + i);
    row->native_flags = TOAD_NATIVE_PRESERVE_VALUE_TYPES;
  }
  ctx->host_plugins[ctx->host_plugin_count++] = registration;
  return TOAD_SCRIPT_STATUS_OK;
rollback:
  release_registration(registration);
  return plugin_fail(
      result, status, 0,
      "plugin exports conflict or do not match the supported scalar Function contract");
invalid_manifest:
  if (cmeta_plugin_registry_release(registry, &lease) != CMETA_PLUGIN_OK) abort();
  return plugin_fail(result, status, 0,
                     "plugin must publish a bounded passive scalar Function manifest");
}
