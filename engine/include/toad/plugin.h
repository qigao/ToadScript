/**
 * @file plugin.h
 * @brief Toad module adapter published through the canonical Salts Plugin ABI.
 *
 * Salts owns DSO publication, loading, lifecycle and leases. Toad owns
 * only the per-script-context module adapter semantics: load(env, scratch)
 * mounts one module/context and returns its opaque context instance; unload()
 * releases that context instance.
 */
#ifndef TOAD_PLUGIN_H
#define TOAD_PLUGIN_H

#include <cmeta/plugin.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TOAD_C_API
#define TOAD_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct toad_env_s toad_env_t;
typedef struct mem_pool_s mem_pool_t;
typedef struct toad_module_s toad_module_t;
TOAD_C_API void toad_env_add_module(toad_env_t *env,
                                        const toad_module_t *mod);

#define TOAD_PLUGIN_MODULE_EXPORT_ID "toad.module"
#define TOAD_PLUGIN_MODULE_CONTRACT_ID "toad.module"
#define TOAD_PLUGIN_MODULE_CONTRACT_VERSION 1u

/*
 * This interface is intentionally per-context. It is not the Salts DSO
 * lifecycle: the plugin manifest remains passive and CMeta::Plugin owns the
 * module start/lease/stop/unload state machine.
 */
#define TOAD_PLUGIN_MODULE_METHODS(X, I) \
    X(I, R2, void *, load, void *, env, void *, scratch) \
    X(I, V1, void, unload, void *, instance)

CMETA_INTERFACE(toad_plugin_module, TOAD_PLUGIN_MODULE_METHODS);

#define TOAD_PLUGIN_DETAIL_EXPORT(plugin_name, load_fn, unload_fn)                 \
    static char toad__##plugin_name##_adapter_token;                              \
    static const toad_plugin_module_vtable toad__##plugin_name##_adapter_vtable = { \
        .implementation = #plugin_name,                                         \
        .capabilities = 0u,                                                     \
        .load = (load_fn),                                                      \
        .unload = (unload_fn),                                                  \
    };                                                                          \
    static toad_plugin_module toad__##plugin_name##_adapter = {                     \
        &toad__##plugin_name##_adapter_token,                                     \
        &toad__##plugin_name##_adapter_vtable                                     \
    };                                                                          \
    static const cmeta_plugin_export toad__##plugin_name##_export = {             \
        .struct_size = CMETA_PLUGIN_EXPORT_SIZE,                                \
        .kind = CMETA_PLUGIN_EXPORT_INTERFACE,                                  \
        .contract_version = TOAD_PLUGIN_MODULE_CONTRACT_VERSION,                  \
        .capabilities = 0u,                                                     \
        .export_id = TOAD_PLUGIN_MODULE_EXPORT_ID,                                \
        .contract_id = TOAD_PLUGIN_MODULE_CONTRACT_ID,                            \
        .value.interface = {                                                    \
            .desc = &toad_plugin_module_interface_meta,                           \
            .value = &toad__##plugin_name##_adapter,                             \
        },                                                                      \
    };                                                                          \
    static const cmeta_plugin_manifest toad__##plugin_name##_manifest = {         \
        .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,                              \
        .abi_version = CMETA_PLUGIN_ABI_VERSION,                                \
        .plugin_id = #plugin_name,                                              \
        .version = {1u, 0u, 0u},                                               \
        .exports = &toad__##plugin_name##_export,                                \
        .export_count = 1u,                                                     \
        .self = NULL,                                                           \
        .start = NULL,                                                          \
        .request_stop = NULL,                                                   \
        .is_quiescent = NULL,                                                   \
        .destroy = NULL,                                                        \
    };                                                                          \
    CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL    \
    cmeta_plugin_query(uint32_t host_abi) {                                     \
        return host_abi == CMETA_PLUGIN_ABI_VERSION                            \
                   ? &toad__##plugin_name##_manifest                             \
                   : NULL;                                                      \
    }

/* Stateless module: mount one toad_module_t into the target environment. */
#define TOAD_PLUGIN_MODULE(plugin_name, module_fn)                                \
    static void *toad__##plugin_name##_load(void *self, void *env,               \
                                           void *scratch) {                     \
        const toad_module_t *module;                                          \
        (void)self;                                                             \
        (void)scratch;                                                          \
        module = module_fn();                                                   \
        if (env == NULL || module == NULL) return NULL;                         \
        toad_env_add_module((toad_env_t *)env, module);                     \
        return env;                                                             \
    }                                                                           \
    static void toad__##plugin_name##_unload(void *self, void *instance) {        \
        (void)self;                                                             \
        (void)instance;                                                         \
    }                                                                           \
    TOAD_PLUGIN_DETAIL_EXPORT(plugin_name, toad__##plugin_name##_load,              \
                            toad__##plugin_name##_unload)

/* Stateless module with context cleanup attached to the borrowed environment. */
#define TOAD_PLUGIN_MODULE_WITH_UNLOAD(plugin_name, module_fn, unload_fn)         \
    static void *toad__##plugin_name##_load(void *self, void *env,               \
                                           void *scratch) {                     \
        const toad_module_t *module;                                          \
        (void)self;                                                             \
        (void)scratch;                                                          \
        module = module_fn();                                                   \
        if (env == NULL || module == NULL) return NULL;                         \
        toad_env_add_module((toad_env_t *)env, module);                     \
        return env;                                                             \
    }                                                                           \
    static void toad__##plugin_name##_unload(void *self, void *instance) {        \
        (void)self;                                                             \
        if (instance != NULL) unload_fn((toad_env_t *)instance);              \
    }                                                                           \
    TOAD_PLUGIN_DETAIL_EXPORT(plugin_name, toad__##plugin_name##_load,              \
                            toad__##plugin_name##_unload)

/* Stateful module: create one context per Toad script context. */
#define TOAD_PLUGIN_STATEFUL(plugin_name, create_fn, loader_fn, destroy_fn)       \
    static void *toad__##plugin_name##_load(void *self, void *env,               \
                                           void *scratch) {                     \
        void *ctx;                                                              \
        (void)self;                                                             \
        ctx = (void *)create_fn();                                              \
        if (ctx == NULL) return NULL;                                           \
        loader_fn(ctx, env, scratch);                                           \
        return ctx;                                                             \
    }                                                                           \
    static void toad__##plugin_name##_unload(void *self, void *instance) {        \
        (void)self;                                                             \
        if (instance != NULL) destroy_fn(instance);                             \
    }                                                                           \
    TOAD_PLUGIN_DETAIL_EXPORT(plugin_name, toad__##plugin_name##_load,              \
                            toad__##plugin_name##_unload)

#ifdef __cplusplus
}
#endif

#endif /* TOAD_PLUGIN_H */
