/**
 * @file registry.c
 * @brief Unified registry for module-based built-in function dispatch.
 */

#include "internal.h"
#include <toad/class.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define REGISTRY_INITIAL_CAP 256

static toad_func_entry_t *g_registry = NULL;
static size_t g_registry_count = 0;
static size_t g_registry_cap = 0;
static int g_registry_ready = 0;

static const cmeta_type_identity toad_env_cmeta_identity =
    CMETA_TYPE_ID_ATOM_INIT("toad.engine.env");
static const cmeta_type_identity toad_scratch_cmeta_identity =
    CMETA_TYPE_ID_ATOM_INIT("toad.engine.scratch");

static const cmeta_type_desc toad_env_cmeta_type = {
    .name = "toad_env_t",
    .size = sizeof(toad_env_t),
    .align = _Alignof(toad_env_t),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &toad_env_cmeta_identity
};
static const cmeta_type_desc toad_scratch_cmeta_type = {
    .name = "mem_pool_t",
    .size = sizeof(mem_pool_t),
    .align = _Alignof(mem_pool_t),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &toad_scratch_cmeta_identity
};

static const cmeta_type_identity toad_env_ptr_cmeta_identity =
    CMETA_TYPE_ID_POINTER_INIT(&toad_env_cmeta_identity);
static const cmeta_type_identity toad_scratch_ptr_cmeta_identity =
    CMETA_TYPE_ID_POINTER_INIT(&toad_scratch_cmeta_identity);

static const cmeta_type_desc toad_env_ptr_cmeta_type = {
    .name = "toad_env_t *",
    .size = sizeof(toad_env_t *),
    .align = _Alignof(toad_env_t *),
    .kind = CMETA_T_POINTER,
    .pointee = &toad_env_cmeta_type,
    .traits = NULL,
    .identity = &toad_env_ptr_cmeta_identity
};
static const cmeta_type_desc toad_scratch_ptr_cmeta_type = {
    .name = "mem_pool_t *",
    .size = sizeof(mem_pool_t *),
    .align = _Alignof(mem_pool_t *),
    .kind = CMETA_T_POINTER,
    .pointee = &toad_scratch_cmeta_type,
    .traits = NULL,
    .identity = &toad_scratch_ptr_cmeta_identity
};

int toad_func_entry_reflect(
    const toad_func_entry_t *entry,
    toad_function_reflection_t *out) {
    if (!entry || !entry->name || !entry->fn || !out) return 0;

    memset(out, 0, sizeof(*out));
    out->params[0] = (cmeta_param_desc){
        sizeof(cmeta_param_desc), "argc", &cmeta_type_size, CMETA_PARAM_IN
    };
    out->params[1] = (cmeta_param_desc){
        sizeof(cmeta_param_desc), "args", toad_value_ptr_cmeta_type(),
        CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE
    };
    out->params[2] = (cmeta_param_desc){
        sizeof(cmeta_param_desc), "env", &toad_env_ptr_cmeta_type,
        CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED
    };
    out->params[3] = (cmeta_param_desc){
        sizeof(cmeta_param_desc), "scratch", &toad_scratch_ptr_cmeta_type,
        CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED
    };
    out->function = (cmeta_function_desc){
        .size = sizeof(cmeta_function_desc),
        .name = entry->name,
        .return_type = toad_value_cmeta_type(),
        .params = out->params,
        .param_count = 4u,
        .effects = CMETA_EFFECT_UNKNOWN,
        .properties = CMETA_PROP_NONE,
        /*
         * Raw Engine builtin results are not one uniform ownership class.
         * A builtin may return a scratch-borrowed dynamic payload. The
         * authoritative escape boundary is toad_call_builtin(), which
         * clones/promotes the raw value into the caller env before scratch
         * teardown. Do not infer VALUE from the by-value C return spelling.
         */
        .result_flags = CMETA_RESULT_UNKNOWN
    };
    out->invoke = entry->fn;
    return cmeta_function_desc_valid(&out->function);
}

int toad_module_function_reflect(
    const toad_module_t *module,
    size_t index,
    toad_function_reflection_t *out) {
    if (!module || !module->entries || index >= module->count) return 0;
    return toad_func_entry_reflect(&module->entries[index], out);
}

