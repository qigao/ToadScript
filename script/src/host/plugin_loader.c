/**
 * @file plugin_loader.c
 * @brief Toad host adapter over the canonical Salts plugin registry.
 */
#include <toad/plugin_loader.h>
#include "plugin_databind.h"
#include "plugin_function.h"
#include "../../../engine/src/runtime_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int plugin_error_set(toad_plugin_error_t *error, toad_plugin_error_code_t code,
                            toad_plugin_error_stage_t stage, uint32_t native_code,
                            const char *path, const char *format, ...);

static int plugin_bind_function_exports(
    toad_plugin_handle_t *handle, toad_env_t *env,
    toad_plugin_error_t *error) {
  toad_plugin_function_binding_t *bindings = NULL;
  toad_native_registration_t *registrations = NULL;
  size_t function_count = 0u;
  size_t registration_index = 0u;
  size_t scalar_count = 0u;
  toad_registration_status_t registration_status;

  if (!handle || !handle->manifest || !env)
    return plugin_error_set(error, TOAD_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TOAD_PLUGIN_STAGE_ARGUMENT, 0u, NULL,
                            "plugin function binding requires a manifest and environment");

  for (size_t i = 0u; i < handle->manifest->export_count; ++i) {
    if (handle->manifest->exports[i].kind == CMETA_PLUGIN_EXPORT_FUNCTION)
      ++function_count;
  }
  if (function_count == 0u) return TOAD_PLUGIN_ERROR_NONE;

  bindings = (toad_plugin_function_binding_t *)calloc(
      function_count, sizeof(*bindings));
  registrations = (toad_native_registration_t *)calloc(
      function_count, sizeof(*registrations));
  if (!bindings || !registrations) {
    free(registrations);
    free(bindings);
    return plugin_error_set(error, TOAD_PLUGIN_ERROR_OUT_OF_MEMORY,
                            TOAD_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                            "out of memory binding canonical plugin functions");
  }

  for (size_t i = 0u; i < handle->manifest->export_count; ++i) {
    const cmeta_plugin_export *entry = &handle->manifest->exports[i];
    void *databind_binding;
    cmeta_plugin_status status;
    if (entry->kind != CMETA_PLUGIN_EXPORT_FUNCTION) continue;

    status = cmeta_plugin_export_require_function(
        entry, entry->contract_id, entry->contract_version, 0u);
    if (status != CMETA_PLUGIN_OK) {
      free(registrations);
      free(bindings);
      return plugin_error_set(
          error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_INITIALIZE,
          (uint32_t)status, NULL,
          "plugin function '%s' failed canonical Function validation",
          entry->export_id ? entry->export_id : "<unnamed>");
    }

    databind_binding =
        toad_plugin_databind_find(handle, entry->export_id);
    if (databind_binding != NULL) {
      registrations[registration_index].name = entry->export_id;
      registrations[registration_index].fn = toad_plugin_databind_call;
      registrations[registration_index].user_data = databind_binding;
      registrations[registration_index].flags =
          TOAD_NATIVE_PRESERVE_VALUE_TYPES;
      ++registration_index;
      continue;
    }

    if (!ts_plugin_function_scalar_binding(
            entry, &bindings[scalar_count])) {
      free(registrations);
      free(bindings);
      return plugin_error_set(
          error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_INITIALIZE,
          (uint32_t)status, NULL,
          "plugin function '%s' is neither catalog-backed nor in the canonical finite scalar ABI",
          entry->export_id ? entry->export_id : "<unnamed>");
    }

    registrations[registration_index].name = entry->export_id;
    registrations[registration_index].fn = ts_plugin_function_scalar_call;
    registrations[registration_index].user_data = &bindings[scalar_count];
    registrations[registration_index].flags =
        TOAD_NATIVE_PRESERVE_VALUE_TYPES;
    ++scalar_count;
    ++registration_index;
  }

  if (registration_index != function_count) {
    free(registrations);
    free(bindings);
    return plugin_error_set(
        error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_INITIALIZE,
        0u, NULL, "plugin Function registration count is inconsistent");
  }

  registration_status = toad_env_register_funcs_checked(
      env, registrations, registration_index);
  free(registrations);
  if (registration_status != TOAD_REGISTRATION_OK) {
    free(bindings);
    return plugin_error_set(
        error,
        registration_status == TOAD_REGISTRATION_OUT_OF_MEMORY
            ? TOAD_PLUGIN_ERROR_OUT_OF_MEMORY
            : TOAD_PLUGIN_ERROR_DESCRIPTOR,
        TOAD_PLUGIN_STAGE_INITIALIZE, (uint32_t)registration_status, NULL,
        registration_status == TOAD_REGISTRATION_CONFLICT
            ? "plugin function export conflicts with an existing script binding"
            : "failed to register canonical plugin function bindings");
  }

  if (scalar_count == 0u) {
    free(bindings);
    bindings = NULL;
  }

  handle->function_bindings = bindings;
  handle->function_binding_count = scalar_count;
  return TOAD_PLUGIN_ERROR_NONE;
}

