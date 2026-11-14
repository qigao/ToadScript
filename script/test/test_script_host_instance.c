#include "tinytest.h"

#include "../src/internal.h"
#include "../src/host/api_internal.h"
#include "../src/host/internal.h"

#include <stdint.h>
#include <string.h>

static toad_script_status_t compile_text(toad_script_ctx_t *ctx,
                                           toad_script_result_t *result,
                                           const char *source,
                                           toad_script_module_t **out_module) {
  toad_script_module_options_t options;
  toad_script_module_options_init(&options);
  options.module_name = (toad_script_string_view_t){"instance-test", 13};
  return toad_script_module_compile(
      ctx, (toad_script_string_view_t){source, strlen(source)}, &options,
      result, out_module);
}

static toad_script_status_t create_instance(
    toad_script_module_t *module, toad_script_execution_mode_t mode,
    toad_script_result_t *result, toad_script_instance_t **out_instance) {
  toad_script_instance_options_t options;
  toad_script_instance_options_init(&options);
  options.mode = mode;
  return toad_script_instance_create(module, &options, result, out_instance);
}

static toad_script_status_t resolve_export(
    toad_script_instance_t *instance, const char *name, size_t name_size,
    toad_script_result_t *result, toad_script_export_handle_t *out_handle) {
  return toad_script_instance_resolve_export(
      instance, (toad_script_string_view_t){name, name_size}, result,
      out_handle);
}

static void destroy_instance(toad_script_instance_t *instance,
                             toad_script_result_t *result) {
  if (instance)
    check_equal(toad_script_instance_destroy(instance, result),
                TOAD_SCRIPT_STATUS_OK);
}

static void destroy_module(toad_script_module_t *module,
                           toad_script_result_t *result) {
  if (module)
    check_equal(toad_script_module_destroy(module, result),
                TOAD_SCRIPT_STATUS_OK);
}

typedef struct destroy_during_call_probe_s {
  toad_script_instance_t *instance;
  toad_script_result_t *result;
  toad_script_status_t destroy_status;
  size_t calls;
} destroy_during_call_probe_t;

static toad_script_status_t destroy_during_call_host(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  destroy_during_call_probe_t *probe =
      (destroy_during_call_probe_t *)user_data;
  toad_script_value_view_t value = {.kind = TOAD_SCRIPT_VALUE_NUMBER};
  (void)args;
  (void)arg_count;
  probe->calls++;
  probe->destroy_status =
      toad_script_instance_destroy(probe->instance, probe->result);
  value.as.number = 17.0;
  return toad_script_host_result_set_value(builder, &value);
}

typedef struct destroy_module_during_init_probe_s {
  toad_script_module_t *module;
  toad_script_result_t *result;
  toad_script_status_t destroy_status;
  size_t calls;
} destroy_module_during_init_probe_t;

static toad_script_status_t destroy_module_during_init_host(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  destroy_module_during_init_probe_t *probe =
      (destroy_module_during_init_probe_t *)user_data;
  toad_script_value_view_t value = {.kind = TOAD_SCRIPT_VALUE_NUMBER};
  (void)args;
  (void)arg_count;
  probe->calls++;
  probe->destroy_status =
      toad_script_module_destroy(probe->module, probe->result);
  value.as.number = 1.0;
  return toad_script_host_result_set_value(builder, &value);
}