static toad_value_t toad_call_undefined(toad_env_t *env, const char *name) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };

    if (!env) return zero;

    snprintf(env->error_msg, sizeof(env->error_msg), "Undefined function '%s'",
             name ? name : "<null>");
    env->error_line = env->last_line;
    env->error_column = env->last_column;
    env->flow = toad_FLOW_THROW;
    env->error_value = toad_val_str(vstr_from_cstr(env->error_msg));
    return zero;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const toad_func_entry_t *)a)->name,
                  ((const toad_func_entry_t *)b)->name);
}

static int search_cmp(const void *key, const void *entry) {
    return strcmp((const char *)key, ((const toad_func_entry_t *)entry)->name);
}

static double toad_numeric_value(toad_value_t value) {
    if (value.type == TOAD_VAL_INTEGER) return (double)value.data.integer;
    if (value.type == TOAD_VAL_NUMBER) return value.data.number;
    return 0.0;
}

static toad_value_t *normalize_numeric_args(toad_value_t *args, size_t argc, toad_value_t *stack_args,
                                              size_t stack_cap, int *needs_free) {
    if (needs_free) *needs_free = 0;
    if (!args || argc == 0) return args;

    toad_value_t *normalized = stack_args;
    if (argc > stack_cap) {
        normalized = (toad_value_t *)malloc(argc * sizeof(toad_value_t));
        if (!normalized) return args;
        if (needs_free) *needs_free = 1;
    }

    for (size_t i = 0; i < argc; ++i) {
        normalized[i] = args[i];
        if (normalized[i].type == TOAD_VAL_INTEGER) {
            normalized[i] = toad_val_num((double)normalized[i].data.integer);
        }
    }

    return normalized;
}

static int builtin_needs_raw_args(const char *name) {
    const char *entry = name ? strrchr(name, '.') : NULL;
    if (!name) return 0;
    entry = entry ? entry + 1 : name;
    return strcmp(entry, "typeof") == 0 ||
           strcmp(entry, "is_int64") == 0 ||
           strcmp(entry, "is_bool") == 0 ||
           strcmp(entry, "is_bytes") == 0 ||
           strcmp(entry, "is_decimal") == 0;
}

/* =========================================================================
 * Module cache: sorted flat array of all per-env module entries
 * ========================================================================= */

/** Build sorted cache of all module entries for O(log n) lookup. */
static void build_mod_cache(toad_env_t *env) {
    if (!env || !env->modules || env->module_count == 0) return;

    /* Count total entries across all modules */
    size_t total = 0;
    for (size_t m = 0; m < env->module_count; m++)
        total += env->modules[m]->count;
    if (total == 0) return;

    /* Allocate and flatten */
    toad_func_entry_t *cache = (toad_func_entry_t *)malloc(total * sizeof(toad_func_entry_t));
    if (!cache) return;

    size_t idx = 0;
    for (size_t m = 0; m < env->module_count; m++) {
        const toad_module_t *mod = env->modules[m];
        for (size_t i = 0; i < mod->count; i++)
            cache[idx++] = mod->entries[i];
    }

    /* Sort by name for binary search */
    qsort(cache, total, sizeof(toad_func_entry_t), entry_cmp);

    env->mod_cache = cache;
    env->mod_cache_count = total;
}

/** O(log n) lookup in the sorted module cache. */
static const toad_func_entry_t *mod_cache_find_entry(
    toad_env_t *env, const char *name) {
    if (!env || !name) return NULL;
    if (!env->mod_cache) build_mod_cache(env);
    if (!env->mod_cache || env->mod_cache_count == 0) return NULL;

    return (const toad_func_entry_t *)bsearch(
        name, (toad_func_entry_t *)env->mod_cache, env->mod_cache_count,
        sizeof(toad_func_entry_t), search_cmp);
}

static toad_builtin_fn mod_cache_find(toad_env_t *env, const char *name) {
    const toad_func_entry_t *hit = mod_cache_find_entry(env, name);
    return hit ? hit->fn : NULL;
}