static void plugin_error_clear(toad_plugin_error_t *error) {
  if (error) memset(error, 0, sizeof(*error));
}

static int plugin_error_set(toad_plugin_error_t *error, toad_plugin_error_code_t code,
                            toad_plugin_error_stage_t stage, uint32_t native_code,
                            const char *path, const char *format, ...) {
  if (error) {
    va_list args;
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->stage = stage;
    error->native_code = native_code;
    if (path)
      snprintf(error->attempted_path, sizeof(error->attempted_path), "%s", path);
    if (format) {
      va_start(args, format);
      vsnprintf(error->message, sizeof(error->message), format, args);
      va_end(args);
    }
  }
  return code;
}

static char *plugin_strdup(const char *text) {
  size_t length;
  char *copy;
  if (!text) return NULL;
  length = strlen(text);
  if (length == SIZE_MAX) return NULL;
  copy = (char *)malloc(length + 1u);
  if (!copy) return NULL;
  memcpy(copy, text, length + 1u);
  return copy;
}

static char *plugin_join_path(const char *directory, const char *leaf) {
  size_t directory_len;
  size_t leaf_len;
  char separator =
#if defined(_WIN32)
      '\\';
#else
      '/';
#endif
  char *path;

  if (!directory || !leaf) return NULL;
  directory_len = strlen(directory);
  leaf_len = strlen(leaf);
  if (directory_len > SIZE_MAX - leaf_len - 2u) return NULL;
  path = (char *)malloc(directory_len + leaf_len + 2u);
  if (!path) return NULL;
  memcpy(path, directory, directory_len);
  path[directory_len] = separator;
  memcpy(path + directory_len + 1u, leaf, leaf_len + 1u);
  return path;
}

#if defined(_WIN32)
#include <windows.h>
/* Windows RPC's COM shorthand conflicts with CMeta's interface export field. */
#undef interface

static int plugin_has_path_separator(const char *path) {
  return path && (strchr(path, '\\') || strchr(path, '/') || strchr(path, ':'));
}

static wchar_t *plugin_utf8_to_wide(const char *text) {
  int length;
  wchar_t *wide;
  if (!text || !*text) return NULL;
  length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (length <= 0) return NULL;
  wide = (wchar_t *)malloc((size_t)length * sizeof(*wide));
  if (!wide) return NULL;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
    free(wide);
    return NULL;
  }
  return wide;
}

static char *plugin_wide_to_utf8(const wchar_t *wide) {
  int length;
  char *text;
  if (!wide || !*wide) return NULL;
  length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (length <= 0) return NULL;
  text = (char *)malloc((size_t)length);
  if (!text) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, length, NULL, NULL) <= 0) {
    free(text);
    return NULL;
  }
  return text;
}

static char *plugin_executable_dir(void) {
  DWORD capacity = 256u;
  wchar_t *buffer = NULL;

  for (;;) {
    DWORD length;
    wchar_t *slash;
    wchar_t *grown = (wchar_t *)realloc(buffer, (size_t)capacity * sizeof(*buffer));
    if (!grown) {
      free(buffer);
      return NULL;
    }
    buffer = grown;
    SetLastError(ERROR_SUCCESS);
    length = GetModuleFileNameW(NULL, buffer, capacity);
    if (length == 0u) {
      free(buffer);
      return NULL;
    }
    if (length < capacity && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      char *result;
      slash = wcsrchr(buffer, L'\\');
      if (!slash) slash = wcsrchr(buffer, L'/');
      if (!slash) {
        free(buffer);
        return NULL;
      }
      *slash = L'\0';
      result = plugin_wide_to_utf8(buffer);
      free(buffer);
      return result;
    }
    if (capacity > UINT32_MAX / 2u) {
      free(buffer);
      return NULL;
    }
    capacity *= 2u;
  }
}