spec("Toad mode-fixed Host instances") {
  it("creates interpreter and JIT instances from one lowered module") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *interpreter = NULL;
    toad_script_instance_t *jit = NULL;
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func value(){return 42;};export(\"value\");",
                             &module),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &interpreter),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_JIT, result, &jit),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(ts_host_instance_mode(interpreter),
                TOAD_SCRIPT_EXEC_INTERPRETER);
    check_equal(ts_host_instance_mode(jit), TOAD_SCRIPT_EXEC_JIT);
    check_equal(ts_host_instance_initializer_count(interpreter), (size_t)1);
    check_equal(ts_host_instance_initializer_count(jit), (size_t)1);
    check_equal(ts_host_module_parse_count(module), (size_t)1);
    check_equal(ts_host_module_lower_count(module), (size_t)1);
    destroy_instance(jit, result);
    destroy_instance(interpreter, result);
    destroy_module(module, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }

  it("rejects invalid options without publishing a partial instance") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *instance = (toad_script_instance_t *)(uintptr_t)1;
    toad_script_instance_options_t options;
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func value(){return 1;};export(\"value\");",
                             &module),
                TOAD_SCRIPT_STATUS_OK);
    toad_script_instance_options_init(&options);
    options.mode = 99;
    check_equal(toad_script_instance_create(module, &options, result, &instance),
                TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    check_null(instance);
    instance = (toad_script_instance_t *)(uintptr_t)1;
    options.mode = TOAD_SCRIPT_EXEC_INTERPRETER;
    options.struct_size++;
    check_equal(toad_script_instance_create(module, &options, result, &instance),
                TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    check_null(instance);
    instance = (toad_script_instance_t *)(uintptr_t)1;
    options.struct_size = sizeof(options);
    options.reserved[2] = 1;
    check_equal(toad_script_instance_create(module, &options, result, &instance),
                TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    check_null(instance);
    destroy_module(module, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }

  it("checks the result context before touching instance outputs") {
    toad_script_ctx_t *first_ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_ctx_t *second_ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *first_result = NULL;
    toad_script_result_t *second_result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *instance = (toad_script_instance_t *)(uintptr_t)1;
    toad_script_instance_options_t options;
    check_equal(toad_script_result_create(first_ctx, &first_result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(toad_script_result_create(second_ctx, &second_result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(first_ctx, first_result,
                             "func value(){return 1;};export(\"value\");",
                             &module),
                TOAD_SCRIPT_STATUS_OK);
    toad_script_instance_options_init(&options);
    check_equal(toad_script_instance_create(module, &options, second_result,
                                              &instance),
                TOAD_SCRIPT_STATUS_CONTEXT_MISMATCH);
    check_equal((uintptr_t)instance, (uintptr_t)1);
    destroy_module(module, first_result);
    toad_script_result_destroy(second_result);
    toad_script_result_destroy(first_result);
    toad_script_free(second_ctx);
    toad_script_free(first_ctx);
  }

  it("keeps initializer state isolated between instances in both modes") {
    static const char source[] =
        "counter=1;func next(){counter=counter+1;return counter;};"
        "export(\"next\");";
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *interpreter = NULL;
    toad_script_instance_t *jit = NULL;
    toad_script_export_handle_t interpreter_handle = 0;
    toad_script_export_handle_t jit_handle = 0;
    double output = 0;
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result, source, &module),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &interpreter),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_JIT, result, &jit),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(resolve_export(interpreter, "next", 4, result,
                               &interpreter_handle),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(resolve_export(jit, "next", 4, result, &jit_handle),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(ts_host_instance_execute_numeric(interpreter, interpreter_handle,
                                                 NULL, 0, &output),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(output, 2.0);
    check_equal(ts_host_instance_execute_numeric(interpreter, interpreter_handle,
                                                 NULL, 0, &output),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(output, 3.0);
    check_equal(ts_host_instance_execute_numeric(jit, jit_handle, NULL, 0,
                                                 &output),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(output, 2.0);
    destroy_instance(jit, result);
    destroy_instance(interpreter, result);
    destroy_module(module, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }

  it("keeps module code alive until the last instance is destroyed") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *instance = NULL;
    toad_script_export_handle_t handle = 0;
    double output = 0;
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func value(){return 42;};export(\"value\");",
                             &module),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_JIT, result, &instance),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(resolve_export(instance, "value", 5, result, &handle),
                TOAD_SCRIPT_STATUS_OK);
    destroy_module(module, result);
    module = NULL;
    check_equal(ts_host_instance_execute_numeric(instance, handle, NULL, 0,
                                                 &output),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(output, 42.0);
    destroy_instance(instance, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }

  it("pins the module before an initializer callback can destroy it") {
    for (int use_jit = 0; use_jit <= 1; ++use_jit) {
      destroy_module_during_init_probe_t probe = {0};
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      toad_script_result_t *result = NULL;
      toad_script_module_t *module = NULL;
      toad_script_instance_t *instance = NULL;
      toad_script_export_handle_t handle = 0;
      toad_script_host_function_descriptor_t descriptor = {
          .struct_size = sizeof(descriptor),
          .min_arity = 0,
          .max_arity = 0,
          .name = {"destroy_module_on_init", 22},
      };
      double output = 0;
      check_equal(toad_script_result_create(ctx, &result),
                  TOAD_SCRIPT_STATUS_OK);
      probe.result = result;
      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, destroy_module_during_init_host, &probe,
                      result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(compile_text(
                      ctx, result,
                      "destroy_module_on_init();"
                      "func value(){return 42;};export(\"value\");",
                      &module),
                  TOAD_SCRIPT_STATUS_OK);
      probe.module = module;
      check_equal(create_instance(module,
                                  use_jit ? TOAD_SCRIPT_EXEC_JIT
                                          : TOAD_SCRIPT_EXEC_INTERPRETER,
                                  result, &instance),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(probe.calls, (size_t)1);
      check_equal(probe.destroy_status, TOAD_SCRIPT_STATUS_OK);
      check_equal(resolve_export(instance, "value", 5, result, &handle),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_instance_execute_numeric(instance, handle, NULL, 0,
                                                   &output),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(output, 42.0);
      destroy_instance(instance, result);
      toad_script_result_destroy(result);
      toad_script_free(ctx);
    }
  }

  it("rejects destroy while the instance is executing") {
    destroy_during_call_probe_t probe = {0};
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *instance = NULL;
    toad_script_export_handle_t handle = 0;
    toad_script_host_function_descriptor_t descriptor = {
        .struct_size = sizeof(descriptor),
        .min_arity = 0,
        .max_arity = 0,
        .name = {"destroy_probe", 13},
    };
    double output = 0;
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    probe.result = result;
    check_equal(toad_script_context_register_host_function(
                    ctx, &descriptor, destroy_during_call_host, &probe, result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(
                    ctx, result,
                    "func invoke(){return destroy_probe();};export(\"invoke\");",
                    &module),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &instance),
                TOAD_SCRIPT_STATUS_OK);
    probe.instance = instance;
    check_equal(resolve_export(instance, "invoke", 6, result, &handle),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(ts_host_instance_execute_numeric(instance, handle, NULL, 0,
                                                 &output),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(output, 17.0);
    check_equal(probe.calls, (size_t)1);
    check_equal(probe.destroy_status, TOAD_SCRIPT_STATUS_INVALID_STATE);
    destroy_instance(instance, result);
    destroy_module(module, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }

  it("allows retained cleanup after an early parent close and rejects new instances") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *first = NULL;
    toad_script_instance_t *second = (toad_script_instance_t *)(uintptr_t)1;
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func value(){return 1;};export(\"value\");",
                             &module),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &first),
                TOAD_SCRIPT_STATUS_OK);
    toad_script_free(ctx);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &second),
                TOAD_SCRIPT_STATUS_INVALID_STATE);
    check_null(second);
    destroy_module(module, result);
    destroy_instance(first, result);
    toad_script_result_destroy(result);
  }

  it("resolves exports and validates handle and info ownership") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *first = NULL;
    toad_script_instance_t *second = NULL;
    toad_script_export_handle_t handle = 99;
    toad_script_export_info_t info = {.struct_size = sizeof(info)};
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(
                    ctx, result,
                    "func add(a,b){return a+b;};export(\"add\");", &module),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &first),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &second),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(resolve_export(first, "missing", 7, result, &handle),
                TOAD_SCRIPT_STATUS_NOT_FOUND);
    check_equal(handle, (toad_script_export_handle_t)0);
    check_equal(resolve_export(first, "add", 3, result, &handle),
                TOAD_SCRIPT_STATUS_OK);
    check_not_equal(handle, (toad_script_export_handle_t)0);
    check_equal(toad_script_instance_get_export_info(first, handle, result,
                                                      &info),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(info.min_arity, (uint32_t)2);
    check_equal(info.max_arity, (uint32_t)2);
    check_equal(info.name.size, (size_t)3);
    check_equal(memcmp(info.name.data, "add", 3), 0);
    check_equal(toad_script_instance_get_export_info(second, handle, result,
                                                      &info),
                TOAD_SCRIPT_STATUS_INVALID_EXPORT_HANDLE);
    check_equal(toad_script_instance_get_export_info(first, 0, result, &info),
                TOAD_SCRIPT_STATUS_INVALID_EXPORT_HANDLE);
    check_equal(toad_script_instance_get_export_info(
                    first,
                    ((toad_script_export_handle_t)ts_host_instance_generation(first)
                     << 32) |
                        2u,
                    result, &info),
                TOAD_SCRIPT_STATUS_INVALID_EXPORT_HANDLE);
    info.struct_size++;
    check_equal(toad_script_instance_get_export_info(first, handle, result,
                                                      &info),
                TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    info.struct_size = sizeof(info);
    info.reserved[0] = 1;
    check_equal(toad_script_instance_get_export_info(first, handle, result,
                                                      &info),
                TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    destroy_instance(second, result);
    destroy_instance(first, result);
    destroy_module(module, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }

  it("rejects stale handles after instance destruction") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *first = NULL;
    toad_script_instance_t *second = NULL;
    toad_script_export_handle_t stale = 0;
    toad_script_export_info_t info = {.struct_size = sizeof(info)};
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func f(){return 1;};export(\"f\");", &module),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_JIT, result, &first),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(resolve_export(first, "f", 1, result, &stale),
                TOAD_SCRIPT_STATUS_OK);
    destroy_instance(first, result);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_JIT, result, &second),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(toad_script_instance_get_export_info(second, stale, result,
                                                      &info),
                TOAD_SCRIPT_STATUS_INVALID_EXPORT_HANDLE);
    destroy_instance(second, result);
    destroy_module(module, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }

  it("allocates the last generation once and never wraps or reuses it") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
    toad_script_result_t *result = NULL;
    toad_script_module_t *module = NULL;
    toad_script_instance_t *last = NULL;
    toad_script_instance_t *overflow = (toad_script_instance_t *)(uintptr_t)1;
    check_equal(toad_script_result_create(ctx, &result),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(compile_text(ctx, result,
                             "func f(){return 1;};export(\"f\");", &module),
                TOAD_SCRIPT_STATUS_OK);
    ctx->next_instance_generation = UINT32_MAX;
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &last),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(ts_host_instance_generation(last), UINT32_MAX);
    destroy_instance(last, result);
    check_equal(create_instance(module, TOAD_SCRIPT_EXEC_INTERPRETER, result,
                                &overflow),
                TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED);
    check_null(overflow);
    check_equal(ctx->next_instance_generation,
                (uint64_t)UINT32_MAX + (uint64_t)1);
    destroy_module(module, result);
    toad_script_result_destroy(result);
    toad_script_free(ctx);
  }
}
