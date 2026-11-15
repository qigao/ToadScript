#ifndef TOAD_HOST_PLUGIN_FUNCTION_H
#define TOAD_HOST_PLUGIN_FUNCTION_H

#include <cmeta/plugin.h>
#include <toad/module.h>

#define TOAD_PLUGIN_MAX_SCALAR_PARAMS 16u

typedef enum toad_plugin_scalar_kind_e {
  TOAD_PLUGIN_SCALAR_UNSUPPORTED = 0,
  TOAD_PLUGIN_SCALAR_VOID,
  TOAD_PLUGIN_SCALAR_BOOL,
  TOAD_PLUGIN_SCALAR_INT,
  TOAD_PLUGIN_SCALAR_LONG,
  TOAD_PLUGIN_SCALAR_FLOAT,
  TOAD_PLUGIN_SCALAR_DOUBLE
} toad_plugin_scalar_kind_t;

typedef union toad_plugin_scalar_storage_u {
  _Bool boolean_value;
  int int_value;
  long long_value;
  float float_value;
  double double_value;
} toad_plugin_scalar_storage_t;

typedef struct toad_plugin_function_binding_s {
  const cmeta_plugin_export *entry;
  cmeta_plugin_function_invoke_fn invoke;
  void *context;
  size_t param_count;
  toad_plugin_scalar_kind_t param_kinds[TOAD_PLUGIN_MAX_SCALAR_PARAMS];
  toad_plugin_scalar_kind_t return_kind;
} toad_plugin_function_binding_t;

int ts_plugin_function_scalar_binding(const cmeta_plugin_export *entry,
                                      toad_plugin_function_binding_t *binding);
int ts_plugin_scalar_from_engine(const toad_value_t *arg, toad_plugin_scalar_kind_t kind,
                                 toad_plugin_scalar_storage_t *storage, void **address);
void *ts_plugin_scalar_return_storage(toad_plugin_scalar_kind_t kind,
                                      toad_plugin_scalar_storage_t *storage);
toad_value_t ts_plugin_scalar_to_engine(toad_plugin_scalar_kind_t kind,
                                        const toad_plugin_scalar_storage_t *storage);
toad_value_t ts_plugin_function_scalar_call(size_t argc, toad_value_t *args, toad_env_t *env,
                                            void *user_data);

#endif