static int plugin_path_exists(const char *path) {
  DWORD attrs;
  wchar_t *wide = plugin_utf8_to_wide(path);
  if (!wide) return 0;
  attrs = GetFileAttributesW(wide);
  free(wide);
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

#else

#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static int plugin_has_path_separator(const char *path) {
  return path && strchr(path, '/');
}

static char *plugin_executable_path(void) {
#if defined(__APPLE__)
  uint32_t size = 0u;
  char *path;
  if (_NSGetExecutablePath(NULL, &size) == 0 || size == 0u) return NULL;
  path = (char *)malloc(size);
  if (!path) return NULL;
  if (_NSGetExecutablePath(path, &size) != 0) {
    free(path);
    return NULL;
  }
  return path;
#elif defined(__linux__)
  size_t capacity = 256u;
  char *path = NULL;
  for (;;) {
    ssize_t length;
    char *grown = (char *)realloc(path, capacity);
    if (!grown) {
      free(path);
      return NULL;
    }
    path = grown;
    length = readlink("/proc/self/exe", path, capacity - 1u);
    if (length < 0) {
      free(path);
      return NULL;
    }
    if ((size_t)length < capacity - 1u) {
      path[length] = '\0';
      return path;
    }
    if (capacity > SIZE_MAX / 2u) {
      free(path);
      return NULL;
    }
    capacity *= 2u;
  }
#else
  return NULL;
#endif
}

static char *plugin_executable_dir(void) {
  char *path = plugin_executable_path();
  char *slash;
  if (!path) return NULL;
  slash = strrchr(path, '/');
  if (!slash) {
    free(path);
    return NULL;
  }
  *slash = '\0';
  return path;
}

static int plugin_path_exists(const char *path) {
  return path && access(path, R_OK) == 0;
}
#endif

static char *plugin_resolve_path(const char *path, toad_plugin_error_t *error) {
  char *exe_dir;
  char *plugins_dir;
  char *candidate;
  const char *root;

  if (!path || !*path) {
    plugin_error_set(error, TOAD_PLUGIN_ERROR_INVALID_ARGUMENT,
                     TOAD_PLUGIN_STAGE_ARGUMENT, 0u, path,
                     "plugin path is required");
    return NULL;
  }

  if (plugin_has_path_separator(path)) {
    if (plugin_path_exists(path)) return plugin_strdup(path);
    plugin_error_set(error, TOAD_PLUGIN_ERROR_OPEN, TOAD_PLUGIN_STAGE_OPEN, 0u,
                     path, "plugin file does not exist");
    return NULL;
  }

  exe_dir = plugin_executable_dir();
  if (!exe_dir) {
    plugin_error_set(error, TOAD_PLUGIN_ERROR_OPEN, TOAD_PLUGIN_STAGE_OPEN, 0u,
                     path, "failed to determine executable directory");
    return NULL;
  }

  plugins_dir = plugin_join_path(exe_dir, "plugins");
  candidate = plugins_dir ? plugin_join_path(plugins_dir, path) : NULL;
  free(plugins_dir);
  if (candidate && plugin_path_exists(candidate)) {
    free(exe_dir);
    return candidate;
  }
  free(candidate);

  root = getenv("TOAD_ROOT");
  if (root && *root) {
    char *bin_dir = plugin_join_path(root, "bin");
    plugins_dir = bin_dir ? plugin_join_path(bin_dir, "plugins") : NULL;
    free(bin_dir);
    candidate = plugins_dir ? plugin_join_path(plugins_dir, path) : NULL;
    free(plugins_dir);
    if (candidate && plugin_path_exists(candidate)) {
      free(exe_dir);
      return candidate;
    }
    free(candidate);
  }

  candidate = plugin_join_path(exe_dir, path);
  free(exe_dir);
  if (candidate && plugin_path_exists(candidate)) return candidate;

  plugin_error_set(error, TOAD_PLUGIN_ERROR_OPEN, TOAD_PLUGIN_STAGE_OPEN, 0u,
                   candidate ? candidate : path,
                   "plugin was not found in executable/package plugin locations");
  free(candidate);
  return NULL;
}