static const toad_func_entry_t *mod_find_entry_in_named_module(
    toad_env_t *env, const char *module_name, size_t module_name_len,
    const char *entry_name) {
    if (!env || !module_name || !entry_name) return NULL;

    for (toad_env_t *e = env; e; e = e->parent) {
        for (size_t m = 0; m < e->module_count; ++m) {
            const toad_module_t *mod = e->modules[m];
            if (!mod || !mod->module_name) continue;
            if (strlen(mod->module_name) != module_name_len ||
                strncmp(mod->module_name, module_name, module_name_len) != 0)
                continue;

            for (size_t i = 0; i < mod->count; ++i) {
                if (strcmp(mod->entries[i].name, entry_name) == 0)
                    return &mod->entries[i];
            }
        }
    }
    return NULL;
}

static toad_builtin_fn mod_find_in_named_module(
    toad_env_t *env, const char *module_name, size_t module_name_len,
    const char *entry_name) {
    const toad_func_entry_t *hit = mod_find_entry_in_named_module(
        env, module_name, module_name_len, entry_name);
    return hit ? hit->fn : NULL;
}

static int is_global_compat_namespace(const char *module_name, size_t module_name_len) {
    static const char *const names[] = {"math", "stats", "string", "io", "core", "regex", NULL};
    for (size_t i = 0; names[i]; ++i) {
        if (strlen(names[i]) == module_name_len &&
            strncmp(names[i], module_name, module_name_len) == 0)
            return 1;
    }
    return 0;
}

void toad_registry_add_module(const toad_module_t *mod) {
    if (!mod) return;
    for (size_t i = 0; i < mod->count; ++i) {
        if (g_registry_count >= g_registry_cap) {
            size_t new_cap = g_registry_cap == 0 ? REGISTRY_INITIAL_CAP : g_registry_cap * 2;
            toad_func_entry_t *grown = (toad_func_entry_t *)realloc(g_registry, new_cap * sizeof(toad_func_entry_t));
            if (!grown) return;
            g_registry = grown;
            g_registry_cap = new_cap;
        }
        g_registry[g_registry_count++] = mod->entries[i];
    }
    g_registry_ready = 0; // Need re-sort
}

void toad_registry_init(void) {
    if (g_registry_ready) return;

    /* Register core module by default */
    toad_registry_add_module(toad_module_core());

    qsort(g_registry, g_registry_count, sizeof(toad_func_entry_t), entry_cmp);
    g_registry_ready = 1;
}

static const toad_func_entry_t *toad_registry_find_entry(
    const char *name) {
    if (!name) return NULL;
    if (!g_registry_ready) toad_registry_init();
    if (g_registry_count == 0) return NULL;

    return (const toad_func_entry_t *)bsearch(
        name, g_registry, g_registry_count,
        sizeof(toad_func_entry_t), search_cmp);
}

toad_builtin_fn toad_registry_find(const char *name) {
    const toad_func_entry_t *hit = toad_registry_find_entry(name);
    return hit ? hit->fn : NULL;
}

static const toad_func_entry_t *toad_find_builtin_entry(
    const char *name, toad_env_t *env) {
    const toad_func_entry_t *hit;
    const char *dot;

    if (!name) return NULL;
    if (env) {
        hit = mod_cache_find_entry(env, name);
        if (hit) return hit;

        dot = strchr(name, '.');
        if (dot) {
            hit = mod_find_entry_in_named_module(
                env, name, (size_t)(dot - name), dot + 1);
            if (hit) return hit;
        }
    }
    return toad_registry_find_entry(name);
}

toad_builtin_fn toad_find_builtin(const char *name, toad_env_t *env) {
    const toad_func_entry_t *hit = toad_find_builtin_entry(name, env);
    return hit ? hit->fn : NULL;
}

int toad_find_builtin_reflection(
    const char *name, toad_env_t *env, toad_function_reflection_t *out) {
    const toad_func_entry_t *entry;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    entry = toad_find_builtin_entry(name, env);
    return entry != NULL && toad_func_entry_reflect(entry, out);
}

