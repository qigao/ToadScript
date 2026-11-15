#include <stdio.h>
#include <string.h>
#include <toad/script.h>

int main(int argc, char **argv) {
  cmeta_plugin_registry registry = {0};
  cmeta_plugin_registry_config config = {.capacity = 2};
  cmeta_plugin_ref plugins[2] = {{0}};
  int loaded[2] = {0}, started[2] = {0}, initialized = 0, success = 0;
  toad_script_ctx_t *ctx = NULL;
  toad_script_result_t *result = NULL;
  toad_script_module_t *module = NULL;
  toad_script_instance_t *instance = NULL;
  toad_script_module_options_t options;
  const char *source = "func calculate(raw){value=plant_normalize(raw,2.0);"
                       "return plant_cost(value,10.0)+plant_limit();};export(\"calculate\");";
  if (argc != 3) {
    fprintf(stderr, "Usage: example_native_modules <calibration-plugin> <maintenance-plugin>\n");
    return 2;
  }
#define NATIVE(operation)                                                                          \
  do {                                                                                             \
    cmeta_plugin_status s = (operation);                                                           \
    if (s != CMETA_PLUGIN_OK) {                                                                    \
      fprintf(stderr, "%s\n", cmeta_plugin_status_string(s));                                      \
      goto finish;                                                                                 \
    }                                                                                              \
  } while (0)
#define SCRIPT(operation)                                                                          \
  do {                                                                                             \
    if ((operation) != TOAD_SCRIPT_STATUS_OK) goto finish;                                         \
  } while (0)
  NATIVE(cmeta_plugin_registry_init(&registry, &config));
  initialized = 1;
  for (size_t i = 0; i < 2; ++i) {
    NATIVE(cmeta_plugin_registry_load(&registry, argv[i + 1], &plugins[i]));
    loaded[i] = 1;
    NATIVE(cmeta_plugin_registry_start(&registry, plugins[i]));
    started[i] = 1;
  }
  ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
  if (!ctx) goto finish;
  SCRIPT(toad_script_result_create(ctx, &result));
  for (size_t i = 0; i < 2; ++i)
    SCRIPT(toad_script_context_register_plugin(ctx, &registry, plugins[i], result));
  toad_script_module_options_init(&options);
  options.flags = TOAD_SCRIPT_MODULE_EXPLICIT_CALLS;
  SCRIPT(toad_script_module_compile(ctx, (toad_script_string_view_t){source, strlen(source)},
                                    &options, result, &module));
  for (int mode = TOAD_SCRIPT_EXEC_INTERPRETER; mode <= TOAD_SCRIPT_EXEC_JIT; ++mode) {
    toad_script_instance_options_t instance_options;
    toad_script_call_options_t call_options;
    toad_script_export_handle_t function = 0;
    toad_script_value_view_t input = {0}, output = {0};
    toad_script_instance_options_init(&instance_options);
    instance_options.mode = (toad_script_execution_mode_t)mode;
    toad_script_call_options_init(&call_options);
    SCRIPT(toad_script_instance_create(module, &instance_options, result, &instance));
    SCRIPT(toad_script_instance_resolve_export(
        instance, (toad_script_string_view_t){"calculate", 9}, result, &function));
    input.kind = TOAD_SCRIPT_VALUE_NUMBER;
    input.as.number = 8.0;
    SCRIPT(toad_script_instance_call(instance, function, &input, 1, &call_options, result));
    SCRIPT(toad_script_result_get_value(result, &output));
    if (output.kind != TOAD_SCRIPT_VALUE_NUMBER || output.as.number != 132.0) goto finish;
    printf("%s: cost=%.0f\n", mode == TOAD_SCRIPT_EXEC_INTERPRETER ? "interpreter" : "jit",
           output.as.number);
    SCRIPT(toad_script_instance_destroy(instance, result));
    instance = NULL;
  }
  success = 1;
finish:
  if (!success) {
    toad_script_error_info_t error = {.struct_size = sizeof(error)};
    if (result && toad_script_result_get_error(result, &error) == TOAD_SCRIPT_STATUS_OK)
      fprintf(stderr, "Toad: %.*s\n", (int)error.message.size, error.message.data);
    else fprintf(stderr, "Native module example failed\n");
  }
  /* A failed destruction retains its owner: never unload code still in use. */
  if (instance && toad_script_instance_destroy(instance, result) != TOAD_SCRIPT_STATUS_OK) return 1;
  if (module && toad_script_module_destroy(module, result) != TOAD_SCRIPT_STATUS_OK) return 1;
  if (result) toad_script_result_destroy(result);
  if (ctx) toad_script_free(ctx);
  for (size_t i = 2; i > 0; --i) {
    size_t index = i - 1;
    if (!loaded[index]) continue;
    if (started[index]) {
      bool quiescent = false;
      if (cmeta_plugin_registry_request_stop(&registry, plugins[index]) != CMETA_PLUGIN_OK ||
          cmeta_plugin_registry_poll_quiescent(&registry, plugins[index], &quiescent) !=
              CMETA_PLUGIN_OK ||
          !quiescent)
        return 1;
    }
    if (cmeta_plugin_registry_unload(&registry, plugins[index]) != CMETA_PLUGIN_OK) return 1;
  }
  if (initialized && cmeta_plugin_registry_destroy(&registry) != CMETA_PLUGIN_OK) return 1;
  return success ? 0 : 1;
}