static int plugin_error_from_salts(toad_plugin_error_t *error,
                                   cmeta_plugin_status status,
                                   const char *path) {
  toad_plugin_error_code_t code = TOAD_PLUGIN_ERROR_DESCRIPTOR;
  toad_plugin_error_stage_t stage = TOAD_PLUGIN_STAGE_ABI;
  const char *detail = cmeta_plugin_status_string(status);

  switch (status) {
    case CMETA_PLUGIN_LOAD_FAILED:
      code = TOAD_PLUGIN_ERROR_OPEN;
      stage = TOAD_PLUGIN_STAGE_OPEN;
      break;
    case CMETA_PLUGIN_QUERY_MISSING:
      code = TOAD_PLUGIN_ERROR_SYMBOL;
      stage = TOAD_PLUGIN_STAGE_SYMBOL;
      break;
    case CMETA_PLUGIN_QUERY_REJECTED:
    case CMETA_PLUGIN_UNSUPPORTED_ABI:
      code = TOAD_PLUGIN_ERROR_ABI;
      stage = TOAD_PLUGIN_STAGE_ABI;
      break;
    case CMETA_PLUGIN_ALLOCATION_FAILED:
      code = TOAD_PLUGIN_ERROR_OUT_OF_MEMORY;
      stage = TOAD_PLUGIN_STAGE_OPEN;
      break;
    default:
      code = TOAD_PLUGIN_ERROR_DESCRIPTOR;
      stage = TOAD_PLUGIN_STAGE_ABI;
      break;
  }

  if (status == CMETA_PLUGIN_QUERY_MISSING) {
    return plugin_error_set(error, code, stage, (uint32_t)status, path,
                            "required symbol '%s' was not found",
                            CMETA_PLUGIN_QUERY_SYMBOL);
  }
  if (status == CMETA_PLUGIN_UNSUPPORTED_ABI ||
      status == CMETA_PLUGIN_QUERY_REJECTED) {
    return plugin_error_set(error, code, stage, (uint32_t)status, path,
                            "plugin ABI mismatch: %s", detail);
  }
  return plugin_error_set(error, code, stage, (uint32_t)status, path,
                          "Salts plugin admission failed: %s", detail);
}

static void plugin_registry_cleanup_loaded(cmeta_plugin_registry *registry,
                                           cmeta_plugin_ref ref,
                                           cmeta_plugin_lease *lease,
                                           int started) {
  bool quiescent = false;

  if (!registry || !registry->impl) return;
  if (lease && cmeta_plugin_lease_valid(*lease))
    (void)cmeta_plugin_registry_release(registry, lease);

  if (cmeta_plugin_ref_valid(ref)) {
    if (started) {
      cmeta_plugin_status status =
          cmeta_plugin_registry_request_stop(registry, ref);
      if (status == CMETA_PLUGIN_OK || status == CMETA_PLUGIN_ALREADY)
        (void)cmeta_plugin_registry_poll_quiescent(
            registry, ref, &quiescent);
      if (quiescent)
        (void)cmeta_plugin_registry_unload(registry, ref);
    } else {
      (void)cmeta_plugin_registry_unload(registry, ref);
    }
  }
  (void)cmeta_plugin_registry_destroy(registry);
}

