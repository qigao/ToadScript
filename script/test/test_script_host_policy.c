#include "tinytest.h"
#include <string.h>
#include <toad/script.h>

static toad_script_status_t pure_callback(void *user, const toad_script_value_view_t *args,
                                          size_t count, toad_script_host_result_builder_t *result) {
  (void)user;
  if (count != 1) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  return toad_script_host_result_set_value(result, args);
}

static toad_script_status_t compile_closed(const char *text) {
  toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
  toad_script_result_t *result = NULL;
  toad_script_module_t *module = NULL;
  toad_script_module_options_t options;
  toad_script_host_function_descriptor_t host = {0};
  toad_script_status_t status;
  check_not_null(ctx);
  check_equal(toad_script_result_create(ctx, &result), TOAD_SCRIPT_STATUS_OK);
  host.struct_size = sizeof(host);
  host.name = (toad_script_string_view_t){"host_pure", 9};
  host.min_arity = host.max_arity = 1;
  check_equal(toad_script_context_register_host_function(ctx, &host, pure_callback, NULL, result),
              TOAD_SCRIPT_STATUS_OK);
  toad_script_module_options_init(&options);
  options.flags = TOAD_SCRIPT_MODULE_EXPLICIT_CALLS;
  status = toad_script_module_compile(ctx, (toad_script_string_view_t){text, strlen(text)},
                                      &options, result, &module);
  if (status == TOAD_SCRIPT_STATUS_OK) {
    for (int mode = TOAD_SCRIPT_EXEC_INTERPRETER; mode <= TOAD_SCRIPT_EXEC_JIT; ++mode) {
      toad_script_instance_options_t instance_options;
      toad_script_call_options_t call_options;
      toad_script_instance_t *instance = NULL;
      toad_script_export_handle_t handle = 0;
      toad_script_value_view_t argument = {0}, value = {0};
      toad_script_instance_options_init(&instance_options);
      instance_options.mode = (toad_script_execution_mode_t)mode;
      toad_script_call_options_init(&call_options);
      check_equal(toad_script_instance_create(module, &instance_options, result, &instance),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_instance_resolve_export(
                      instance, (toad_script_string_view_t){"calculate", 9}, result, &handle),
                  TOAD_SCRIPT_STATUS_OK);
      argument.kind = TOAD_SCRIPT_VALUE_INT64;
      argument.as.integer = 42;
      check_equal(toad_script_instance_call(instance, handle, &argument, 1, &call_options, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(result, &value), TOAD_SCRIPT_STATUS_OK);
      check_equal(value.kind, TOAD_SCRIPT_VALUE_RECORD);
      check_equal(toad_script_instance_destroy(instance, result), TOAD_SCRIPT_STATUS_OK);
    }
    check_equal(toad_script_module_destroy(module, result), TOAD_SCRIPT_STATUS_OK);
  }
  toad_script_result_destroy(result);
  toad_script_free(ctx);
  return status;
}

suite("Explicit Host call capabilities") {
  it("accepts named helpers records lists and explicitly registered callbacks") {
    check_equal(compile_closed("func helper(x){return host_pure(x);};func calculate(x){return map "
                               "{items:list(helper(x))};};export(\"calculate\");"),
                TOAD_SCRIPT_STATUS_OK);
  }
  it("rejects ambient I/O imports eval and dynamic dispatch before lowering") {
    const char *const sources[] = {
        "func calculate(x){return file_exists(x);};export(\"calculate\");",
        "func calculate(x){return import(x);};export(\"calculate\");",
        "func calculate(x){return eval(x);};export(\"calculate\");",
        "func calculate(x){return x.call();};export(\"calculate\");",
        "func calculate(x){return unknown(x);};export(\"calculate\");",
        "func calculate(x){return `probe ${file_exists(x)}`;};export(\"calculate\");"};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i)
      check_equal(compile_closed(sources[i]), TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
  }
  it("rejects callable shadowing in assignments parameters loops and catch variables") {
    const char *const sources[] = {
        "func helper(x){return x;};helper=file_exists;func calculate(x){return "
        "helper(x);};export(\"calculate\");",
        "func helper(x){return x;};func calculate(helper){return "
        "helper(1);};export(\"calculate\");",
        "func helper(x){return x;};func calculate(x){for(helper in list(x)){helper(x);};return "
        "x;};export(\"calculate\");",
        "func helper(x){return x;};func calculate(x){try{throw x;}catch(helper){helper(x);};return "
        "x;};export(\"calculate\");"};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i)
      check_equal(compile_closed(sources[i]), TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
  }
}