toad_value_t toad_call_builtin(toad_builtin_fn fn, size_t argc,
                                   toad_value_t *args, toad_env_t *env) {
    mem_pool_t scratch;
    toad_value_t raw;
    toad_value_t promoted;

    if (!fn || !env || mem_init(&scratch, 0) != 0) {
        promoted = toad_val_num(0.0);
        if (env) {
            env->aborted = 1;
            snprintf(env->error_msg, sizeof(env->error_msg),
                     "failed to initialize builtin scratch storage");
        }
        return promoted;
    }
    raw = fn(argc, args, env, &scratch);
    promoted = toad_value_clone_to_env(raw, env);
    toad_value_destroy(&raw);
    mem_destroy(&scratch);
    return promoted;
}

toad_value_t toad_call_internal(const char *name, size_t argc,
                                    toad_value_t *args, toad_env_t *env) {
    toad_value_t stack_args[8];
    int normalized_needs_free = 0;
    toad_value_t *normalized_args = normalize_numeric_args(args, argc, stack_args, 8, &normalized_needs_free);
    toad_value_t *builtin_args = builtin_needs_raw_args(name) ? args : normalized_args;

    /* 1. Check native/script functions in environment */
    if (env) {
        toad_env_t *curr_env_iter = env;
        while (curr_env_iter) {
            toad_func_t *f = curr_env_iter->funcs;
            while (f) {
                if (strcmp(f->name, name) == 0) {
                    if (f->is_script) {
                        toad_value_t result = eval_script_function(f, argc, args, env, env);
                        if (normalized_needs_free) free(normalized_args);
                        return result;
                    } else {
                        toad_value_t *native_args =
                            (f->data.native.flags &
                             TOAD_NATIVE_PRESERVE_VALUE_TYPES)
                                ? args
                                : normalized_args;
                        toad_value_t raw =
                            f->data.native.fn(argc, native_args, env,
                                              f->data.native.user_data);
                        toad_value_t result = toad_value_clone_to_env(raw, env);
                        toad_value_destroy(&raw);
                        if (normalized_needs_free) free(normalized_args);
                        return result;
                    }
                }
                f = f->next;
            }
            curr_env_iter = curr_env_iter->parent;
        }
    }

    /* 1.25 Check if name is a variable holding a function value */
    if (env) {
        toad_value_t callee = toad_env_get(env, name);
        if (callee.type == TOAD_VAL_CLASS) {
            toad_value_t result =
                toad_oop_instantiate_class_value(callee, name, argc, args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
        if (callee.type == TOAD_VAL_BOUND_METHOD) {
            toad_value_t result = toad_oop_call_bound_method(callee, argc, args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
        if (callee.type == TOAD_VAL_FUNCTION && callee.data.function.body) {
            toad_value_t result = toad_call_function_value(callee, argc, args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
    }

    /* 1.5 Per-env module cache — O(log n) sorted-array lookup */
    if (env) {
        /* Try non-namespaced first */
        toad_builtin_fn mod_fn = mod_cache_find(env, name);
        if (mod_fn) {
            toad_value_t result = toad_call_builtin(mod_fn, argc, builtin_args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }

        /* Try namespaced: "module.func" only inside the named module. */
        const char *dot = strchr(name, '.');
        if (dot) {
            mod_fn = mod_find_in_named_module(env, name, (size_t)(dot - name), dot + 1);
            if (mod_fn) {
                toad_value_t result = toad_call_builtin(mod_fn, argc, builtin_args, env);
                if (normalized_needs_free) free(normalized_args);
                return result;
            }
        }
    }

    /* 2. Module registry — O(log n) sorted-array lookup */
    /* Try full name first (e.g., "vec.reverse") */
    toad_builtin_fn mod_fn = toad_registry_find(name);
    if (mod_fn) {
        toad_value_t result = toad_call_builtin(mod_fn, argc, builtin_args, env);
        if (normalized_needs_free) free(normalized_args);
        return result;
    }

    /* Check known global built-in namespaces too.  Plugin namespaces are handled
     * by env modules above and must not fall back to unrelated short names. */
    const char *final_dot = strchr(name, '.');
    if (final_dot && is_global_compat_namespace(name, (size_t)(final_dot - name))) {
        mod_fn = toad_registry_find(final_dot + 1);
        if (mod_fn) {
            toad_value_t result = toad_call_builtin(mod_fn, argc, builtin_args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
    }

    if (normalized_needs_free) free(normalized_args);
    return toad_call_undefined(env, name);
}
