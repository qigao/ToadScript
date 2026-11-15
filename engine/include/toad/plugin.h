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

#include <cmeta/plugin_decl.h>
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

/* Value-returning scalar authoring convenience. CMeta remains the signature,
 * carrier, result-ownership and exact-invoke fact source. Void/advanced native
 * contracts can use the canonical CMeta declarations directly. */
#define TOAD_PLUGIN_FUNCTION(type, name, ...) \
    FunctionInvokeDeclAsAbiResult(value, type, CMETA_FUNCTION_RETURN_TYPEOF(type), \
        CMETA_FUNCTION_RETURN_ABI(type), CMETA_RESULT_VALUE, name, __VA_ARGS__)
#define TOAD_PLUGIN_FUNCTION0(type, name) \
    Function0InvokeDeclAsAbiResult(value, type, CMETA_FUNCTION_RETURN_TYPEOF(type), \
        CMETA_FUNCTION_RETURN_ABI(type), CMETA_RESULT_VALUE, name)

/* Exactly one publication/query per DSO. This list feeds the canonical Plugin
 * declaration rather than maintaining a parallel manifest or mutable registry. */
#define TOAD_PLUGIN_DETAIL_MODULE_EXPORTS(X) \
    X(interface, (toad_plugin_module, &toad__module_adapter), \
      TOAD_PLUGIN_MODULE_EXPORT_ID, TOAD_PLUGIN_MODULE_CONTRACT_ID, \
      TOAD_PLUGIN_MODULE_CONTRACT_VERSION, 0u)
#define TOAD_PLUGIN_DETAIL_EXPORT(plugin_name, load_fn, unload_fn) \
    static char toad__module_adapter_token; \
    static const toad_plugin_module_vtable toad__module_adapter_vtable = { \
        #plugin_name, 0u, (load_fn), (unload_fn) \
    }; \
    static toad_plugin_module toad__module_adapter = { \
        &toad__module_adapter_token, &toad__module_adapter_vtable \
    }; \
    CMETA_PLUGIN_DECLARE(toad__module, #plugin_name, (1u, 0u, 0u), \
        TOAD_PLUGIN_DETAIL_MODULE_EXPORTS, CMETA_PLUGIN_PASSIVE());

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
