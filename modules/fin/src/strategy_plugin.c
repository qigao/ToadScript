#include <toad/plugin.h>
#include "fin_plugin_functions.h"

#include <threads/threads.h>

const toad_module_t *toad_module_strategy(void);

static void *fin_module_load(void *self, void *env, void *scratch) {
  const toad_module_t *module;
  (void)self;
  (void)scratch;
  module = toad_module_strategy();
  if (!env || !module) return NULL;
  toad_env_add_module((toad_env_t *)env, module);
  return env;
}

static void fin_module_unload(void *self, void *instance) {
  (void)self;
  (void)instance;
}

static char fin_module_token;

static const toad_plugin_module_vtable fin_module_vtable = {
    .implementation = "fin",
    .capabilities = 0u,
    .load = fin_module_load,
    .unload = fin_module_unload,
};

static toad_plugin_module fin_module = {
    .self = &fin_module_token,
    .vtable = &fin_module_vtable,
};

static bool CMETA_PLUGIN_CALL fin_kelly_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  double win_rate;
  double avg_win;
  double avg_loss;

  if (context != NULL || !return_storage || !params ||
      param_count != 3u || !params[0] || !params[1] || !params[2])
    return false;

  win_rate = *(const double *)params[0];
  avg_win = *(const double *)params[1];
  avg_loss = *(const double *)params[2];
  *(double *)return_storage =
      ts_fin_kelly(win_rate, avg_win, avg_loss);
  return true;
}

/*
 * Migration state under one Salts Plugin ABI:
 * - toad.module keeps the remaining legacy language facade mounted;
 * - strategy.kelly is already a canonical Function export.
 * Both are protected by the same manifest lease; this is not a dual loader or
 * a parallel Plugin ABI.
 */
static cmeta_plugin_export fin_exports[2];
static sync_once_t fin_exports_once = SALTS_ONCE_INIT;

static void fin_exports_init(void) {
  fin_exports[0] = (cmeta_plugin_export){
      .struct_size = CMETA_PLUGIN_EXPORT_SIZE,
      .kind = CMETA_PLUGIN_EXPORT_INTERFACE,
      .contract_version = TOAD_PLUGIN_MODULE_CONTRACT_VERSION,
      .capabilities = 0u,
      .export_id = TOAD_PLUGIN_MODULE_EXPORT_ID,
      .contract_id = TOAD_PLUGIN_MODULE_CONTRACT_ID,
      .value.interface = {
          .desc = &toad_plugin_module_interface_meta,
          .value = &fin_module,
      },
  };

  fin_exports[1] = (cmeta_plugin_export){
      .struct_size = CMETA_PLUGIN_EXPORT_SIZE,
      .kind = CMETA_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "strategy.kelly",
      .contract_id = "toad.fin.strategy.kelly",
      .value.function = {
          .desc = FunctionMeta(ts_fin_kelly),
          .abi = FunctionAbi(ts_fin_kelly),
          .context = NULL,
          .invoke = fin_kelly_invoke,
      },
  };
}

static const cmeta_plugin_manifest fin_manifest = {
    .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,
    .abi_version = CMETA_PLUGIN_ABI_VERSION,
    .plugin_id = "fin",
    .version = {1u, 0u, 0u},
    .exports = fin_exports,
    .export_count = 2u,
    .self = NULL,
    .start = NULL,
    .request_stop = NULL,
    .is_quiescent = NULL,
    .destroy = NULL,
};

CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL
cmeta_plugin_query(uint32_t host_abi) {
  if (host_abi != CMETA_PLUGIN_ABI_VERSION) return NULL;
  sync_once(&fin_exports_once, fin_exports_init);
  return &fin_manifest;
}