int toad_plugin_load_ex(const char *path, const char *expected_name,
                      toad_plugin_handle_t **out, toad_plugin_error_t *error) {
  char *resolved = NULL;
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_registry_config config = {.capacity = 1u};
  cmeta_plugin_ref ref = {0};
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  const cmeta_plugin_export *module_export = NULL;
  toad_plugin_module *module = NULL;
  cmeta_plugin_status status;
  toad_plugin_handle_t *handle = NULL;
  int started = 0;

  plugin_error_clear(error);
  if (out) *out = NULL;
  if (!path || !*path || !out)
    return plugin_error_set(error, TOAD_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TOAD_PLUGIN_STAGE_ARGUMENT, 0u, path,
                            "plugin path and output handle are required");

  resolved = plugin_resolve_path(path, error);
  if (!resolved) return error ? error->code : TOAD_PLUGIN_ERROR_OPEN;

  status = cmeta_plugin_registry_init(&registry, &config);
  if (status != CMETA_PLUGIN_OK) {
    free(resolved);
    return plugin_error_from_salts(error, status, path);
  }

  status = cmeta_plugin_registry_load(&registry, resolved, &ref);
  if (status != CMETA_PLUGIN_OK) {
    int result = plugin_error_from_salts(error, status, resolved);
    free(resolved);
    (void)cmeta_plugin_registry_destroy(&registry);
    return result;
  }

  status = cmeta_plugin_registry_start(&registry, ref);
  if (status != CMETA_PLUGIN_OK) {
    int result = plugin_error_from_salts(error, status, resolved);
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, 0);
    return result;
  }
  started = 1;

  status = cmeta_plugin_registry_acquire(&registry, ref, &lease, &manifest);
  if (status != CMETA_PLUGIN_OK) {
    int result = plugin_error_from_salts(error, status, resolved);
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return result;
  }

  if (manifest->self != NULL || manifest->start != NULL ||
      manifest->request_stop != NULL || manifest->is_quiescent != NULL ||
      manifest->destroy != NULL) {
    plugin_error_set(error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_ABI,
                     0u, resolved,
                     "Toad module plugins must use passive Salts DSO lifecycle");
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return TOAD_PLUGIN_ERROR_DESCRIPTOR;
  }

  if (expected_name && strcmp(manifest->plugin_id, expected_name) != 0) {
    plugin_error_set(error, TOAD_PLUGIN_ERROR_NAME, TOAD_PLUGIN_STAGE_ABI, 0u,
                     resolved,
                     "plugin name mismatch: expected '%s', got '%s'",
                     expected_name, manifest->plugin_id);
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return TOAD_PLUGIN_ERROR_NAME;
  }

  status = cmeta_plugin_manifest_find_export(
      manifest, TOAD_PLUGIN_MODULE_EXPORT_ID, &module_export);
  if (status == CMETA_PLUGIN_OK) {
    if (cmeta_plugin_export_require_interface(
            module_export, TOAD_PLUGIN_MODULE_CONTRACT_ID,
            TOAD_PLUGIN_MODULE_CONTRACT_VERSION, 0u,
            toad_plugin_module_interface()) != CMETA_PLUGIN_OK) {
      plugin_error_set(error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_ABI,
                       (uint32_t)status, resolved,
                       "Toad module interface export is invalid");
      free(resolved);
      plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
      return TOAD_PLUGIN_ERROR_DESCRIPTOR;
    }

    module = (toad_plugin_module *)module_export->value.interface.value;
    if (!toad_plugin_module_valid(module)) {
      plugin_error_set(error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_ABI,
                       0u, resolved,
                       "Toad module interface value is invalid");
      free(resolved);
      plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
      return TOAD_PLUGIN_ERROR_DESCRIPTOR;
    }
  } else if (status == CMETA_PLUGIN_UNKNOWN_EXPORT) {
    size_t function_count = 0u;
    module_export = NULL;
    for (size_t i = 0u; i < manifest->export_count; ++i) {
      if (manifest->exports[i].kind == CMETA_PLUGIN_EXPORT_FUNCTION)
        ++function_count;
    }
    if (function_count == 0u) {
      plugin_error_set(
          error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_ABI,
          (uint32_t)status, resolved,
          "plugin publishes neither canonical Function exports nor the Toad module interface");
      free(resolved);
      plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
      return TOAD_PLUGIN_ERROR_DESCRIPTOR;
    }
  } else {
    plugin_error_set(error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_ABI,
                     (uint32_t)status, resolved,
                     "failed to inspect canonical plugin exports");
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return TOAD_PLUGIN_ERROR_DESCRIPTOR;
  }

  handle = (toad_plugin_handle_t *)calloc(1, sizeof(*handle));
  if (!handle) {
    free(resolved);
    plugin_registry_cleanup_loaded(&registry, ref, &lease, started);
    return plugin_error_set(error, TOAD_PLUGIN_ERROR_OUT_OF_MEMORY,
                            TOAD_PLUGIN_STAGE_OPEN, 0u, path,
                            "out of memory while creating plugin handle");
  }

  handle->registry = registry;
  handle->plugin_ref = ref;
  handle->lease = lease;
  handle->manifest = manifest;
  handle->module_export = module_export;
  handle->module = module;
  handle->instance = NULL;
  handle->function_bindings = NULL;
  handle->function_binding_count = 0u;
  handle->databind_bindings = NULL;
  handle->databind_binding_count = 0u;
  handle->databind_codec = NULL;
  handle->initialized = 0;
  registry.impl = NULL;
  free(resolved);

  *out = handle;
  plugin_error_clear(error);
  return TOAD_PLUGIN_ERROR_NONE;
}

int toad_plugin_find_bound_function(
    const toad_plugin_handle_t *handle, const char *export_id,
    toad_plugin_function_view_t *out) {
  const toad_plugin_function_binding_t *bindings;

  if (out) memset(out, 0, sizeof(*out));
  if (!handle || !export_id || !*export_id || !out ||
      !handle->function_bindings || handle->function_binding_count == 0u)
    return 0;

  bindings =
      (const toad_plugin_function_binding_t *)handle->function_bindings;
  for (size_t i = 0u; i < handle->function_binding_count; ++i) {
    const cmeta_plugin_export *entry = bindings[i].entry;
    if (entry && entry->export_id &&
        strcmp(entry->export_id, export_id) == 0) {
      out->entry = entry;
      out->invoke = bindings[i].invoke;
      out->context = bindings[i].context;
      return 1;
    }
  }
  return 0;
}

