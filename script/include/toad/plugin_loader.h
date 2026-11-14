/**
 * @file plugin_loader.h
 * @brief Toad adapter over the canonical Salts plugin registry.
 */
#ifndef TOAD_PLUGIN_LOADER_H
#define TOAD_PLUGIN_LOADER_H

#include <toad/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct toad_plugin_function_view_s {
    const cmeta_plugin_export *entry;         /* borrowed under the plugin lease */
    cmeta_plugin_function_invoke_fn invoke;   /* pre-bound exact adapter */
    void *context;                            /* provider-owned, lease-bound */
} toad_plugin_function_view_t;

typedef struct toad_plugin_handle_s {
    cmeta_plugin_registry registry;          /* owns the loaded DSO */
    cmeta_plugin_ref plugin_ref;             /* registry identity */
    cmeta_plugin_lease lease;                /* keeps manifest/code alive */
    const cmeta_plugin_manifest *manifest;   /* borrowed under lease */
    const cmeta_plugin_export *module_export;/* canonical module capability */
    toad_plugin_module *module;                /* optional borrowed interface handle */
    void *instance;                          /* optional per-script-context instance */
    void *function_bindings;                 /* private canonical Function binding cache */
    size_t function_binding_count;
    void *databind_bindings;                 /* private generated Service BindingPlan cache */
    size_t databind_binding_count;
    void *databind_codec;                    /* caller-owned catalog codec, lease-bound metadata */
    int initialized;
} toad_plugin_handle_t;

#define TOAD_PLUGIN_ERROR_PATH_CAPACITY 1024U
#define TOAD_PLUGIN_ERROR_MESSAGE_CAPACITY 512U

typedef enum toad_plugin_error_code_e {
    TOAD_PLUGIN_ERROR_NONE = 0,
    TOAD_PLUGIN_ERROR_INVALID_ARGUMENT = -1,
    TOAD_PLUGIN_ERROR_OPEN = -2,
    TOAD_PLUGIN_ERROR_SYMBOL = -3,
    TOAD_PLUGIN_ERROR_DESCRIPTOR = -4,
    TOAD_PLUGIN_ERROR_ABI = -5,
    TOAD_PLUGIN_ERROR_NAME = -6,
    TOAD_PLUGIN_ERROR_OUT_OF_MEMORY = -7,
    TOAD_PLUGIN_ERROR_INITIALIZE = -8
} toad_plugin_error_code_t;

typedef enum toad_plugin_error_stage_e {
    TOAD_PLUGIN_STAGE_NONE = 0,
    TOAD_PLUGIN_STAGE_ARGUMENT,
    TOAD_PLUGIN_STAGE_OPEN,
    TOAD_PLUGIN_STAGE_SYMBOL,
    TOAD_PLUGIN_STAGE_ABI,
    TOAD_PLUGIN_STAGE_INITIALIZE
} toad_plugin_error_stage_t;

typedef struct toad_plugin_error_s {
    toad_plugin_error_code_t code;
    toad_plugin_error_stage_t stage;
    uint32_t native_code;
    char attempted_path[TOAD_PLUGIN_ERROR_PATH_CAPACITY];
    char message[TOAD_PLUGIN_ERROR_MESSAGE_CAPACITY];
} toad_plugin_error_t;

/**
 * Load and validate a plugin, optionally requiring its descriptor name to
 * equal @p expected_name. The loader searches executable-relative locations
 * only when @p path is a filename without directory separators.
 *
 * @return TOAD_PLUGIN_ERROR_NONE on success, otherwise a negative error code.
 */
int toad_plugin_load_ex(const char *path, const char *expected_name,
                      toad_plugin_handle_t **out, toad_plugin_error_t *error);

/**
 * Resolve the configured Toad plugin path and load it through
 * CMeta::Plugin. The DSO must export the canonical cmeta_plugin_query entry.
 * @return handle on success, NULL on failure.
 */
toad_plugin_handle_t *toad_plugin_load(const char *path);

/*
 * Control-plane lookup over the already-bound canonical Function cache.
 * Performs no registry/symbol/manifest discovery. Returned descriptor/code
 * pointers are valid only while the handle's Salts Plugin lease remains held.
 */
int toad_plugin_find_bound_function(
    const toad_plugin_handle_t *handle, const char *export_id,
    toad_plugin_function_view_t *out);

/**
 * Call the canonical Toad module interface load(env, scratch) method
 * while the Salts plugin lease is held, and store the per-context instance.
 * @return 0 on success, -1 on failure.
 */
int toad_plugin_init(toad_plugin_handle_t *h, void *env, void *scratch );

/** Initialize a validated plugin and report a structured failure. */
int toad_plugin_init_ex(toad_plugin_handle_t *h, void *env, void *scratch,
                      toad_plugin_error_t *error);

/**
 * Release the per-context module instance, then release the Salts plugin lease,
 * quiesce and unload the DSO.
 */
void toad_plugin_unload(toad_plugin_handle_t *h);

#ifdef __cplusplus
}
#endif

#endif /* TOAD_PLUGIN_LOADER_H */