toad_plugin_handle_t *toad_plugin_load(const char *path) {
  toad_plugin_handle_t *handle = NULL;
  if (toad_plugin_load_ex(path, NULL, &handle, NULL) != TOAD_PLUGIN_ERROR_NONE)
    return NULL;
  return handle;
}

int toad_plugin_init_ex(toad_plugin_handle_t *handle, void *env, void *scratch,
                      toad_plugin_error_t *error) {
  int bind_status;

  plugin_error_clear(error);
  if (!handle || !handle->manifest || !env)
    return plugin_error_set(error, TOAD_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TOAD_PLUGIN_STAGE_ARGUMENT, 0u, NULL,
                            "validated plugin handle and target environment are required");
  if (handle->initialized)
    return plugin_error_set(error, TOAD_PLUGIN_ERROR_INVALID_ARGUMENT,
                            TOAD_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                            "plugin is already initialized");

  if (handle->module && toad_plugin_module_valid(handle->module)) {
    handle->instance = toad_plugin_module_load(handle->module, env, scratch);
    if (!handle->instance)
      return plugin_error_set(error, TOAD_PLUGIN_ERROR_INITIALIZE,
                              TOAD_PLUGIN_STAGE_INITIALIZE, 0u, NULL,
                              "plugin module initialization returned no instance");
  }

  {
    char databind_error[TOAD_PLUGIN_ERROR_MESSAGE_CAPACITY] = {0};
    if (!toad_plugin_databind_prepare(
            handle, databind_error, sizeof(databind_error))) {
      if (handle->instance && handle->module &&
          toad_plugin_module_valid(handle->module)) {
        toad_plugin_module_unload(handle->module, handle->instance);
        handle->instance = NULL;
      }
      return plugin_error_set(
          error, TOAD_PLUGIN_ERROR_DESCRIPTOR, TOAD_PLUGIN_STAGE_INITIALIZE,
          0u, NULL, "%s",
          databind_error[0]
              ? databind_error
              : "DataBind Service catalog binding failed");
    }
  }

  bind_status = plugin_bind_function_exports(
      handle, (toad_env_t *)env, error);
  if (bind_status != TOAD_PLUGIN_ERROR_NONE) {
    toad_plugin_databind_clear(handle);
    if (handle->instance && handle->module &&
        toad_plugin_module_valid(handle->module)) {
      toad_plugin_module_unload(handle->module, handle->instance);
      handle->instance = NULL;
    }
    return bind_status;
  }

  handle->initialized = 1;
  return TOAD_PLUGIN_ERROR_NONE;
}

int toad_plugin_init(toad_plugin_handle_t *handle, void *env, void *scratch) {
  return toad_plugin_init_ex(handle, env, scratch, NULL) ==
                 TOAD_PLUGIN_ERROR_NONE
             ? 0
             : -1;
}

void toad_plugin_unload(toad_plugin_handle_t *handle) {
  bool quiescent = false;

  if (!handle) return;

  if (handle->instance && handle->module &&
      toad_plugin_module_valid(handle->module)) {
    toad_plugin_module_unload(handle->module, handle->instance);
    handle->instance = NULL;
  }

  toad_plugin_databind_clear(handle);
  free(handle->function_bindings);
  handle->function_bindings = NULL;
  handle->function_binding_count = 0u;
  handle->initialized = 0;

  if (cmeta_plugin_lease_valid(handle->lease))
    (void)cmeta_plugin_registry_release(&handle->registry, &handle->lease);

  if (cmeta_plugin_ref_valid(handle->plugin_ref)) {
    cmeta_plugin_status status =
        cmeta_plugin_registry_request_stop(&handle->registry,
                                           handle->plugin_ref);
    if (status == CMETA_PLUGIN_OK || status == CMETA_PLUGIN_ALREADY)
      (void)cmeta_plugin_registry_poll_quiescent(
          &handle->registry, handle->plugin_ref, &quiescent);
    if (quiescent)
      (void)cmeta_plugin_registry_unload(&handle->registry,
                                         handle->plugin_ref);
  }

  (void)cmeta_plugin_registry_destroy(&handle->registry);
  free(handle);
}
