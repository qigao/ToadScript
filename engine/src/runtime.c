/**
 * @file runtime.c
 * @brief runtime environment, value ownership, and symbol table support
 */

#include <toad/engine.h>
#include "internal.h"
#include "runtime_internal.h"
#include <toad/class.h>
#include <cstl.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

/* Global spin lock guarding the captured-scope closure chain shared by a
 * context root. The host may run scripts on one thread while timer/task
 * callbacks run on a coroutine event-loop thread; both mutate the chain
 * (snapshot insertion, reference counting, sweeping), so those critical
 * sections must be serialized. */
static atomic_flag toad_closure_chain_lock = ATOMIC_FLAG_INIT;

static void toad_closure_chain_acquire(void) {
    while (atomic_flag_test_and_set_explicit(&toad_closure_chain_lock,
                                             memory_order_acquire)) {
    }
}

static void toad_closure_chain_release(void) {
    atomic_flag_clear_explicit(&toad_closure_chain_lock, memory_order_release);
}

#ifdef _WIN32
#include <windows.h>
static uintptr_t toad_current_tid(void) { return (uintptr_t)GetCurrentThreadId(); }
#else
#include <pthread.h>
static uintptr_t toad_current_tid(void) { return (uintptr_t)pthread_self(); }
#endif

// Hash table entry for variables
typedef struct {
    char *name;
    toad_value_t value;
    int is_constant;
    int is_borrowed;
} toad_var_entry_t;

/*
 * Each environment owns its HashMap and the strdup'd variable names stored
 * in it.  The map byte-copies entries; the destroy path releases names and
 * non-borrowed values before releasing raw map storage.  Enumeration uses a
 * short-lived snapshot so no map value pointer crosses a mutation boundary.
 */
typedef size_t htab_size_t;
typedef size_t htab_hash_t;
typedef struct { void *data; size_t length; } toad_var_snapshot_t;
typedef struct { htab_hash_t hash; toad_var_entry_t el; } toad_var_entry_t_htab_el_t;
typedef struct {
    hash_map_t map;
    toad_var_snapshot_t els;
    htab_size_t els_bound;
    htab_size_t els_num;
} toad_var_entry_t_htab_t;

#define HTAB(T) T##_htab_t
#define HTAB_EL(T) T##_htab_el_t
#define VARR_ADDR(T, array) ((T *)((array).data))
#define HTAB_DELETED_HASH SIZE_MAX
#define HTAB_FIND 0
#define HTAB_INSERT 1
#define HTAB_REPLACE 2
#define HTAB_DELETE 3
#define HTAB_OP(T, operation) T##_htab_##operation

static size_t toad_var_name_hash(const void *key, size_t key_size, void *context) {
    const char *const *name = (const char *const *)key;
    size_t hash = 5381u;
    (void)key_size;
    (void)context;
    if (!name || !*name) return 0u;
    for (const unsigned char *p = (const unsigned char *)*name; *p; ++p)
        hash = ((hash << 5u) + hash) ^ *p;
    return hash;
}

static bool toad_var_name_equal(const void *left, const void *right,
                                  size_t key_size, void *context) {
    const char *const *lhs = (const char *const *)left;
    const char *const *rhs = (const char *const *)right;
    (void)key_size;
    (void)context;
    return lhs && rhs && *lhs && *rhs && strcmp(*lhs, *rhs) == 0;
}

static int toad_var_entry_t_htab_sync(HTAB(toad_var_entry_t) *table) {
    size_t count;
    toad_var_entry_t_htab_el_t *elements;
    if (!table) return 0;
    free(table->els.data);
    table->els.data = NULL;
    table->els.length = 0;
    count = hash_map_size(&table->map);
    table->els_bound = count;
    table->els_num = count;
    if (count == 0) return 1;
    elements = (toad_var_entry_t_htab_el_t *)calloc(count, sizeof(*elements));
    if (!elements) return 0;
    for (size_t slot = 0, out = 0; slot < hash_map_capacity(&table->map); ++slot) {
        const char *const *key =
            (const char *const *)hash_map_key_at_const(&table->map, slot);
        const toad_var_entry_t *value =
            (const toad_var_entry_t *)hash_map_value_at_const(&table->map, slot);
        if (!key || !value) continue;
        elements[out].hash = toad_var_name_hash(key, sizeof(*key), NULL);
        if (elements[out].hash == HTAB_DELETED_HASH) --elements[out].hash;
        elements[out].el = *value;
        ++out;
    }
    table->els.data = elements;
    table->els.length = count;
    return 1;
}

static HTAB(toad_var_entry_t) *toad_var_entry_t_htab_new(size_t entry_limit) {
    HTAB(toad_var_entry_t) *table = (HTAB(toad_var_entry_t) *)calloc(1, sizeof(*table));
    if (!table) return NULL;
    if (hash_map_init_bytes(&table->map, sizeof(char *), _Alignof(char *),
                            sizeof(toad_var_entry_t), _Alignof(toad_var_entry_t),
                            entry_limit, toad_var_name_hash,
                            toad_var_name_equal, NULL) != STL_OK) {
        free(table);
        return NULL;
    }
    return table;
}

static void toad_var_entry_t_htab_destroy(HTAB(toad_var_entry_t) **table) {
    if (!table || !*table) return;
    free((*table)->els.data);
    hash_map_raw_destroy_storage(&(*table)->map);
    free(*table);
    *table = NULL;
}

static int toad_var_entry_t_htab_do(HTAB(toad_var_entry_t) *table,
                                      toad_var_entry_t entry, int operation,
                                      toad_var_entry_t *result) {
    toad_var_entry_t *stored;
    toad_var_entry_t removed;
    if (!table || !entry.name) return 0;
    stored = (toad_var_entry_t *)hash_map_get(&table->map, &entry.name);
    if (operation == HTAB_FIND) {
        if (!stored) return 0;
        if (result) *result = *stored;
        return 1;
    }
    if (operation == HTAB_DELETE) {
        if (!stored || hash_map_remove(&table->map, &entry.name, &removed) != STL_OK) return 0;
        if (result) *result = removed;
        return toad_var_entry_t_htab_sync(table);
    }
    if ((operation == HTAB_INSERT && stored) ||
        (operation != HTAB_INSERT && operation != HTAB_REPLACE) ||
        hash_map_put(&table->map, &entry.name, &entry) != STL_OK) return 0;
    if (result) *result = entry;
    return toad_var_entry_t_htab_sync(table);
}

typedef struct {
    void **items;
    size_t count;
    size_t capacity;
} toad_ptr_set_t;

typedef struct {
    toad_ptr_set_t lists;
    toad_ptr_set_t maps;
    toad_ptr_set_t classes;
    toad_ptr_set_t instances;
    mem_pool_t *owner_arena;
} toad_release_state_t;

static int ptr_set_mark_seen(toad_ptr_set_t *set, void *ptr) {
    if (!set || !ptr) return 0;

    for (size_t i = 0; i < set->count; ++i) {
        if (set->items[i] == ptr) return 0;
    }

    if (set->count == set->capacity) {
        size_t new_capacity = set->capacity ? set->capacity * 2 : 8;
        void **new_items = (void**)realloc(set->items, new_capacity * sizeof(void*));
        if (!new_items) return 0;
        set->items = new_items;
        set->capacity = new_capacity;
    }

    set->items[set->count++] = ptr;
    return 1;
}

static void toad_release_state_destroy(toad_release_state_t *state) {
    if (!state) return;

    free(state->lists.items);
    free(state->maps.items);
    free(state->classes.items);
    free(state->instances.items);
    state->lists.items = NULL;
    state->maps.items = NULL;
    state->classes.items = NULL;
    state->instances.items = NULL;
    state->owner_arena = NULL;
    state->lists.count = 0;
    state->maps.count = 0;
    state->classes.count = 0;
    state->instances.count = 0;
    state->lists.capacity = 0;
    state->maps.capacity = 0;
    state->classes.capacity = 0;
    state->instances.capacity = 0;
}

static void toad_release_value(toad_value_t *value, toad_release_state_t *state);

typedef struct {
    toad_instance_t *instance;
    toad_env_t *env;
} toad_instance_clone_ctx_t;

typedef struct {
    toad_class_t *source;
    toad_class_t *klass;
    toad_env_t *env;
} toad_class_clone_ctx_t;

static void toad_clone_instance_field(const char *name, toad_value_t *value,
                                        void *user_data) {
    toad_instance_clone_ctx_t *ctx = (toad_instance_clone_ctx_t*)user_data;
    if (!ctx || !ctx->instance || !ctx->env || !name || !value) return;

    toad_value_t cloned = toad_value_clone_to_env(*value, ctx->env);
    toad_instance_set_field(ctx->instance, name, cloned);
    toad_value_destroy(&cloned);
}

static void toad_clone_class_static_field(const char *name, toad_value_t *value,
                                            void *user_data) {
    toad_class_clone_ctx_t *ctx = (toad_class_clone_ctx_t*)user_data;
    if (!ctx || !ctx->source || !ctx->klass || !ctx->env || !name || !value) return;

    toad_value_t cloned;
    if (value->type == TOAD_VAL_CLASS &&
        value->data.class_val.klass == ctx->source) {
        cloned = toad_val_class(ctx->klass);
    } else {
        cloned = toad_value_clone_to_env(*value, ctx->env);
    }

    {
        int access = toad_class_get_static_field_access(
            ctx->source, name, NULL);
        const char *declared_type = toad_class_get_static_field_type(
            ctx->source, name);
        toad_class_declare_static_field_typed(
            ctx->klass, name, declared_type, cloned, access);
    }
    toad_value_destroy(&cloned);
}

static void toad_clone_class_static_fields(toad_class_t *source,
                                             toad_class_t *klass,
                                             toad_env_t *env) {
    if (!source || !klass || !env) return;

    if (source->prototype && klass->prototype) {
        toad_clone_class_static_fields(source->prototype, klass->prototype, env);
    }

    toad_class_clone_ctx_t ctx = {
        .source = source,
        .klass = klass,
        .env = env
    };
    toad_class_foreach_named_static_field(source, toad_clone_class_static_field, &ctx);
}

toad_value_t toad_value_clone_to_env(toad_value_t value, toad_env_t *dst_env) {
    if (!dst_env) return value;

    switch (value.type) {
        case TOAD_VAL_STRING:
        case TOAD_VAL_BYTES:
        case TOAD_VAL_BIGINT:
        case TOAD_VAL_ENUM:
        case TOAD_VAL_FLAGS:
        case TOAD_VAL_VECTOR:
        case TOAD_VAL_TYPED_ARRAY: {
            toad_value_t copied = {0};
            if (toad_value_copy_to_env(value, dst_env, &copied) == 0) return copied;
            copied.type = TOAD_VAL_NULL;
            return copied;
        }
        case TOAD_VAL_LIST:
        case TOAD_VAL_SET: {
            toad_value_t cloned = value.type == TOAD_VAL_SET ? toad_val_set_empty()
                                                                 : toad_val_list_empty();
            if (!value.data.list.items || value.data.list.count == 0) return cloned;
            for (size_t i = 0; i < value.data.list.count; ++i) {
                toad_value_t child = toad_value_clone_to_env(
                    toad_value_borrow(value.data.list.items[i]), dst_env);
                if (dst_env->aborted || toad_list_push(&cloned, child) != 0) {
                    toad_value_destroy(&child);
                    toad_value_destroy(&cloned);
                    dst_env->aborted = 1;
                    snprintf(dst_env->error_msg, sizeof(dst_env->error_msg),
                             "failed to clone list value");
                    return (toad_value_t){ .type = TOAD_VAL_NULL };
                }
                toad_value_destroy(&child);
            }
            return cloned;
        }
        case TOAD_VAL_MAP:
        case TOAD_VAL_OBJECT: {
            toad_value_t cloned = value.type == TOAD_VAL_OBJECT
                ? toad_val_object()
                : toad_val_map();
            toad_map_iter_t it = toad_map_iter_begin(&value);
            const char *key;
            toad_value_t child;
            while (toad_map_iter_next(&it, &key, &child)) {
                toad_value_t cloned_child = toad_value_clone_to_env(child, dst_env);
                if (dst_env->aborted ||
                    toad_map_set(&cloned, key, cloned_child) != 0) {
                    toad_value_destroy(&cloned_child);
                    toad_value_destroy(&cloned);
                    dst_env->aborted = 1;
                    snprintf(dst_env->error_msg, sizeof(dst_env->error_msg),
                             "failed to clone map value");
                    return (toad_value_t){ .type = TOAD_VAL_NULL };
                }
                toad_value_destroy(&cloned_child);
            }
            return cloned;
        }
        case TOAD_VAL_CLASS: {
            toad_class_t *src = value.data.class_val.klass;
            if (!src || src->arena == &dst_env->arena) return value;

            toad_class_t *copy = toad_class_clone_to_arena(src, &dst_env->arena);
            if (!copy) {
                toad_value_t null_value;
                memset(&null_value, 0, sizeof(null_value));
                null_value.type = TOAD_VAL_NULL;
                return null_value;
            }

            toad_clone_class_static_fields(src, copy, dst_env);
            return toad_val_class(copy);
        }
        case TOAD_VAL_INSTANCE: {
            toad_instance_t *src = value.data.instance_val.instance;
            if (!src || src->arena == &dst_env->arena) return value;

            toad_class_t *klass = toad_class_clone_to_arena(src->klass, &dst_env->arena);
            if (!klass) {
                toad_value_t null_value;
                memset(&null_value, 0, sizeof(null_value));
                null_value.type = TOAD_VAL_NULL;
                return null_value;
            }
            toad_clone_class_static_fields(src->klass, klass, dst_env);

            toad_instance_t *copy = toad_instance_create(klass, &dst_env->arena);
            if (!copy) {
                toad_value_t null_value;
                memset(&null_value, 0, sizeof(null_value));
                null_value.type = TOAD_VAL_NULL;
                return null_value;
            }

            toad_instance_clone_ctx_t ctx = { .instance = copy, .env = dst_env };
            toad_instance_foreach_named_field(src, toad_clone_instance_field, &ctx);
            return toad_val_instance(copy);
        }
        case TOAD_VAL_BOUND_METHOD: {
            toad_value_t instance_value =
                toad_val_instance(value.data.bound_method_val.instance);
            toad_value_t cloned_instance =
                toad_value_clone_to_env(instance_value, dst_env);
            if (cloned_instance.type != TOAD_VAL_INSTANCE) return cloned_instance;

            toad_func_t *method = value.data.bound_method_val.method;
            if (method && method->owner_class &&
                method->owner_class != cloned_instance.data.instance_val.instance->klass) {
                toad_func_t *cloned_method =
                    toad_class_lookup_method_arity(
                        cloned_instance.data.instance_val.instance->klass,
                        method->name, 0, method->data.script.arg_count);
                if (cloned_method) method = cloned_method;
            }

            return toad_val_bound_method(cloned_instance.data.instance_val.instance,
                                           method);
        }
        case TOAD_VAL_FUNCTION:
            /* A cloned function value independently owns the captured-scope
             * reference; every release path drops one reference per clone.
             * With capture-by-need snapshots this no longer chain-captures the
             * previous closure value, and it keeps captured variables alive. */
            return toad_value_retain(value);
        default:
            return value;
    }
}

static int toad_value_is_reference_type(toad_value_t value) {
    switch (value.type) {
        case TOAD_VAL_CLASS:
        case TOAD_VAL_INSTANCE:
        case TOAD_VAL_BOUND_METHOD:
        case TOAD_VAL_FUNCTION:
        case TOAD_VAL_COROUTINE:
            return 1;
        default:
            return 0;
    }
}

static toad_value_t toad_value_store_to_env(toad_value_t value, toad_env_t *dst_env) {
    if (toad_value_is_reference_type(value)) {
        /* The environment entry becomes a reference holder: function values
         * take one reference on the captured scope so replacing or freeing the
         * entry can drop it symmetrically in toad_env_release_stored_value. */
        if (value.type == TOAD_VAL_FUNCTION && value.data.function.closure_env) {
            toad_env_retain(value.data.function.closure_env);
            value.ownership = TOAD_VALUE_OWNED;
        }
        return value;
    }

    if (value.ownership == TOAD_VALUE_OWNED &&
        (value.storage || value.storage_aux) &&
        (!value.storage || mem_buffer_pool(value.storage) == &dst_env->arena) &&
        (!value.storage_aux || mem_buffer_pool(value.storage_aux) == &dst_env->arena))
        return value;

    /* Containers own their storage but retain object identity for nested reference
     * values. This is required for collections of observers, callbacks, and class
     * instances; deep-cloning those elements would change identity and can bind
     * them to the lifetime of a temporary method environment. */
    if (value.type == TOAD_VAL_LIST || value.type == TOAD_VAL_SET) {
        toad_value_t stored = value.type == TOAD_VAL_SET ? toad_val_set_empty()
                                                             : toad_val_list_empty();
        if (!value.data.list.items || value.data.list.count == 0) return stored;
        stored.data.list.items = (toad_value_t *)mem_alloc_array(
            &dst_env->arena, sizeof(*stored.data.list.items), value.data.list.count);
        if (!stored.data.list.items) return stored;
        stored.data.list.count = value.data.list.count;
        stored.data.list.capacity = value.data.list.count;
        for (size_t i = 0; i < value.data.list.count; ++i)
            stored.data.list.items[i] = toad_value_store_to_env(
                toad_value_borrow(value.data.list.items[i]), dst_env);
        return stored;
    }
    if (toad_value_is_object_like(&value)) {
        toad_value_t stored = value.type == TOAD_VAL_OBJECT ? toad_val_object()
                                                                : toad_val_map();
        toad_map_iter_t it = toad_map_iter_begin(&value);
        const char *key;
        toad_value_t child;
        while (toad_map_iter_next(&it, &key, &child)) {
            toad_value_t stored_child = toad_value_store_to_env(child, dst_env);
            toad_map_set(&stored, key, stored_child);
            toad_value_destroy(&stored_child);
        }
        return stored;
    }
    return toad_value_clone_to_env(value, dst_env);
}

static void toad_env_release_stored_value(toad_env_t *env, toad_value_t *value) {
    toad_release_state_t state;

    if (!env || !value) return;
    if (toad_value_is_reference_type(*value)) {
        if (value->type == TOAD_VAL_FUNCTION && value->ownership == TOAD_VALUE_OWNED &&
            value->data.function.closure_env) {
            toad_env_release(value->data.function.closure_env);
            value->data.function.closure_env = NULL;
        }
        return;
    }

    memset(&state, 0, sizeof(state));
    state.owner_arena = &env->arena;
    toad_release_value(value, &state);
    toad_release_state_destroy(&state);
}

static void toad_env_release_entry_value(toad_env_t *env, toad_var_entry_t *entry) {
    if (!entry || entry->is_borrowed) return;
    toad_env_release_stored_value(env, &entry->value);
}

void toad_env_set_local(toad_env_t *env, const char *name, toad_value_t value) {
    if (!env || !name) return;

    HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)env->vars;
    toad_var_entry_t key = { .name = (char*)name };
    toad_var_entry_t result;

    if (HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        if (result.is_constant) return;
        toad_value_t stored = toad_value_store_to_env(value, env);
        toad_env_release_entry_value(env, &result);
        result.value = stored;
        result.is_borrowed = 0;
        HTAB_OP(toad_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    } else {
        toad_value_t stored = toad_value_store_to_env(value, env);
        toad_var_entry_t new_entry = {
            .name = strdup(name),
            .value = stored,
            .is_constant = 0,
            .is_borrowed = 0
        };
        HTAB_OP(toad_var_entry_t, do)(htab, new_entry, HTAB_INSERT, &result);
    }
}

static void toad_env_set_local_cloned(toad_env_t *env, const char *name,
                                        toad_value_t value) {
    if (!env || !name) return;

    HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)env->vars;
    toad_var_entry_t key = { .name = (char*)name };
    toad_var_entry_t result;

    if (HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        if (result.is_constant) return;
        toad_value_t stored = toad_value_clone_to_env(value, env);
        toad_env_release_entry_value(env, &result);
        result.value = stored;
        result.is_borrowed = 0;
        HTAB_OP(toad_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    } else {
        toad_value_t stored = toad_value_clone_to_env(value, env);
        toad_var_entry_t new_entry = {
            .name = strdup(name),
            .value = stored,
            .is_constant = 0,
            .is_borrowed = 0
        };
        HTAB_OP(toad_var_entry_t, do)(htab, new_entry, HTAB_INSERT, &result);
    }
}

void toad_env_set_local_borrowed(toad_env_t *env, const char *name,
                                          toad_value_t value) {
    if (!env || !name) return;

    HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)env->vars;
    toad_var_entry_t key = { .name = (char*)name };
    toad_var_entry_t result;

    if (HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        if (result.is_constant) return;
        toad_env_release_entry_value(env, &result);
        result.value = value;
        result.is_borrowed = 1;
        HTAB_OP(toad_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    } else {
        toad_var_entry_t new_entry = {
            .name = strdup(name),
            .value = value,
            .is_constant = 0,
            .is_borrowed = 1
        };
        HTAB_OP(toad_var_entry_t, do)(htab, new_entry, HTAB_INSERT, &result);
    }
}

void toad_env_set(toad_env_t *env, const char *name, toad_value_t value) {
    if (!env || !name) return;

    toad_env_t *curr_env = env;
    while (curr_env) {
        HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)curr_env->vars;
        toad_var_entry_t key = { .name = (char*)name };
        toad_var_entry_t result;

        if (HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
            if (result.is_constant) return;
            toad_value_t stored = toad_value_store_to_env(value, curr_env);
            toad_env_release_entry_value(curr_env, &result);
            result.value = stored;
            result.is_borrowed = 0;
            HTAB_OP(toad_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
            return;
        }
        curr_env = curr_env->parent;
    }

    // Not found in any scope, create in current scope
    toad_env_set_local(env, name, value);
}


void eval_destructure(toad_node_t *target, toad_value_t rhs, toad_env_t *env, int is_constant) {
    if (!target || !env) return;

    if (target->type == TOAD_NODE_VARIABLE) {
        if (is_constant) toad_env_set_constant(env, target->data.variable.name, rhs);
        else toad_env_set_local(env, target->data.variable.name, rhs);
    } else if (target->type == TOAD_NODE_VECTOR) {
        if (rhs.type != TOAD_VAL_VECTOR) return;
        size_t rhs_idx = 0;
        for (size_t i = 0; i < target->data.vector.count; ++i) {
            toad_node_t *el = target->data.vector.elements[i];
            if (el->type == TOAD_NODE_SPREAD) {
                toad_node_t *child = el->data.spread.child;
                if (child->type == TOAD_NODE_VARIABLE) {
                    size_t rest_sz = (rhs_idx < rhs.data.vector.size) ? (rhs.data.vector.size - rhs_idx) : 0;
                    toad_value_t rest_val;
                    if (toad_value_copy_to_env(
                            toad_val_vec(rest_sz ? rhs.data.vector.data + rhs_idx : NULL,
                                           rest_sz),
                            env, &rest_val) != 0)
                        return;
                    if (is_constant)
                        toad_env_set_constant(env, child->data.variable.name, rest_val);
                    else
                        toad_env_set(env, child->data.variable.name, rest_val);
                    rhs_idx = rhs.data.vector.size;
                }
            } else if (el->type == TOAD_NODE_NULL) {
                rhs_idx++;
            } else {
                toad_value_t val = (rhs_idx < rhs.data.vector.size) ?
                    toad_val_num(rhs.data.vector.data[rhs_idx++]) : toad_val_num(0);
                eval_destructure(el, val, env, is_constant);
            }
        }
    } else if (target->type == TOAD_NODE_MAP_LITERAL) {
        if (!toad_value_is_object_like(&rhs)) return;
        toad_node_t *rest_node = NULL;
        for (size_t i = 0; i < target->data.map_literal.count; ++i) {
            char *key = target->data.map_literal.keys[i];
            if (key) {
                toad_value_t prop_val = toad_map_get(&rhs, key);
                eval_destructure(target->data.map_literal.values[i], prop_val, env, is_constant);
            } else {
                rest_node = target->data.map_literal.values[i];
            }
        }
        if (rest_node) {
            toad_value_t rest_map = toad_val_map();
            toad_map_iter_t it = toad_map_iter_begin(&rhs);
            const char *rhs_key;
            toad_value_t rhs_val;
            while (toad_map_iter_next(&it, &rhs_key, &rhs_val)) {
                int matched = 0;
                for (size_t i = 0; i < target->data.map_literal.count; ++i) {
                    if (target->data.map_literal.keys[i] && strcmp(target->data.map_literal.keys[i], rhs_key) == 0) {
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    toad_map_set(&rest_map, rhs_key, rhs_val);
                }
            }
            eval_destructure(rest_node, rest_map, env, is_constant);
            toad_value_destroy(&rest_map);
        }
    } else if (target->type == TOAD_NODE_SPREAD) {
        toad_node_t *child = target->data.spread.child;
        if (child->type == TOAD_NODE_VARIABLE) {
            if (is_constant) toad_env_set_constant(env, child->data.variable.name, rhs);
            else toad_env_set(env, child->data.variable.name, rhs);
        }
    }
}

// Symbol Table Implementation
static void toad_env_init_storage(toad_env_t *env) {
    if (env) {
        memset(env, 0, sizeof(toad_env_t));

        // Environment-owned raw CSTL table; retain source-compatible capacity.
        HTAB(toad_var_entry_t) *htab =
            toad_var_entry_t_htab_new(SIZE_MAX);
        env->vars = htab;
        env->funcs = NULL;

        env->modules = NULL;
        env->module_count = 0;
        env->flow = toad_FLOW_NORMAL;
        env->return_value.type = TOAD_VAL_NUMBER;
        env->return_value.data.number = 0.0;
        env->parent = NULL;
        mem_init(&env->arena, 65536); // 64KB initial

        // Safety Limits Defaults
        env->max_recursion = 100;
        env->curr_recursion = 0;
        env->max_loop_iterations = 10000;
        env->curr_loop_iterations = 0;
        env->max_nodes = 100000;
        env->curr_nodes = 0;
        env->aborted = 0;
        env->max_external_value_bytes = 4U * 1024U * 1024U;

        // Error reporting
        env->error_msg[0] = '\0';
        env->error_line = 0;
        env->error_column = 0;
    }
}

void toad_env_init_child(toad_env_t *env, toad_env_t *parent) {
    if (!env || !parent) return;
    toad_env_init_storage(env);
    env->parent = parent;
    env->eval_node = parent->eval_node;
    env->exec_script_body = parent->exec_script_body;
    env->safe_point = parent->safe_point;
    env->safe_point_user_data = parent->safe_point_user_data;
    env->max_recursion = parent->max_recursion;
    env->max_loop_iterations = parent->max_loop_iterations;
    env->max_nodes = parent->max_nodes;
    env->max_external_value_bytes = parent->max_external_value_bytes;
}

void toad_env_init(toad_env_t *env) {
    if (env) {
        toad_env_init_storage(env);

        // Built-in constants (set_constant calls env_set internally)
        toad_env_set_constant(env, "pi", toad_val_num(3.14159265358979323846));
        toad_env_set_constant(env, "e", toad_val_num(2.71828182845904523536));
        toad_env_set_constant(env, "inf", toad_val_num(INFINITY));
        toad_env_set_constant(env, "nan", toad_val_num(NAN));
        toad_env_set_constant(env, "true", toad_val_bool(1));
        toad_env_set_constant(env, "false", toad_val_bool(0));
    }
}

toad_env_t *toad_env_snapshot_names(toad_env_t *env, const char *const *names,
                                        size_t name_count) {
    if (!env) return NULL;
    toad_env_t *root = env;
    while(root->parent) root = root->parent;

    toad_env_t *new_env = (toad_env_t *)malloc(sizeof(toad_env_t));
    if (!new_env) return NULL;

    // Initialize with hash table
    toad_env_init_storage(new_env);
    new_env->max_external_value_bytes = env->max_external_value_bytes;
    new_env->eval_node = env->eval_node;
    new_env->exec_script_body = env->exec_script_body;

    /* Copy selected variables from env and its parents. When names == NULL the
     * whole environment is copied (class closures, module imports). Closure
     * snapshots pass their free-variable list so repeated closure assignment
     * does not chain-capture the previous closure value. */
    toad_env_t *curr_old = env;
    while (curr_old) {
        if (curr_old->vars) {
            // Copy from hash table
            HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)curr_old->vars;
            HTAB_EL(toad_var_entry_t) *els_addr = VARR_ADDR(HTAB_EL(toad_var_entry_t), htab->els);
            htab_size_t bound = htab->els_bound;

            for (htab_size_t i = 0; i < bound; i++) {
                if (els_addr[i].hash != HTAB_DELETED_HASH) {
                    toad_var_entry_t entry = els_addr[i].el;
                    if (names) {
                        size_t n;
                        int wanted = 0;
                        for (n = 0; n < name_count; ++n) {
                            if (names[n] && entry.name && strcmp(names[n], entry.name) == 0) {
                                wanted = 1;
                                break;
                            }
                        }
                        if (!wanted) continue;
                    }
                    // Check if already exists in new_env via direct hash lookup
                    HTAB(toad_var_entry_t) *new_htab = (HTAB(toad_var_entry_t)*)new_env->vars;
                    toad_var_entry_t probe = { .name = entry.name };
                    toad_var_entry_t found;
                    int exists = HTAB_OP(toad_var_entry_t, do)(new_htab, probe, HTAB_FIND, &found);

                    if (!exists) {
                        toad_env_set_local_cloned(new_env, entry.name, entry.value);
                        if (entry.is_constant) {
                            toad_env_set_constant(new_env, entry.name, entry.value);
                        }
                    }
                }
            }
        }
        curr_old = curr_old->parent;
    }

    /* Attach to root's closure list. The chain owns one reference (ref_count
     * starts at 1); function values and class methods add further references
     * through toad_env_retain() and drop them through toad_env_release().
     * toad_env_sweep_closures() reclaims snapshots with no live references. */
    toad_closure_chain_acquire();
    new_env->next_closure = root->next_closure;
    new_env->closure_root = root;
    new_env->ref_count = 1;
    new_env->is_class_closure = 0;
    new_env->closure_owner_tid = toad_current_tid();
    root->next_closure = new_env;
    toad_closure_chain_release();

    return new_env;
}

toad_env_t *toad_env_snapshot(toad_env_t *env) {
    return toad_env_snapshot_names(env, NULL, 0);
}

void toad_env_retain(toad_env_t *env) {
    /* Only captured snapshot scopes carry a reference count. Root, child and
     * task-local scopes use ref_count == 0 and are managed by their owners. */
    if (!env) return;
    toad_closure_chain_acquire();
    if (env->ref_count > 0) env->ref_count++;
    toad_closure_chain_release();
}

void toad_env_release(toad_env_t *env) {
    if (!env) return;
    toad_closure_chain_acquire();
    if (env->ref_count <= 0) {
        toad_closure_chain_release();
        return;
    }
    if (--env->ref_count > 0) {

        toad_closure_chain_release();
        return;
    }
    toad_closure_chain_release();

    /* Last reference dropped. The snapshot stays linked with ref_count == 0
     * and is reclaimed by toad_env_sweep_closures() or the final env free.
     * Deferring the actual free keeps the chain mutation on the sweeping
     * thread, which is safe when timer/task callbacks run concurrently. */
}

void toad_env_sweep_closures(toad_env_t *root) {
    uintptr_t tid = toad_current_tid();
    toad_env_t *swept = NULL;
    toad_env_t **link;

    if (!root) return;

    /* Reclaim only snapshots created by the calling thread. ref_count == 1
     * means only the chain owns it; ref_count == 0 means a dropped reference
     * is pending collection. Either way no function value or method references
     * this snapshot anymore. The owner-thread filter keeps a sweeping thread
     * from racing with another thread that is mid-way between creating a
     * snapshot and storing (and thus retaining) it.
     * Unlink under the chain lock; release outside it because
     * toad_env_free() re-enters the lock while dropping the references held
     * by the snapshot's own variables. */
    toad_closure_chain_acquire();
    link = &root->next_closure;
    while (*link) {
        toad_env_t *closure = *link;
        toad_env_t *next = closure->next_closure;
        if (closure->closure_owner_tid == tid && closure->ref_count <= 1) {
            *link = next;
            closure->next_closure = swept;
            swept = closure;
        } else {
            link = &closure->next_closure;
        }
    }
    toad_closure_chain_release();

    while (swept) {
        toad_env_t *closure = swept;
        swept = closure->next_closure;
        closure->next_closure = NULL;
        toad_env_free(closure);
        free(closure);
    }
}

static void toad_release_map_value(toad_value_t *map, toad_release_state_t *state) {
    if (!toad_value_is_object_like(map) || !map->data.map.htab) return;
    if (!ptr_set_mark_seen(&state->maps, map->data.map.htab)) return;

    toad_map_iter_t it = toad_map_iter_begin(map);
    const char *key;
    toad_value_t child;
    while (toad_map_iter_next(&it, &key, &child)) {
        toad_value_t *slot = toad_map_get_ptr(map, key);
        if (slot) toad_release_value(slot, state);
    }

    toad_map_free(map);
}

static void toad_release_list_value(toad_value_t *list, toad_release_state_t *state) {
    if (!list || (list->type != TOAD_VAL_LIST && list->type != TOAD_VAL_SET) ||
        !list->data.list.items)
        return;
    if (!ptr_set_mark_seen(&state->lists, list->data.list.items)) return;

    for (size_t i = 0; i < list->data.list.count; ++i) {
        toad_release_value(&list->data.list.items[i], state);
    }

    if (list->data.list.heap_owned) {
        vec_t vec = {
            .data = list->data.list.items,
            .size = list->data.list.count,
            .capacity = list->data.list.capacity,
            .elem_size = sizeof(toad_value_t),
            .elem_stride = sizeof(toad_value_t),
            .elem_align = _Alignof(toad_value_t),
            .element_limit = SIZE_MAX / sizeof(toad_value_t),
            .initialized = true,
        };
        vec_destroy(&vec);
    }
    list->data.list.items = NULL;
    list->data.list.count = 0;
    list->data.list.capacity = 0;
    list->data.list.heap_owned = 0;
    if (list->data.list.value_pool) {
        mem_destroy(list->data.list.value_pool);
        free(list->data.list.value_pool);
        list->data.list.value_pool = NULL;
    }
}

static void toad_release_oop_field(toad_value_t *value, void *user_data) {
    toad_release_value(value, (toad_release_state_t*)user_data);
}

static void toad_release_owned_instance(toad_instance_t *instance,
                                          toad_release_state_t *state) {
    if (!instance || !state || instance->arena != state->owner_arena) return;
    if (!ptr_set_mark_seen(&state->instances, instance)) return;

    toad_instance_foreach_field(instance, toad_release_oop_field, state);
    toad_instance_destroy(instance);
}

static void toad_release_value(toad_value_t *value, toad_release_state_t *state) {
    if (!value || !state) return;

    toad_value_release_storage(value);

    switch (value->type) {
        case TOAD_VAL_LIST:
        case TOAD_VAL_SET:
            toad_release_list_value(value, state);
            break;
        case TOAD_VAL_MAP:
        case TOAD_VAL_OBJECT:
            toad_release_map_value(value, state);
            break;
        case TOAD_VAL_CLASS: {
            toad_class_t *klass = value->data.class_val.klass;
            if (klass && klass->arena == state->owner_arena &&
                ptr_set_mark_seen(&state->classes, klass)) {
                toad_class_foreach_static_field(klass, toad_release_oop_field, state);
                toad_class_destroy(klass);
            }
            break;
        }
        case TOAD_VAL_INSTANCE:
            toad_release_owned_instance(value->data.instance_val.instance, state);
            break;
        case TOAD_VAL_BOUND_METHOD:
            toad_release_owned_instance(value->data.bound_method_val.instance, state);
            break;
        case TOAD_VAL_FUNCTION:
            if (value->ownership == TOAD_VALUE_OWNED && value->data.function.closure_env) {
                toad_env_release(value->data.function.closure_env);
                value->data.function.closure_env = NULL;
            }
            break;
        case TOAD_VAL_TYPED_ARRAY:
            if (value->data.typed_array.heap_owned) free(value->data.typed_array.data);
            value->data.typed_array.data = NULL;
            value->data.typed_array.count = 0;
            value->data.typed_array.heap_owned = 0;
            break;
        default:
            break;
    }
}

// Helper to manually free hash table entries
static void free_htab_entries(HTAB(toad_var_entry_t) *htab, toad_release_state_t *state) {
    if (!htab) return;

    // Iterate through all entries and free them manually
    HTAB_EL(toad_var_entry_t) *els_addr = VARR_ADDR(HTAB_EL(toad_var_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; i++) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            toad_var_entry_t entry = els_addr[i].el;
            free(entry.name);
            if (!entry.is_borrowed) {
                toad_release_value(&entry.value, state);
            }
        }
    }
}

static void toad_env_free_internal(toad_env_t *env, toad_release_state_t *state) {
    if (!env) return;

    mem_pool_t *previous_owner_arena = state->owner_arena;
    state->owner_arena = &env->arena;

    /* Release variable values first. Function values drop their captured
     * scope references here; a snapshot whose last reference was the closure
     * chain is unlinked and freed recursively inside toad_env_release(). */
    if (env->vars) {
        HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)env->vars;
        free_htab_entries(htab, state);
        HTAB_OP(toad_var_entry_t, destroy)(&htab);
        env->vars = NULL;
    }

    /* Release script function closure references before the chain sweep. */
    toad_func_t *fcurr = env->funcs;
    while (fcurr) {
        toad_func_t *fnext = fcurr->next;
        free(fcurr->name);
        if (fcurr->is_script && fcurr->data.script.arg_params) {
            free(fcurr->data.script.arg_params);
        }
        toad_env_release(fcurr->closure_env);
        free(fcurr);
        fcurr = fnext;
    }
    env->funcs = NULL;

    if (env->modules) {
        free((void *)env->modules);
        env->modules = NULL;
    }
    env->module_count = 0;
    if (env->mod_cache) {
        free(env->mod_cache);
        env->mod_cache = NULL;
    }
    env->mod_cache_count = 0;
    if (env->regex_ctx) {
        toad_regex_ctx_destroy(env->regex_ctx);
        env->regex_ctx = NULL;
    }

    /* Free remaining captured snapshots. Snapshots only reference scopes that
     * predate them, so walking the chain from head (newest) to tail (oldest)
     * releases cross references before the referenced snapshot is freed.
     * Unlink under the chain lock and free outside it: freeing a snapshot can
     * drop references that link further orphans back onto the chain, so the
     * unlink/free loop repeats until the chain is empty. */
    for (;;) {
        toad_env_t *closure;

        toad_closure_chain_acquire();
        closure = env->next_closure;
        env->next_closure = NULL;
        toad_closure_chain_release();
        if (!closure) break;

        while (closure) {
            toad_env_t *next = closure->next_closure;
            closure->next_closure = NULL;
            toad_env_free_internal(closure, state);
            free(closure);
            closure = next;
        }
    }
    mem_destroy(&env->arena);
    state->owner_arena = previous_owner_arena;
}

void toad_env_free(toad_env_t *env) {
    toad_release_state_t state;

    memset(&state, 0, sizeof(state));
    toad_env_free_internal(env, &state);
    toad_release_state_destroy(&state);
}

toad_value_t toad_env_get(toad_env_t *env, const char *name) {
    toad_value_t val = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!env || !name) return val;

    toad_env_t *curr_env = env;
    while (curr_env) {
        HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)curr_env->vars;
        toad_var_entry_t key = { .name = (char*)name };
        toad_var_entry_t result;

        if (HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
            return toad_value_borrow(result.value);
        }
        curr_env = curr_env->parent;
    }
    return val;
}

int toad_env_has(toad_env_t *env, const char *name) {
    if (!env || !name) return 0;
    toad_env_t *curr_env = env;
    while (curr_env) {
        if (curr_env->vars) {
            HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)curr_env->vars;
            toad_var_entry_t key = { .name = (char*)name };
            toad_var_entry_t result;
            if (HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result))
                return 1;
        }
        curr_env = curr_env->parent;
    }
    return 0;
}

static int toad_env_has_local_var(toad_env_t *env, const char *name) {
    if (!env || !name || !env->vars) return 0;

    HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)env->vars;
    toad_var_entry_t key = { .name = (char*)name };
    toad_var_entry_t result;
    return HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result) ? 1 : 0;
}

void toad_env_import_vars(toad_env_t *dst, toad_env_t *src) {
    if (!dst || !src || !src->vars) return;

    HTAB(toad_var_entry_t) *src_htab = (HTAB(toad_var_entry_t)*)src->vars;
    HTAB_EL(toad_var_entry_t) *els_addr =
        VARR_ADDR(HTAB_EL(toad_var_entry_t), src_htab->els);
    htab_size_t bound = src_htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash == HTAB_DELETED_HASH) continue;
        toad_var_entry_t entry = els_addr[i].el;
        if (!entry.name || toad_env_has_local_var(dst, entry.name)) continue;
        if (strcmp(entry.name, "this") == 0) continue;

        toad_env_set_local(dst, entry.name, toad_value_borrow(entry.value));
        if (entry.is_constant)
            toad_env_set_constant(dst, entry.name, toad_value_borrow(entry.value));
    }
}

toad_value_t toad_env_eval_node(const toad_node_t *node, toad_env_t *env) {
    if (env && env->eval_node) return env->eval_node(node, env);
    return toad_eval(node, env);
}

static void toad_registration_chain_free(toad_func_t *head) {
    while (head) {
        toad_func_t *next = head->next;
        free(head->name);
        free(head);
        head = next;
    }
}

toad_registration_status_t toad_env_register_funcs_checked_with_fault(
    toad_env_t *env, const toad_native_registration_t *registrations,
    size_t registration_count, toad_registration_fault_fn should_fail,
    void *fault_user_data) {
    toad_func_t *pending_head = NULL;
    toad_func_t *pending_tail = NULL;
    size_t allocation_index = 0;

    if (!env || (registration_count != 0 && !registrations))
        return TOAD_REGISTRATION_INVALID_ARGUMENT;
    for (size_t i = 0; i < registration_count; ++i) {
        if (!registrations[i].name || !registrations[i].fn ||
            (registrations[i].flags & ~TOAD_NATIVE_FLAG_MASK) != 0u)
            return TOAD_REGISTRATION_INVALID_ARGUMENT;
        if (toad_env_has_func(env, registrations[i].name))
            return TOAD_REGISTRATION_CONFLICT;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(registrations[i].name, registrations[j].name) == 0)
                return TOAD_REGISTRATION_CONFLICT;
        }
    }

    for (size_t i = 0; i < registration_count; ++i) {
        size_t name_size = strlen(registrations[i].name);
        toad_func_t *new_func;
        char *owned_name;

        if (should_fail && should_fail(allocation_index, fault_user_data))
            new_func = NULL;
        else
            new_func = (toad_func_t *)malloc(sizeof(*new_func));
        allocation_index++;
        if (!new_func) {
            toad_registration_chain_free(pending_head);
            return TOAD_REGISTRATION_OUT_OF_MEMORY;
        }
        memset(new_func, 0, sizeof(*new_func));

        if (name_size == SIZE_MAX ||
            (should_fail && should_fail(allocation_index, fault_user_data)))
            owned_name = NULL;
        else
            owned_name = (char *)malloc(name_size + 1);
        allocation_index++;
        if (!owned_name) {
            free(new_func);
            toad_registration_chain_free(pending_head);
            return TOAD_REGISTRATION_OUT_OF_MEMORY;
        }
        memcpy(owned_name, registrations[i].name, name_size + 1);

        new_func->name = owned_name;
        new_func->is_script = 0;
        new_func->access_level = TOAD_ACCESS_PUBLIC;
        new_func->data.native.fn = registrations[i].fn;
        new_func->data.native.user_data = registrations[i].user_data;
        new_func->data.native.flags = registrations[i].flags;
        new_func->next = pending_head;
        pending_head = new_func;
        if (!pending_tail) pending_tail = new_func;
    }

    if (pending_tail) {
        pending_tail->next = env->funcs;
        env->funcs = pending_head;
    }
    return TOAD_REGISTRATION_OK;
}

toad_registration_status_t toad_env_register_funcs_checked(
    toad_env_t *env, const toad_native_registration_t *registrations,
    size_t registration_count) {
    return toad_env_register_funcs_checked_with_fault(
        env, registrations, registration_count, NULL, NULL);
}

void toad_env_register_func(toad_env_t *env, const char *name, toad_native_fn fn, void *user_data) {
    if (!env || !name || !fn) return;
    toad_func_t *new_func = (toad_func_t*)calloc(1, sizeof(toad_func_t));
    new_func->name = strdup(name);
    new_func->is_script = 0;
    new_func->access_level = TOAD_ACCESS_PUBLIC;
    new_func->data.native.fn = fn;
    new_func->data.native.user_data = user_data;
    new_func->data.native.flags = TOAD_NATIVE_FLAG_NONE;
    new_func->next = env->funcs;
    env->funcs = new_func;
}

int toad_env_has_func(toad_env_t *env, const char *name) {
    if (!env || !name) return 0;
    for (toad_env_t *curr_env = env; curr_env; curr_env = curr_env->parent) {
        for (toad_func_t *f = curr_env->funcs; f; f = f->next) {
            if (f->name && strcmp(f->name, name) == 0) return 1;
        }
    }
    return 0;
}

void toad_env_set_constant(toad_env_t *env, const char *name, toad_value_t value) {
    toad_env_set(env, name, value);

    HTAB(toad_var_entry_t) *htab = (HTAB(toad_var_entry_t)*)env->vars;
    toad_var_entry_t key = { .name = (char*)name };
    toad_var_entry_t result;

    if (HTAB_OP(toad_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        result.is_constant = 1;
        HTAB_OP(toad_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    }
}

void toad_env_add_module(toad_env_t *env, const toad_module_t *mod) {
    if (!env || !mod) return;
    const toad_module_t **new_arr = (const toad_module_t **)realloc(
        (void *)env->modules, (env->module_count + 1) * sizeof(toad_module_t *));
    if (!new_arr) return;
    new_arr[env->module_count] = mod;
    env->modules = new_arr;
    env->module_count++;

    /* Invalidate sorted cache — will be rebuilt on next call */
    if (env->mod_cache) { free(env->mod_cache); env->mod_cache = NULL; }
    env->mod_cache_count = 0;
}

// Error throwing helper (Phase 2)
toad_value_t throw_error(toad_env_t *env, const toad_node_t *node, const char *fmt, ...) {
    if (!env) return toad_val_num(0);

    va_list args;
    va_start(args, fmt);
    vsnprintf(env->error_msg, sizeof(env->error_msg), fmt, args);
    va_end(args);

    env->error_line = node ? node->line : env->last_line;
    env->error_column = node ? node->column : env->last_column;
    env->flow = toad_FLOW_THROW;
    env->error_value = toad_val_str(vstr_from_cstr(env->error_msg));

    return toad_val_num(0);
}

// Type name helper (Phase 2)
const char* type_name(int type) {
    switch (type) {
        case TOAD_VAL_NUMBER: return "number";
        case TOAD_VAL_INTEGER: return "int64";
        case TOAD_VAL_BOOL: return "bool";
        case TOAD_VAL_STRING: return "string";
        case TOAD_VAL_BYTES: return "bytes";
        case TOAD_VAL_VECTOR: return "vector";
        case TOAD_VAL_MAP: return "map";
        case TOAD_VAL_OBJECT: return "object";
        case TOAD_VAL_NULL: return "null";
        case TOAD_VAL_LIST: return "list";
        case TOAD_VAL_SET: return "set";
        case TOAD_VAL_FUNCTION: return "function";
        case TOAD_VAL_CLASS: return "class";
        case TOAD_VAL_INSTANCE: return "instance";
        case TOAD_VAL_BOUND_METHOD: return "bound_method";
        case TOAD_VAL_UUID: return "uuid";
        case TOAD_VAL_DATETIME: return "datetime";
        case TOAD_VAL_DATE: return "date";
        case TOAD_VAL_TIME: return "time";
        case TOAD_VAL_DURATION: return "duration";
        case TOAD_VAL_DECIMAL: return "decimal";
        case TOAD_VAL_BIGINT: return "bigint";
        case TOAD_VAL_MONEY: return "money";
        case TOAD_VAL_ENUM: return "enum";
        case TOAD_VAL_FLAGS: return "flags";
        case TOAD_VAL_OFFSET_DATETIME: return "offset_datetime";
        case TOAD_VAL_TYPED_ARRAY: return "typed_array";
        default: return "unknown";
    }
}

// Convert value to double (handles INTEGER and NUMBER)
double val_to_double(toad_value_t val) {
    if (val.type == TOAD_VAL_INTEGER) return (double)val.data.integer;
    if (val.type == TOAD_VAL_NUMBER) return val.data.number;
    if (val.type == TOAD_VAL_BOOL) return val.data.boolean ? 1.0 : 0.0;
    return 0.0;
}

int values_match(toad_value_t lhs, toad_value_t rhs) {
    int lhs_numeric = (lhs.type == TOAD_VAL_NUMBER || lhs.type == TOAD_VAL_INTEGER);
    int rhs_numeric = (rhs.type == TOAD_VAL_NUMBER || rhs.type == TOAD_VAL_INTEGER);

    if (lhs_numeric && rhs_numeric)
        return fabs(val_to_double(lhs) - val_to_double(rhs)) < 1e-9;
    if (lhs.type == TOAD_VAL_BOOL && rhs.type == TOAD_VAL_BOOL)
        return lhs.data.boolean == rhs.data.boolean;
    if (lhs.type == TOAD_VAL_STRING && rhs.type == TOAD_VAL_STRING)
        return vstr_eq(lhs.data.string, rhs.data.string);
    if (lhs.type == TOAD_VAL_BYTES && rhs.type == TOAD_VAL_BYTES)
        return lhs.data.bytes.len == rhs.data.bytes.len &&
               (lhs.data.bytes.len == 0 ||
                memcmp(lhs.data.bytes.data, rhs.data.bytes.data, lhs.data.bytes.len) == 0);
    if (lhs.type == TOAD_VAL_UUID && rhs.type == TOAD_VAL_UUID)
        return memcmp(lhs.data.uuid, rhs.data.uuid, sizeof(lhs.data.uuid)) == 0;
    if (lhs.type == TOAD_VAL_DATETIME && rhs.type == TOAD_VAL_DATETIME)
        return memcmp(&lhs.data.datetime, &rhs.data.datetime, sizeof(lhs.data.datetime)) == 0;
    if (lhs.type == TOAD_VAL_OFFSET_DATETIME && rhs.type == TOAD_VAL_OFFSET_DATETIME)
        return lhs.data.offset_datetime.offset_minutes == rhs.data.offset_datetime.offset_minutes &&
               memcmp(&lhs.data.offset_datetime.datetime, &rhs.data.offset_datetime.datetime,
                      sizeof(lhs.data.offset_datetime.datetime)) == 0;
    if (lhs.type == TOAD_VAL_DATE && rhs.type == TOAD_VAL_DATE)
        return lhs.data.date.year == rhs.data.date.year &&
               lhs.data.date.month == rhs.data.date.month &&
               lhs.data.date.day == rhs.data.date.day;
    if (lhs.type == TOAD_VAL_TIME && rhs.type == TOAD_VAL_TIME)
        return lhs.data.time.hour == rhs.data.time.hour &&
               lhs.data.time.minute == rhs.data.time.minute &&
               lhs.data.time.second == rhs.data.time.second &&
               lhs.data.time.millisecond == rhs.data.time.millisecond;
    if (lhs.type == TOAD_VAL_DURATION && rhs.type == TOAD_VAL_DURATION)
        return lhs.data.duration_ms == rhs.data.duration_ms;
    if (lhs.type == TOAD_VAL_DECIMAL && rhs.type == TOAD_VAL_DECIMAL) {
        toad_decimal_t l = lhs.data.decimal;
        toad_decimal_t r = rhs.data.decimal;
        while (l.scale > 0 && l.mantissa % 10 == 0) {
            l.mantissa /= 10;
            l.scale--;
        }
        while (r.scale > 0 && r.mantissa % 10 == 0) {
            r.mantissa /= 10;
            r.scale--;
        }
        if (l.mantissa == 0) l.scale = 0;
        if (r.mantissa == 0) r.scale = 0;
        return l.mantissa == r.mantissa && l.scale == r.scale;
    }
    if (lhs.type == TOAD_VAL_BIGINT && rhs.type == TOAD_VAL_BIGINT)
        return vstr_eq(lhs.data.bigint.text, rhs.data.bigint.text);
    if (lhs.type == TOAD_VAL_MONEY && rhs.type == TOAD_VAL_MONEY)
        return lhs.data.money.amount.mantissa == rhs.data.money.amount.mantissa &&
               lhs.data.money.amount.scale == rhs.data.money.amount.scale &&
               memcmp(lhs.data.money.currency, rhs.data.money.currency, 4) == 0;
    if ((lhs.type == TOAD_VAL_ENUM || lhs.type == TOAD_VAL_FLAGS) && lhs.type == rhs.type)
        return lhs.data.enum_val.value == rhs.data.enum_val.value &&
               vstr_eq(lhs.data.enum_val.type_name, rhs.data.enum_val.type_name);
    if (lhs.type == TOAD_VAL_NULL && rhs.type == TOAD_VAL_NULL)
        return 1;

    return lhs.type == rhs.type;
}

static int is_private_member_name(const char *member) {
    return member && member[0] == '_';
}

toad_class_t *eval_current_class(toad_env_t *env);
static int can_access_private_member(toad_env_t *env,
                                     const toad_node_t *object_node);

static int toad_class_is_subclass_of(toad_class_t *klass, toad_class_t *base) {
    for (toad_class_t *curr = klass; curr; curr = curr->prototype) {
        if (curr == base) return 1;
    }
    return 0;
}

static int can_access_declared_member(toad_env_t *env, toad_class_t *owner_class,
                                      int access_level) {
    if (access_level == TOAD_ACCESS_PUBLIC) return 1;
    if (!owner_class) return 0;

    toad_class_t *current_class = eval_current_class(env);
    if (!current_class) return 0;
    if (current_class == owner_class) return 1;
    if (access_level == TOAD_ACCESS_PROTECTED) {
        return toad_class_is_subclass_of(current_class, owner_class);
    }
    return 0;
}

int can_access_method(toad_env_t *env, toad_func_t *method,
                             const toad_node_t *object_node) {
    if (!method) return 0;
    if (is_private_member_name(method->name) &&
        !can_access_private_member(env, object_node)) {
        return 0;
    }
    return can_access_declared_member(env, method->owner_class, method->access_level);
}

toad_value_t throw_method_access_error(toad_env_t *env, const toad_node_t *node,
                                                const char *method_name,
                                                toad_func_t *method) {
    if ((method_name && is_private_member_name(method_name)) ||
        (method && method->access_level == TOAD_ACCESS_PRIVATE)) {
        return throw_error(env, node, "Private method '%s' is not accessible", method_name);
    }
    if (method && method->access_level == TOAD_ACCESS_PROTECTED) {
        return throw_error(env, node, "Protected method '%s' is not accessible", method_name);
    }
    return throw_error(env, node, "Method '%s' is not accessible", method_name);
}

toad_value_t throw_field_access_error(toad_env_t *env, const toad_node_t *node,
                                               const char *field_name, int access_level) {
    if ((field_name && is_private_member_name(field_name)) ||
        access_level == TOAD_ACCESS_PRIVATE) {
        return throw_error(env, node, "Private field '%s' is not accessible", field_name);
    }
    if (access_level == TOAD_ACCESS_PROTECTED) {
        return throw_error(env, node, "Protected field '%s' is not accessible", field_name);
    }
    return throw_error(env, node, "Field '%s' is not accessible", field_name);
}

int can_access_declared_field(toad_env_t *env, toad_class_t *owner_class,
                                     int access_level,
                                     const toad_node_t *object_node,
                                     const char *field_name) {
    if (is_private_member_name(field_name) &&
        !can_access_private_member(env, object_node)) {
        return 0;
    }
    return can_access_declared_member(env, owner_class, access_level);
}

static int can_access_private_member(toad_env_t *env,
                                     const toad_node_t *object_node) {
    if (!object_node) return 0;
    if (object_node->type == TOAD_NODE_THIS) return 1;
    if (object_node->type != TOAD_NODE_VARIABLE) return 0;

    toad_class_t *current_class = eval_current_class(env);
    const char *name = object_node->data.variable.name;
    if (!current_class || !name) return 0;

    if (current_class->name && strcmp(current_class->name, name) == 0) return 1;

    toad_value_t value = toad_env_get(env, name);
    return value.type == TOAD_VAL_CLASS &&
           value.data.class_val.klass == current_class;
}

static toad_node_t *eval_param_binding_node(toad_node_t *param) {
    if (param && param->type == TOAD_NODE_MEMBER_ACCESS &&
        param->data.member_access.object &&
        param->data.member_access.object->type == TOAD_NODE_VARIABLE) {
        return param->data.member_access.object;
    }
    return param;
}

toad_value_t eval_script_function(toad_func_t *func, size_t argc,
                                           toad_value_t *args, toad_env_t *parent_env,
                                           toad_env_t *caller_env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    size_t frame_bytes;
    int frame_entered = 0;
    if (!func || !func->is_script || !parent_env || !caller_env) return zero;

    if (argc > (SIZE_MAX - sizeof(toad_env_t)) / sizeof(toad_value_t)) {
        caller_env->aborted = 1;
        return zero;
    }
    frame_bytes = sizeof(toad_env_t) + argc * sizeof(toad_value_t);
    if (caller_env->safe_point) {
        if (func->data.script.body && func->data.script.body->line > 0) {
            caller_env->last_line = func->data.script.body->line;
            caller_env->last_column = func->data.script.body->column;
        }
        if (caller_env->safe_point(caller_env->safe_point_user_data,
                                   TOAD_SAFE_POINT_FUNCTION_ENTER, frame_bytes) != 0) {
            caller_env->aborted = 1;
            return zero;
        }
        frame_entered = 1;
    }

    caller_env->curr_recursion++;
    if (caller_env->curr_recursion > caller_env->max_recursion) {
        caller_env->aborted = 1;
        caller_env->curr_recursion--;
        if (frame_entered)
            (void)caller_env->safe_point(caller_env->safe_point_user_data,
                                         TOAD_SAFE_POINT_FUNCTION_LEAVE, frame_bytes);
        return zero;
    }

    toad_env_t *local_env = (toad_env_t *)malloc(sizeof(toad_env_t));
    if (!local_env) {
        caller_env->aborted = 1;
        caller_env->curr_recursion--;
        if (frame_entered)
            (void)caller_env->safe_point(caller_env->safe_point_user_data,
                                         TOAD_SAFE_POINT_FUNCTION_LEAVE, frame_bytes);
        return zero;
    }

    toad_env_init_child(local_env, parent_env);
    local_env->eval_node = caller_env->eval_node ? caller_env->eval_node : parent_env->eval_node;
    local_env->exec_script_body =
        caller_env->exec_script_body ? caller_env->exec_script_body : parent_env->exec_script_body;
    local_env->safe_point = caller_env->safe_point;
    local_env->safe_point_user_data = caller_env->safe_point_user_data;
    toad_env_import_vars(local_env, func->closure_env);
    local_env->current_class = func->owner_class ? func->owner_class : parent_env->current_class;
    local_env->current_method_is_static =
        func->owner_class ? func->is_static_method : parent_env->current_method_is_static;
    local_env->current_method_is_constructor =
        func->owner_class && !func->is_static_method && func->name &&
        strcmp(func->name, "constructor") == 0;
    local_env->max_recursion = caller_env->max_recursion;
    local_env->curr_recursion = caller_env->curr_recursion;
    local_env->max_loop_iterations = caller_env->max_loop_iterations;
    local_env->curr_loop_iterations = caller_env->curr_loop_iterations;
    local_env->max_nodes = caller_env->max_nodes;
    local_env->curr_nodes = caller_env->curr_nodes;

    toad_node_t **params = func->data.script.arg_params;
    size_t param_count = func->data.script.arg_count;
    int is_variadic = 0;
    if (param_count > 0 && params && params[param_count - 1] &&
        params[param_count - 1]->type == TOAD_NODE_SPREAD) {
        is_variadic = 1;
    }

    size_t std_args = is_variadic ? (param_count - 1) : param_count;
    for (size_t i = 0; i < std_args; ++i) {
        toad_node_t *param = params ? params[i] : NULL;
        toad_node_t *binding_param = eval_param_binding_node(param);
        toad_value_t val = zero;
        if (i < argc) {
            val = args[i];
        } else if (param && param->type == TOAD_NODE_ASSIGNMENT && param->data.assignment.value) {
            val = toad_env_eval_node(param->data.assignment.value, parent_env);
        }

        if (!binding_param) continue;
        if (binding_param->type == TOAD_NODE_ASSIGNMENT) {
            toad_env_set(local_env, binding_param->data.assignment.name, val);
        } else {
            eval_destructure(binding_param, val, local_env, 0);
        }
    }

    if (is_variadic) {
        size_t rest_sz = (argc > std_args) ? (argc - std_args) : 0;
        toad_value_t *rest_items = NULL;
        if (rest_sz > 0) {
            rest_items = (toad_value_t*)mem_alloc(&local_env->arena, rest_sz * sizeof(toad_value_t));
        }
        if (rest_sz == 0 || rest_items) {
            for (size_t i = 0; i < rest_sz; ++i) {
                rest_items[i] = toad_value_clone_to_env(args[std_args + i], local_env);
            }
            eval_destructure(params[param_count - 1], toad_val_list_ex(rest_items, rest_sz, 0),
                             local_env, 0);
        }
    }

    toad_value_t result = zero;
    if (!local_env->exec_script_body ||
        !local_env->exec_script_body(func, local_env, caller_env, &result)) {
        result = toad_env_eval_node(func->data.script.body, local_env);
    }
    result = toad_value_clone_to_env(result, caller_env);

    caller_env->curr_nodes = local_env->curr_nodes;
    caller_env->curr_loop_iterations = local_env->curr_loop_iterations;
    caller_env->aborted = local_env->aborted;
    caller_env->curr_recursion--;

    if (local_env->flow == toad_FLOW_RETURN) {
        result = toad_value_clone_to_env(local_env->return_value, caller_env);
    } else if (local_env->flow == toad_FLOW_THROW) {
        caller_env->flow = toad_FLOW_THROW;
        caller_env->error_value = toad_value_clone_to_env(local_env->error_value, caller_env);
        snprintf(caller_env->error_msg, sizeof(caller_env->error_msg), "%s",
                 local_env->error_msg);
        caller_env->error_line = local_env->error_line;
        caller_env->error_column = local_env->error_column;
    } else if (local_env->flow == toad_FLOW_YIELD) {
        caller_env->flow = toad_FLOW_YIELD;
    }

    toad_env_free(local_env);
    free(local_env);
    if (frame_entered &&
        caller_env->safe_point(caller_env->safe_point_user_data,
                               TOAD_SAFE_POINT_FUNCTION_LEAVE, frame_bytes) != 0)
        caller_env->aborted = 1;
    return result;
}

static int grow_value_array(toad_value_t **vals, size_t *cap, size_t needed) {
    if (!vals || !cap) return 0;
    if (needed <= *cap) return 1;

    size_t new_cap = (*cap > 0) ? *cap : 4;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    toad_value_t *new_vals = (toad_value_t*)realloc(*vals, new_cap * sizeof(toad_value_t));
    if (!new_vals) return 0;

    *vals = new_vals;
    *cap = new_cap;
    return 1;
}

toad_value_t* eval_expand_args(toad_node_t **nodes, size_t count, toad_env_t *env, size_t *out_count) {
    size_t cap = count > 0 ? count : 4;
    toad_value_t *vals = (toad_value_t*)malloc(cap * sizeof(toad_value_t));
    if (!vals) {
        *out_count = 0;
        return NULL;
    }
    size_t actual = 0;

    for (size_t i = 0; i < count; ++i) {
        if (nodes[i]->type == TOAD_NODE_SPREAD) {
            toad_value_t el = toad_env_eval_node(nodes[i]->data.spread.child, env);
            if (el.type == TOAD_VAL_VECTOR) {
                if (!grow_value_array(&vals, &cap, actual + el.data.vector.size)) {
                    toad_value_destroy(&el);
                    toad_values_destroy(vals, actual);
                    free(vals);
                    *out_count = 0;
                    return NULL;
                }
                for (size_t j = 0; j < el.data.vector.size; ++j) {
                    vals[actual++] = toad_val_num(el.data.vector.data[j]);
                }
                toad_value_destroy(&el);
            } else if (el.type == TOAD_VAL_LIST) {
                if (!grow_value_array(&vals, &cap, actual + el.data.list.count)) {
                    toad_value_destroy(&el);
                    toad_values_destroy(vals, actual);
                    free(vals);
                    *out_count = 0;
                    return NULL;
                }
                for (size_t j = 0; j < el.data.list.count; ++j) {
                    if (toad_value_copy_to_env(
                            toad_value_borrow(el.data.list.items[j]), env,
                            &vals[actual]) != 0) {
                        toad_value_destroy(&el);
                        toad_values_destroy(vals, actual);
                        free(vals);
                        *out_count = 0;
                        return NULL;
                    }
                    actual++;
                }
                toad_value_destroy(&el);
            } else {
                if (!grow_value_array(&vals, &cap, actual + 1)) {
                    toad_value_destroy(&el);
                    toad_values_destroy(vals, actual);
                    free(vals);
                    *out_count = 0;
                    return NULL;
                }
                vals[actual++] = el;
            }
        } else {
            if (!grow_value_array(&vals, &cap, actual + 1)) {
                toad_values_destroy(vals, actual);
                free(vals);
                *out_count = 0;
                return NULL;
            }
            vals[actual++] = toad_env_eval_node(nodes[i], env);
        }
        if (env && (env->flow != toad_FLOW_NORMAL || env->aborted)) break;
    }
    *out_count = actual;
    return vals;
}

toad_class_t *eval_current_class(toad_env_t *env) {
    while (env) {
        if (env->current_class) return env->current_class;
        env = env->parent;
    }
    return NULL;
}

static toad_func_t *eval_find_constructor(toad_class_t *klass) {
    while (klass) {
        if (klass->constructor) return klass->constructor;
        klass = klass->prototype;
    }
    return NULL;
}

toad_func_t *eval_find_constructor_typed(toad_class_t *klass, size_t argc,
                                                  toad_value_t *args) {
    toad_func_t *constructor = toad_class_lookup_constructor_typed(klass, argc, args);
    return constructor ? constructor : eval_find_constructor(klass);
}

static toad_func_t *eval_make_class_func(toad_node_t *method_node,
                                           toad_class_t *klass,
                                           toad_env_t *class_closure,
                                           toad_env_t *env) {
    if (!method_node || !klass || !env) return NULL;

    toad_func_t *method_func =
        (toad_func_t *)mem_alloc(&env->arena, sizeof(toad_func_t));
    if (!method_func) return NULL;

    method_func->name = method_node->data.method.name;
    method_func->is_script = 1;
    method_func->owner_class = klass;
    method_func->closure_env = class_closure;
    toad_env_retain(class_closure);
    method_func->is_static_method = method_node->data.method.is_static;
    method_func->access_level = method_node->data.method.access_level;
    method_func->is_override = method_node->data.method.is_override;
    method_func->is_final = method_node->data.method.is_final;
    method_func->data.script.arg_params = method_node->data.method.arg_params;
    method_func->data.script.arg_count = method_node->data.method.arg_count;
    method_func->data.script.body = method_node->data.method.body;
    method_func->next = NULL;
    return method_func;
}

static toad_func_t *eval_lookup_parent_method_signature(toad_class_t *parent,
                                                          toad_node_t *method_node,
                                                          toad_class_t *owner_class,
                                                          toad_env_t *class_closure) {
    if (!parent || !method_node) return NULL;

    toad_func_t signature;
    memset(&signature, 0, sizeof(signature));
    signature.name = method_node->data.method.name;
    signature.is_script = 1;
    signature.owner_class = owner_class;
    signature.closure_env = class_closure;
    signature.is_static_method = method_node->data.method.is_static;
    signature.data.script.arg_params = method_node->data.method.arg_params;
    signature.data.script.arg_count = method_node->data.method.arg_count;

    return toad_class_lookup_method_signature(parent, &signature);
}

toad_value_t eval_class_value_instantiation(toad_class_t *klass,
                                                     const char *class_name,
                                                     toad_node_t **arg_nodes,
                                                     size_t arg_count,
                                                     toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!klass || !env) return zero;

    if (toad_class_is_interface(klass)) {
        return throw_error(env, NULL, "Cannot instantiate interface '%s'", class_name);
    }
    if (toad_class_is_abstract(klass)) {
        return throw_error(env, NULL, "Cannot instantiate abstract class '%s'", class_name);
    }

    size_t actual_count = 0;
    toad_value_t *args = eval_expand_args(arg_nodes, arg_count, env, &actual_count);
    if (env->flow != toad_FLOW_NORMAL || env->aborted) {
        toad_values_destroy(args, actual_count);
        free(args);
        return zero;
    }

    toad_instance_t *instance = toad_instance_create(klass, &env->arena);
    if (!instance) {
        toad_values_destroy(args, actual_count);
        free(args);
        return zero;
    }

    toad_func_t *constructor = eval_find_constructor_typed(klass, actual_count, args);
    if (constructor) {
        toad_env_t ctor_env;
        toad_env_init_child(&ctor_env, env);
        ctor_env.eval_node = env->eval_node;
        ctor_env.exec_script_body = env->exec_script_body;
        ctor_env.current_class = klass;
        ctor_env.current_method_is_static = 0;
        ctor_env.current_method_is_constructor = 1;
        ctor_env.max_recursion = env->max_recursion;
        ctor_env.curr_recursion = env->curr_recursion;
        ctor_env.max_loop_iterations = env->max_loop_iterations;
        ctor_env.curr_loop_iterations = env->curr_loop_iterations;
        ctor_env.max_nodes = env->max_nodes;
        ctor_env.curr_nodes = env->curr_nodes;
        toad_env_set_local_borrowed(&ctor_env, "this", toad_val_instance(instance));

        eval_script_function(constructor, actual_count, args, &ctor_env, env);

        toad_env_free(&ctor_env);

        if (env->flow != toad_FLOW_NORMAL || env->aborted) {
            toad_values_destroy(args, actual_count);
            free(args);
            return zero;
        }
    }

    toad_values_destroy(args, actual_count);
    free(args);
    return toad_val_instance(instance);
}

toad_value_t eval_class_instantiation(const char *class_name, toad_node_t **arg_nodes,
                                               size_t arg_count, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!class_name || !env) return zero;

    toad_class_t *klass = NULL;
    toad_value_t class_value = toad_env_get(env, class_name);
    if (class_value.type == TOAD_VAL_CLASS) {
        klass = class_value.data.class_val.klass;
    }
    if (!klass) {
        return throw_error(env, NULL, "Undefined class '%s'", class_name);
    }

    return eval_class_value_instantiation(klass, class_name, arg_nodes, arg_count, env);
}

toad_value_t eval_instance_script_method(toad_instance_t *instance,
                                                  toad_func_t *method,
                                                  size_t argc,
                                                  toad_value_t *args,
                                                  toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!instance || !method || !env) return zero;

    toad_env_t method_env;
    toad_env_init_child(&method_env, env);
    method_env.eval_node = env->eval_node;
    method_env.exec_script_body = env->exec_script_body;
    method_env.current_class = method->owner_class;
    method_env.current_method_is_static = 0;
    method_env.current_method_is_constructor = 0;
    method_env.max_recursion = env->max_recursion;
    method_env.curr_recursion = env->curr_recursion;
    method_env.max_loop_iterations = env->max_loop_iterations;
    method_env.curr_loop_iterations = env->curr_loop_iterations;
    method_env.max_nodes = env->max_nodes;
    method_env.curr_nodes = env->curr_nodes;
    toad_env_set_local_borrowed(&method_env, "this", toad_val_instance(instance));

    toad_value_t result = eval_script_function(method, argc, args, &method_env, env);
    toad_env_free(&method_env);
    return result;
}

toad_value_t eval_class_def_node(const toad_node_t *node, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!node || !env || node->type != TOAD_NODE_CLASS_DEF) return zero;

    const char *class_name = node->data.class_def.name;
    if (!class_name) return zero;

    toad_class_t *parent = NULL;
    if (node->data.class_def.parent_name) {
        toad_value_t parent_value = toad_env_get(env, node->data.class_def.parent_name);
        if (parent_value.type == TOAD_VAL_CLASS) {
            parent = parent_value.data.class_val.klass;
        }
        if (!parent) {
            return throw_error(env, node, "Undefined parent class '%s'",
                               node->data.class_def.parent_name);
        }
    }

    toad_class_t *klass = toad_class_create(&env->arena, class_name,
                                                node->data.class_def.constructor,
                                                node->data.class_def.methods,
                                                node->data.class_def.method_count);
    if (!klass) return zero;
    toad_class_set_abstract(klass, node->data.class_def.is_abstract);
    toad_class_set_interface(klass, node->data.class_def.is_interface);
    toad_env_t *class_closure = toad_env_snapshot(env);
    if (class_closure) class_closure->is_class_closure = 1;

    if (parent && toad_class_is_final(parent)) {
        return throw_error(env, node, "Cannot extend final class '%s'",
                           parent->name ? parent->name : "<class>");
    }

    if (parent) {
        toad_class_set_prototype(klass, parent);
    }

    if (node->data.class_def.constructor) {
        toad_node_t *ctor = node->data.class_def.constructor;
        toad_func_t *ctor_func = (toad_func_t *)mem_alloc(&env->arena, sizeof(toad_func_t));
        if (ctor_func) {
            ctor_func->name = "constructor";
            ctor_func->is_script = 1;
            ctor_func->owner_class = klass;
            ctor_func->closure_env = class_closure;
            toad_env_retain(class_closure);
            ctor_func->is_static_method = 0;
            ctor_func->access_level = TOAD_ACCESS_PUBLIC;
            ctor_func->is_override = 0;
            ctor_func->is_final = 0;
            ctor_func->data.script.arg_params = ctor->data.func_def.arg_params;
            ctor_func->data.script.arg_count = ctor->data.func_def.arg_count;
            ctor_func->data.script.body = ctor->data.func_def.body;
            ctor_func->next = NULL;
            klass->constructor = ctor_func;
            toad_class_add_constructor(klass, ctor_func);
        }
    }

    for (size_t i = 0; i < node->data.class_def.method_count; ++i) {
        toad_node_t *method_node = node->data.class_def.methods[i];
        if (method_node->type == TOAD_NODE_FIELD_DECL) {
            toad_value_t field_value;
            memset(&field_value, 0, sizeof(field_value));
            if (method_node->data.field_decl.initializer) {
                field_value = toad_env_eval_node(method_node->data.field_decl.initializer, env);
                if (env->flow != toad_FLOW_NORMAL || env->aborted) return zero;
            } else {
                field_value.type = TOAD_VAL_NULL;
            }

            if (method_node->data.field_decl.is_static) {
                if (!toad_class_declare_static_field_typed(
                        klass, method_node->data.field_decl.name,
                        method_node->data.field_decl.declared_type, field_value,
                        method_node->data.field_decl.access_level)) {
                    toad_value_destroy(&field_value);
                    return throw_error(env, method_node,
                                       "Invalid default value for static field '%s'",
                                       method_node->data.field_decl.name);
                }
            } else {
                toad_class_declare_instance_field_typed(
                    klass, method_node->data.field_decl.name,
                    method_node->data.field_decl.declared_type, field_value,
                    method_node->data.field_decl.initializer != NULL,
                    method_node->data.field_decl.access_level);
            }
            toad_value_destroy(&field_value);
            continue;
        }
        if (method_node->type != TOAD_NODE_METHOD) continue;

        if (method_node->data.method.name &&
            strcmp(method_node->data.method.name, "constructor") == 0) {
            if (method_node->data.method.is_abstract) {
                return throw_error(env, method_node, "Constructor cannot be abstract");
            }
            toad_func_t *ctor_func =
                eval_make_class_func(method_node, klass, class_closure, env);
            if (ctor_func) {
                ctor_func->access_level = TOAD_ACCESS_PUBLIC;
                ctor_func->is_static_method = 0;
                toad_class_add_constructor(klass, ctor_func);
            }
            continue;
        }

        toad_func_t *parent_method =
            eval_lookup_parent_method_signature(parent, method_node, klass, class_closure);
        if (method_node->data.method.is_override && !parent_method) {
            return throw_error(env, method_node,
                               "Method '%s' is marked override but no parent method matches",
                               method_node->data.method.name);
        }
        if (parent_method && parent_method->is_final) {
            return throw_error(env, method_node, "Cannot override final method '%s'",
                               method_node->data.method.name);
        }

        if (method_node->data.method.is_abstract) {
            toad_class_add_abstract_method_signature(
                klass, method_node->data.method.name,
                method_node->data.method.arg_params,
                method_node->data.method.arg_count,
                class_closure,
                method_node->data.method.is_static);
            continue;
        }

        toad_func_t *method_func =
            eval_make_class_func(method_node, klass, class_closure, env);
        if (!method_func) continue;

        toad_class_add_method(klass, method_node->data.method.name, method_func,
                                method_node->data.method.is_static);
    }

    for (size_t i = 0; i < node->data.class_def.interface_count; ++i) {
        const char *interface_name = node->data.class_def.interface_names[i];
        toad_value_t interface_value = interface_name
            ? toad_env_get(env, interface_name)
            : zero;
        if (interface_value.type != TOAD_VAL_CLASS) {
            return throw_error(env, node, "Undefined interface '%s'",
                               interface_name ? interface_name : "<unknown>");
        }

        toad_class_t *interface_class = interface_value.data.class_val.klass;
        if (!toad_class_is_interface(interface_class)) {
            return throw_error(env, node, "Class '%s' is not an interface",
                               interface_name ? interface_name : "<unknown>");
        }
        toad_class_add_interface(klass, interface_class);
        toad_class_add_abstract_methods_from(klass, interface_class);
    }
    toad_class_finalize_abstract_methods(klass);
    toad_class_set_final(klass, node->data.class_def.is_final);
    if (!toad_class_finalize_cmeta(klass) ||
        !toad_class_finalize_cmeta_data(klass)) {
        return throw_error(env, node,
                           "Failed to finalize CMeta reflection for class '%s'",
                           class_name);
    }

    toad_env_register_class(env, class_name, klass);
    return zero;
}

static toad_value_t *numeric_args_to_values(size_t argc, const double *argv) {
    if (argc == 0) return NULL;
    toad_value_t *args = (toad_value_t *)calloc(argc, sizeof(toad_value_t));
    if (!args) return NULL;
    for (size_t i = 0; i < argc; ++i) {
        args[i] = toad_val_num(argv ? argv[i] : 0.0);
    }
    return args;
}

static toad_value_t *numeric_args_to_values_buffer(size_t argc, const double *argv,
                                                     toad_value_t *stack_args,
                                                     size_t stack_cap, int *needs_free) {
    if (needs_free) *needs_free = 0;
    if (argc == 0) return NULL;

    toad_value_t *args = stack_args;
    if (argc > stack_cap) {
        args = (toad_value_t *)malloc(argc * sizeof(toad_value_t));
        if (!args) return NULL;
        if (needs_free) *needs_free = 1;
    }

    for (size_t i = 0; i < argc; ++i) {
        args[i] = toad_val_num(argv ? argv[i] : 0.0);
    }
    return args;
}

TOAD_C_API toad_value_t toad_oop_define_class(const toad_node_t *node, toad_env_t *env) {
    return eval_class_def_node(node, env);
}

TOAD_C_API toad_value_t toad_oop_alias_class(const char *target_name, const char *source_name,
                                                toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!target_name || !source_name || !env) return zero;

    toad_class_t *klass = NULL;
    toad_value_t source_value = toad_env_get(env, source_name);
    if (source_value.type == TOAD_VAL_CLASS) klass = source_value.data.class_val.klass;

    if (!klass) return throw_error(env, NULL, "Undefined class '%s'", source_name);

    toad_value_t class_value = toad_val_class(klass);
    toad_env_set(env, target_name, class_value);
    return class_value;
}

TOAD_C_API toad_value_t toad_oop_instantiate_numeric(const char *class_name, size_t argc,
                                                        const double *argv, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!class_name || !env) return zero;

    toad_class_t *klass = NULL;
    toad_value_t class_value = toad_env_get(env, class_name);
    if (class_value.type == TOAD_VAL_CLASS) klass = class_value.data.class_val.klass;
    if (!klass) return throw_error(env, NULL, "Undefined class '%s'", class_name);
    if (toad_class_is_interface(klass)) {
        return throw_error(env, NULL, "Cannot instantiate interface '%s'", class_name);
    }
    if (toad_class_is_abstract(klass)) {
        return throw_error(env, NULL, "Cannot instantiate abstract class '%s'", class_name);
    }

    toad_instance_t *instance = toad_instance_create(klass, &env->arena);
    if (!instance) return zero;

    toad_value_t *args = numeric_args_to_values(argc, argv);
    if (argc > 0 && !args) return zero;

    toad_func_t *constructor = eval_find_constructor_typed(klass, argc, args);
    if (constructor) {
        toad_env_t ctor_env;
        toad_env_init_child(&ctor_env, env);
        ctor_env.eval_node = env->eval_node;
        ctor_env.exec_script_body = env->exec_script_body;
        ctor_env.current_class = klass;
        ctor_env.current_method_is_static = 0;
        ctor_env.current_method_is_constructor = 1;
        ctor_env.max_recursion = env->max_recursion;
        ctor_env.curr_recursion = env->curr_recursion;
        ctor_env.max_loop_iterations = env->max_loop_iterations;
        ctor_env.curr_loop_iterations = env->curr_loop_iterations;
        ctor_env.max_nodes = env->max_nodes;
        ctor_env.curr_nodes = env->curr_nodes;
        toad_env_set_local_borrowed(&ctor_env, "this", toad_val_instance(instance));

        eval_script_function(constructor, argc, args, &ctor_env, env);
        toad_env_free(&ctor_env);
    }

    free(args);
    if (env->flow != toad_FLOW_NORMAL || env->aborted) return zero;
    return toad_val_instance(instance);
}

TOAD_C_API toad_value_t toad_oop_instantiate_class_value(toad_value_t class_value,
                                                            const char *class_name, size_t argc,
                                                            toad_value_t *args,
                                                            toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (class_value.type != TOAD_VAL_CLASS || !class_value.data.class_val.klass || !env) {
        return zero;
    }

    toad_class_t *klass = class_value.data.class_val.klass;
    if (toad_class_is_interface(klass)) {
        return throw_error(env, NULL, "Cannot instantiate interface '%s'",
                           class_name ? class_name : "<class>");
    }
    if (toad_class_is_abstract(klass)) {
        return throw_error(env, NULL, "Cannot instantiate abstract class '%s'",
                           class_name ? class_name : "<class>");
    }

    toad_instance_t *instance = toad_instance_create(klass, &env->arena);
    if (!instance) return zero;

    toad_func_t *constructor = eval_find_constructor_typed(klass, argc, args);
    if (constructor) {
        toad_env_t ctor_env;
        toad_env_init_child(&ctor_env, env);
        ctor_env.eval_node = env->eval_node;
        ctor_env.exec_script_body = env->exec_script_body;
        ctor_env.current_class = klass;
        ctor_env.current_method_is_static = 0;
        ctor_env.current_method_is_constructor = 1;
        ctor_env.max_recursion = env->max_recursion;
        ctor_env.curr_recursion = env->curr_recursion;
        ctor_env.max_loop_iterations = env->max_loop_iterations;
        ctor_env.curr_loop_iterations = env->curr_loop_iterations;
        ctor_env.max_nodes = env->max_nodes;
        ctor_env.curr_nodes = env->curr_nodes;
        toad_env_set_local_borrowed(&ctor_env, "this", toad_val_instance(instance));

        eval_script_function(constructor, argc, args, &ctor_env, env);
        toad_env_free(&ctor_env);
    }

    if (env->flow != toad_FLOW_NORMAL || env->aborted) return zero;
    return toad_val_instance(instance);
}

TOAD_C_API toad_value_t toad_oop_call_method_checked_numeric(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, const double *argv, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    toad_value_t *args = numeric_args_to_values(argc, argv);
    if (argc > 0 && !args) return zero;

    toad_value_t result = toad_oop_call_method_checked_values(
        object_name, method_name, object_node, argc, args, env);
    free(args);
    return result;
}

TOAD_C_API toad_value_t toad_oop_call_method_checked_values(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, toad_value_t *args, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !method_name || !env || (argc > 0 && !args)) return zero;

    toad_value_t object = toad_env_get(env, object_name);
    toad_value_t result = zero;
    if (object.type == TOAD_VAL_INSTANCE) {
        toad_instance_t *instance = object.data.instance_val.instance;
        toad_func_t *method =
            toad_class_lookup_method_typed(instance->klass, method_name, 0, argc, args);
        if (!method) {
            result = throw_error(env, NULL, "Instance has no method '%s'", method_name);
        } else if (!can_access_method(env, method, object_node)) {
            result = throw_method_access_error(env, object_node, method_name, method);
        } else {
            result = eval_instance_script_method(instance, method, argc, args, env);
        }
    } else if (object.type == TOAD_VAL_CLASS) {
        toad_class_t *klass = object.data.class_val.klass;
        toad_func_t *method =
            toad_class_lookup_method_typed(klass, method_name, 1, argc, args);
        if (!method) {
            result = throw_error(env, NULL, "Class has no static method '%s'", method_name);
        } else if (!can_access_method(env, method, object_node)) {
            result = throw_method_access_error(env, object_node, method_name, method);
        } else {
            result = eval_script_function(method, argc, args, env, env);
        }
    } else {
        result = throw_error(env, NULL, "Method call '%s' is invalid for %s", method_name,
                             type_name(object.type));
    }

    return result;
}

TOAD_C_API toad_value_t toad_oop_call_method_checked_value_nodes(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    const toad_node_t *call_node, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!call_node || call_node->type != TOAD_NODE_MEMBER_CALL || !env) return zero;

    size_t actual_count = 0;
    toad_value_t *args = eval_expand_args(call_node->data.member_call.args,
                                            call_node->data.member_call.arg_count,
                                            env, &actual_count);
    if (!args && actual_count == 0) return zero;

    toad_value_t result = toad_oop_call_method_checked_values(
        object_name, method_name, object_node, actual_count, args, env);
    free(args);
    return result;
}

static const cmeta_type_desc *oop_admit_receiver_type(
    toad_class_t *klass);

TOAD_C_API toad_value_t toad_oop_call_method_numeric(const char *object_name,
                                                        const char *method_name, size_t argc,
                                                        const double *argv, toad_env_t *env) {
    return toad_oop_call_method_checked_numeric(object_name, method_name, NULL, argc, argv,
                                                  env);
}

TOAD_C_API toad_value_t toad_oop_call_method_mono_checked_numeric(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const toad_node_t *object_node, size_t argc, const double *argv, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !expected_class_name || !method_name || !env) return zero;

    toad_value_t object = toad_env_get(env, object_name);
    toad_class_t *expected_class = NULL;
    toad_value_t class_value = toad_env_get(env, expected_class_name);
    if (class_value.type == TOAD_VAL_CLASS) expected_class = class_value.data.class_val.klass;

    if (object.type == TOAD_VAL_INSTANCE && expected_class &&
        object.data.instance_val.instance->klass == expected_class &&
        oop_admit_receiver_type(expected_class) != NULL) {
        toad_value_t *args = numeric_args_to_values(argc, argv);
        if (argc > 0 && !args) return zero;

        toad_func_t *method =
            toad_class_lookup_method_typed(expected_class, method_name, 0, argc, args);
        if (!method) {
            free(args);
            return throw_error(env, NULL, "Instance has no method '%s'", method_name);
        }
        if (!can_access_method(env, method, object_node)) {
            free(args);
            return throw_method_access_error(env, object_node, method_name, method);
        }

        toad_value_t result =
            eval_instance_script_method(object.data.instance_val.instance, method, argc, args, env);
        free(args);
        return result;
    }

    return toad_oop_call_method_checked_numeric(object_name, method_name, object_node, argc,
                                                  argv, env);
}

TOAD_C_API toad_value_t toad_oop_call_method_mono_checked_value_nodes(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const toad_node_t *object_node, const toad_node_t *call_node, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !expected_class_name || !method_name || !call_node || !env) return zero;
    if (call_node->type != TOAD_NODE_MEMBER_CALL) return zero;

    size_t actual_count = 0;
    toad_value_t *args = eval_expand_args(call_node->data.member_call.args,
                                            call_node->data.member_call.arg_count,
                                            env, &actual_count);
    if (!args && actual_count == 0) return zero;

    toad_value_t object = toad_env_get(env, object_name);
    toad_class_t *expected_class = NULL;
    toad_value_t class_value = toad_env_get(env, expected_class_name);
    if (class_value.type == TOAD_VAL_CLASS) expected_class = class_value.data.class_val.klass;

    toad_value_t result = zero;
    if (object.type == TOAD_VAL_INSTANCE && expected_class &&
        object.data.instance_val.instance->klass == expected_class &&
        oop_admit_receiver_type(expected_class) != NULL) {
        toad_func_t *method =
            toad_class_lookup_method_typed(expected_class, method_name, 0,
                                             actual_count, args);
        if (!method) {
            result = throw_error(env, NULL, "Instance has no method '%s'", method_name);
        } else if (!can_access_method(env, method, object_node)) {
            result = throw_method_access_error(env, object_node, method_name, method);
        } else {
            result = eval_instance_script_method(object.data.instance_val.instance, method,
                                                 actual_count, args, env);
        }
    } else {
        result = toad_oop_call_method_checked_values(object_name, method_name, object_node,
                                                       actual_count, args, env);
    }

    free(args);
    return result;
}

TOAD_C_API toad_value_t toad_oop_call_method_mono_numeric(const char *object_name,
                                                              const char *expected_class_name,
                                                              const char *method_name, size_t argc,
                                                              const double *argv,
                                                              toad_env_t *env) {
    return toad_oop_call_method_mono_checked_numeric(object_name, expected_class_name,
                                                       method_name, NULL, argc, argv, env);
}

static const cmeta_type_desc *oop_admit_receiver_type(
    toad_class_t *klass) {
    const cmeta_type_desc *type;

    if (!klass || !toad_class_finalize_cmeta(klass))
        return NULL;
    type = toad_class_cmeta_type(klass);
    return cmeta_type_desc_valid(type) ? type : NULL;
}

static uint64_t oop_method_arg_signature(size_t argc, const toad_value_t *args) {
    uint64_t hash = 1469598103934665603ULL;
    hash ^= (uint64_t)argc;
    hash *= 1099511628211ULL;

    for (size_t i = 0; i < argc; ++i) {
        uint64_t part = (uint64_t)args[i].type;
        if (args[i].type == TOAD_VAL_INSTANCE && args[i].data.instance_val.instance &&
            args[i].data.instance_val.instance->klass) {
            part = (part << 32) ^ args[i].data.instance_val.instance->klass->type_id;
        } else if (args[i].type == TOAD_VAL_CLASS && args[i].data.class_val.klass) {
            part = (part << 32) ^ args[i].data.class_val.klass->type_id;
        }
        hash ^= part;
        hash *= 1099511628211ULL;
    }

    return hash ? hash : 1;
}

TOAD_C_API toad_value_t toad_oop_call_method_cached_checked_numeric(
    const char *object_name, const char *method_name, toad_oop_method_cache_t *cache,
    const toad_node_t *object_node, size_t argc, const double *argv, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !method_name || !env) return zero;

    toad_value_t object = toad_env_get(env, object_name);

    toad_class_t *klass = NULL;
    int is_static = 0;
    if (object.type == TOAD_VAL_INSTANCE) {
        klass = object.data.instance_val.instance->klass;
    } else if (object.type == TOAD_VAL_CLASS) {
        klass = object.data.class_val.klass;
        is_static = 1;
    } else {
        return toad_oop_call_method_checked_numeric(object_name, method_name, object_node,
                                                      argc, argv, env);
    }

    toad_value_t stack_args[8];
    int args_need_free = 0;
    toad_value_t *args =
        numeric_args_to_values_buffer(argc, argv, stack_args, 8, &args_need_free);
    if (argc > 0 && !args) return zero;
    uint64_t arg_signature = oop_method_arg_signature(argc, args);

    toad_func_t *method = NULL;
    if (cache && cache->last_class == klass && cache->is_static == is_static &&
        cache->argc == argc && cache->arg_signature == arg_signature) {
        method = cache->method;
    }
    if (!method) {
        const cmeta_type_desc *receiver_type =
            oop_admit_receiver_type(klass);
        method = toad_class_lookup_method_typed(
            klass, method_name, is_static, argc, args);
        /* Cache only after the receiver class has a valid canonical CMeta
         * identity. Cache hits remain pointer-monomorphic and perform no
         * reflection or registry lookup. */
        if (cache && receiver_type != NULL) {
            cache->last_class = klass;
            cache->method = method;
            cache->arg_signature = arg_signature;
            cache->argc = argc;
            cache->is_static = is_static;
        }
    }
    if (!method) {
        if (args_need_free) free(args);
        if (is_static) return throw_error(env, NULL, "Class has no static method '%s'", method_name);
        return throw_error(env, NULL, "Instance has no method '%s'", method_name);
    }
    if (!can_access_method(env, method, object_node)) {
        if (args_need_free) free(args);
        return throw_method_access_error(env, object_node, method_name, method);
    }

    toad_value_t result = zero;
    if (is_static) {
        result = eval_script_function(method, argc, args, env, env);
    } else {
        result = eval_instance_script_method(object.data.instance_val.instance, method, argc, args, env);
    }
    if (args_need_free) free(args);
    return result;
}

TOAD_C_API toad_value_t toad_oop_call_method_cached_numeric(
    const char *object_name, const char *method_name, toad_oop_method_cache_t *cache,
    size_t argc, const double *argv, toad_env_t *env) {
    return toad_oop_call_method_cached_checked_numeric(object_name, method_name, cache, NULL,
                                                         argc, argv, env);
}

TOAD_C_API toad_value_t toad_oop_call_bound_method(toad_value_t bound_method, size_t argc,
                                                      toad_value_t *args, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (bound_method.type != TOAD_VAL_BOUND_METHOD || !env) return zero;

    return eval_instance_script_method(bound_method.data.bound_method_val.instance,
                                       bound_method.data.bound_method_val.method,
                                       argc, args, env);
}

TOAD_C_API toad_value_t toad_call_function_value(toad_value_t function_value, size_t argc,
                                                    toad_value_t *args, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (function_value.type != TOAD_VAL_FUNCTION ||
        !function_value.data.function.body || !env) {
        return zero;
    }

    toad_func_t func;
    memset(&func, 0, sizeof(func));
    func.name = "<function>";
    func.is_script = 1;
    func.owner_class = function_value.data.function.owner_class;
    func.closure_env = function_value.data.function.closure_env;
    func.is_static_method = function_value.data.function.is_static_method;
    func.access_level = function_value.data.function.access_level;
    func.data.script.arg_params = function_value.data.function.arg_params;
    func.data.script.arg_count = function_value.data.function.arg_count;
    func.data.script.body = function_value.data.function.body;

    toad_env_t *parent_env =
        function_value.data.function.closure_env ? function_value.data.function.closure_env : env;
    return eval_script_function(&func, argc, args, parent_env, env);
}

TOAD_C_API toad_value_t toad_oop_get_member_checked(const char *object_name,
                                                       const char *member_name,
                                                       const toad_node_t *object_node,
                                                       toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !member_name || !env) return zero;

    toad_value_t object = toad_env_get(env, object_name);
    if (object.type == TOAD_VAL_INSTANCE) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        toad_value_t field_value;
        toad_instance_t *instance = object.data.instance_val.instance;
        if (toad_instance_get_field(instance, member_name, &field_value)) {
            toad_class_t *field_owner = NULL;
            int access = toad_class_get_instance_field_access(instance->klass, member_name,
                                                                &field_owner);
            if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            return field_value;
        }

        toad_func_t *method = toad_instance_get_method(instance, member_name);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            return toad_val_bound_method(instance, method);
        }
        return throw_error(env, NULL, "Instance has no field or method '%s'", member_name);
    }
    if (object.type == TOAD_VAL_CLASS) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        toad_value_t static_field;
        toad_class_t *klass = object.data.class_val.klass;
        if (toad_class_get_static_field(klass, member_name, &static_field)) {
            toad_class_t *field_owner = NULL;
            int access = toad_class_get_static_field_access(klass, member_name, &field_owner);
            if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            return static_field;
        }

        toad_func_t *method = toad_class_lookup_method(klass, member_name, 1);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            toad_value_t func_val = { .type = TOAD_VAL_FUNCTION };
            func_val.data.function.arg_params = method->data.script.arg_params;
            func_val.data.function.arg_count = method->data.script.arg_count;
            func_val.data.function.body = method->data.script.body;
            func_val.data.function.closure_env = method->closure_env;
            toad_env_retain(method->closure_env);
            func_val.data.function.owner_class = method->owner_class;
            func_val.data.function.is_static_method = 1;
            func_val.data.function.access_level = method->access_level;
            return func_val;
        }
        return throw_error(env, NULL, "Class has no static field or method '%s'", member_name);
    }
    return zero;
}

TOAD_C_API toad_value_t toad_oop_get_member_cached_checked(
    const char *object_name, const char *member_name, toad_oop_field_cache_t *cache,
    const toad_node_t *object_node, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !member_name || !env) return zero;

    toad_value_t object = toad_env_get(env, object_name);

    if (object.type == TOAD_VAL_INSTANCE) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        toad_instance_t *instance = object.data.instance_val.instance;
        if (cache && !cache->is_static && cache->owner == instance &&
            cache->version == instance->field_version && cache->slot) {
            if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                           object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name,
                                                cache->access_level);
            }
            return *cache->slot;
        }

        toad_class_t *field_owner = NULL;
        int access = toad_class_get_instance_field_access(instance->klass, member_name,
                                                            &field_owner);
        toad_value_t *slot = toad_instance_get_field_slot(instance, member_name);
        if (slot) {
            if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            if (cache) {
                cache->owner = instance;
                cache->field_owner = field_owner;
                cache->version = instance->field_version;
                cache->slot = slot;
                cache->access_level = access;
                cache->is_static = 0;
            }
            return *slot;
        }

        toad_func_t *method = toad_instance_get_method(instance, member_name);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            return toad_val_bound_method(instance, method);
        }
        return throw_error(env, NULL, "Instance has no field or method '%s'", member_name);
    }

    if (object.type == TOAD_VAL_CLASS) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        toad_class_t *klass = object.data.class_val.klass;
        toad_class_t *owner = NULL;
        if (cache && cache->is_static && cache->owner) {
            toad_class_t *cached_owner = (toad_class_t *)cache->owner;
            if (cache->version == cached_owner->static_field_version && cache->slot) {
                if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                               object_node, member_name)) {
                    return throw_field_access_error(env, object_node, member_name,
                                                    cache->access_level);
                }
                return *cache->slot;
            }
        }

        toad_value_t *slot =
            toad_class_get_static_field_slot(klass, member_name, &owner);
        if (slot) {
            int access = toad_class_get_static_field_access(klass, member_name, &owner);
            if (!can_access_declared_field(env, owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            if (cache) {
                cache->owner = owner;
                cache->field_owner = owner;
                cache->version = owner ? owner->static_field_version : 0;
                cache->slot = slot;
                cache->access_level = access;
                cache->is_static = 1;
            }
            return *slot;
        }

        toad_func_t *method = toad_class_lookup_method(klass, member_name, 1);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            toad_value_t func_val = { .type = TOAD_VAL_FUNCTION };
            func_val.data.function.arg_params = method->data.script.arg_params;
            func_val.data.function.arg_count = method->data.script.arg_count;
            func_val.data.function.body = method->data.script.body;
            func_val.data.function.closure_env = method->closure_env;
            toad_env_retain(method->closure_env);
            func_val.data.function.owner_class = method->owner_class;
            func_val.data.function.is_static_method = 1;
            func_val.data.function.access_level = method->access_level;
            return func_val;
        }
        return throw_error(env, NULL, "Class has no static field or method '%s'", member_name);
    }

    return toad_oop_get_member_checked(object_name, member_name, object_node, env);
}

TOAD_C_API toad_value_t toad_oop_get_member(const char *object_name, const char *member_name,
                                               toad_env_t *env) {
    return toad_oop_get_member_checked(object_name, member_name, NULL, env);
}

TOAD_C_API toad_value_t toad_oop_set_member_checked_numeric(
    const char *object_name, const char *member_name, double value,
    const toad_node_t *object_node, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !member_name || !env) return zero;

    toad_value_t object = toad_env_get(env, object_name);

    toad_value_t field_value = toad_val_num(value);
    if (toad_value_is_object_like(&object)) {
        toad_map_set(&object, member_name, field_value);
        toad_env_set(env, object_name, object);
        return field_value;
    }
    if (object.type == TOAD_VAL_INSTANCE) {
        toad_instance_t *instance = object.data.instance_val.instance;
        toad_class_t *field_owner = NULL;
        int access = toad_class_get_instance_field_access(instance->klass, member_name,
                                                            &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }

        char field_error[256];
        if (!toad_instance_set_field_checked(instance, member_name, field_value,
                                               field_error, sizeof(field_error))) {
            return throw_error(env, object_node, "%s", field_error);
        }
        return field_value;
    }
    if (object.type == TOAD_VAL_CLASS) {
        toad_class_t *klass = object.data.class_val.klass;
        toad_class_t *field_owner = NULL;
        int access = toad_class_get_static_field_access(klass, member_name, &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }

        {
            char field_error[256];
            if (!toad_class_set_static_field_checked(
                    klass, member_name, field_value, field_error, sizeof(field_error)))
                return throw_error(env, object_node, "%s", field_error);
        }
        return field_value;
    }
    return zero;
}

TOAD_C_API toad_value_t toad_oop_set_member_cached_checked_numeric(
    const char *object_name, const char *member_name, double value,
    toad_oop_field_cache_t *cache, const toad_node_t *object_node, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !member_name || !env) return zero;

    toad_value_t object = toad_env_get(env, object_name);

    toad_value_t field_value = toad_val_num(value);
    if (toad_value_is_object_like(&object)) {
        toad_map_set(&object, member_name, field_value);
        toad_env_set(env, object_name, object);
        return field_value;
    }

    if (object.type == TOAD_VAL_INSTANCE) {
        toad_instance_t *instance = object.data.instance_val.instance;
        if (cache && !cache->is_static && cache->owner == instance &&
            cache->version == instance->field_version && cache->slot) {
            if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                           object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name,
                                                cache->access_level);
            }
            char field_error[256];
            if (!toad_instance_set_field_checked(instance, member_name, field_value,
                                                   field_error, sizeof(field_error))) {
                return throw_error(env, object_node, "%s", field_error);
            }
            return field_value;
        }

        toad_class_t *field_owner = NULL;
        int access = toad_class_get_instance_field_access(instance->klass, member_name,
                                                            &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }
        {
            char field_error[256];
            if (!toad_instance_set_field_checked(instance, member_name, field_value,
                                                   field_error, sizeof(field_error))) {
                return throw_error(env, object_node, "%s", field_error);
            }
        }
        toad_value_t *slot = toad_instance_get_field_slot(instance, member_name);
        if (cache && slot) {
            cache->owner = instance;
            cache->field_owner = field_owner;
            cache->version = instance->field_version;
            cache->slot = slot;
            cache->access_level = access;
            cache->is_static = 0;
        }
        return field_value;
    }

    if (object.type == TOAD_VAL_CLASS) {
        toad_class_t *klass = object.data.class_val.klass;
        if (cache && cache->is_static && cache->owner == klass &&
            cache->version == klass->static_field_version && cache->slot) {
            if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                           object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name,
                                                cache->access_level);
            }
            {
                char field_error[256];
                if (!toad_class_set_static_field_checked(
                        klass, member_name, field_value, field_error, sizeof(field_error)))
                    return throw_error(env, object_node, "%s", field_error);
            }
            return field_value;
        }

        toad_class_t *field_owner = NULL;
        int access = toad_class_get_static_field_access(klass, member_name, &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }
        {
            char field_error[256];
            if (!toad_class_set_static_field_checked(
                    klass, member_name, field_value, field_error, sizeof(field_error)))
                return throw_error(env, object_node, "%s", field_error);
        }
        toad_value_t *slot = toad_class_get_static_field_slot(klass, member_name, NULL);
        if (cache && slot) {
            cache->owner = klass;
            cache->field_owner = field_owner;
            cache->version = klass->static_field_version;
            cache->slot = slot;
            cache->access_level = access;
            cache->is_static = 1;
        }
        return field_value;
    }

    return zero;
}

TOAD_C_API toad_value_t toad_oop_set_member_numeric(const char *object_name,
                                                       const char *member_name, double value,
                                                       toad_env_t *env) {
    return toad_oop_set_member_checked_numeric(object_name, member_name, value, NULL, env);
}

TOAD_C_API toad_value_t toad_oop_instanceof_name(const char *object_name,
                                                    const char *class_name, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !class_name || !env) return zero;

    toad_value_t object = toad_env_get(env, object_name);
    toad_class_t *klass = NULL;
    toad_value_t class_value = toad_env_get(env, class_name);
    if (class_value.type == TOAD_VAL_CLASS) klass = class_value.data.class_val.klass;
    if (object.type != TOAD_VAL_INSTANCE || !klass) return toad_val_num(0.0);
    return toad_val_num((double)toad_instance_of(object.data.instance_val.instance, klass));
}

/* =========================================================================
 * MEMBER_CALL dispatch helpers
 * ========================================================================= */



/* Helper: get mutable variable from env when obj_node is a VARIABLE */
static int mc_get_var(mc_ctx_t *mc, toad_value_t *out, const char **name) {
    if (!mc || !mc->obj_node || mc->obj_node->type != TOAD_NODE_VARIABLE) return 0;
    *name = mc->obj_node->data.variable.name;
    *out = toad_env_get(mc->env, *name);
    return 1;
}

static toad_value_t *mc_prepare_call_args(mc_ctx_t *mc, toad_value_t *stack_args, size_t stack_cap) {
    size_t call_argc = mc->argc + 1;
    toad_value_t *call_args = stack_args;

    if (call_argc > stack_cap) {
        call_args = (toad_value_t*)malloc(call_argc * sizeof(toad_value_t));
        if (!call_args) return NULL;
    }

    call_args[0] = mc->obj;
    for (size_t i = 0; i < mc->argc; ++i) {
        call_args[i + 1] = mc->args[i];
        if (call_args[i + 1].type == TOAD_VAL_INTEGER) {
            call_args[i + 1] = toad_val_num((double)call_args[i + 1].data.integer);
        }
    }
    return call_args;
}

static toad_value_t unknown_method_error(mc_ctx_t *mc, const char *type_name_str) {
    return throw_error(mc ? mc->env : NULL, mc ? mc->obj_node : NULL,
                       "Unknown %s method '%s'",
                       type_name_str ? type_name_str : "object",
                       (mc && mc->method) ? mc->method : "<null>");
}

static int eval_callable_value(toad_value_t callee, const char *name, size_t argc,
                               toad_value_t *args, toad_env_t *env,
                               toad_value_t *out) {
    if (!out) return 0;

    if (callee.type == TOAD_VAL_CLASS) {
        *out = toad_oop_instantiate_class_value(callee, name, argc, args, env);
        return 1;
    }
    if (callee.type == TOAD_VAL_BOUND_METHOD) {
        *out = eval_instance_script_method(callee.data.bound_method_val.instance,
                                           callee.data.bound_method_val.method,
                                           argc, args, env);
        return 1;
    }
    if (callee.type == TOAD_VAL_FUNCTION && callee.data.function.body) {
        *out = toad_call_function_value(callee, argc, args, env);
        return 1;
    }

    return 0;
}

static int rt_dispatch_provider_method(mc_ctx_t *mc, toad_value_t *out) {
    toad_value_t provider;
    char full_name[160];
    toad_value_t stack_args[8];
    toad_value_t *call_args;
    size_t call_argc;

    if (!mc || !out || !mc->method || !mc->env ||
        !toad_value_is_object_like(&mc->obj) ||
        !toad_map_has(&mc->obj, "__ts_method_provider")) {
        return 0;
    }

    provider = toad_map_get(&mc->obj, "__ts_method_provider");
    if (provider.type != TOAD_VAL_STRING || !provider.data.string.data ||
        provider.data.string.len == 0) {
        return 0;
    }

    if (provider.data.string.len + strlen(mc->method) + 2 >= sizeof(full_name))
        return 0;

    snprintf(full_name, sizeof(full_name), "%.*s.%s",
             (int)provider.data.string.len, provider.data.string.data, mc->method);
    if (!toad_env_has_func(mc->env, full_name) && !toad_find_builtin(full_name, mc->env))
        return 0;

    call_argc = mc->argc + 1;
    call_args = mc_prepare_call_args(mc, stack_args, sizeof(stack_args) / sizeof(stack_args[0]));
    if (!call_args) return 0;

    *out = toad_call_internal(full_name, call_argc, call_args, mc->env);
    if (call_args != stack_args) free(call_args);
    return 1;
}

extern toad_value_t toad_stream_member_call(toad_value_t stream, const char *method,
                                                size_t argc, toad_value_t *args,
                                                toad_env_t *env, mem_pool_t *arena);

static toad_value_t rt_stream_make(toad_value_t source) {
    toad_value_t stream = toad_val_map();
    toad_map_set(&stream, "__ts_stream_kind",
                   toad_val_str(vstr_from_cstr("Toad.Stream.v1")));
    toad_map_set(&stream, "source", source);
    return stream;
}

static int rt_stream_is_value(toad_value_t stream) {
    toad_value_t kind;
    const char *marker = "Toad.Stream.v1";
    size_t marker_len = strlen(marker);

    if (stream.type != TOAD_VAL_MAP || !toad_map_has(&stream, "__ts_stream_kind") ||
        !toad_map_has(&stream, "source")) {
        return 0;
    }
    kind = toad_map_get(&stream, "__ts_stream_kind");
    return kind.type == TOAD_VAL_STRING &&
           kind.data.string.len == marker_len &&
           memcmp(kind.data.string.data, marker, marker_len) == 0;
}

static toad_value_t rt_stream_map_entries(toad_value_t source) {
    toad_value_t values = toad_val_list_empty();
    toad_map_iter_t it;
    toad_value_t value;

    if (!toad_value_is_object_like(&source)) return values;
    it = toad_map_iter_begin(&source);
    while (toad_map_iter_next(&it, NULL, &value)) {
        toad_list_push(&values, value);
    }
    return values;
}

static toad_value_t rt_stream_string_lines(toad_value_t text, mem_pool_t *arena) {
    toad_value_t lines = toad_val_list_empty();
    size_t start = 0;

    if (text.type != TOAD_VAL_STRING || !arena) return lines;
    for (size_t i = 0; i <= text.data.string.len; ++i) {
        if (i == text.data.string.len || text.data.string.data[i] == '\n') {
            size_t end = i;
            char *buf;
            if (end > start && text.data.string.data[end - 1] == '\r') end--;
            buf = (char *)mem_alloc(arena, end - start + 1);
            if (!buf) return lines;
            memcpy(buf, text.data.string.data + start, end - start);
            buf[end - start] = '\0';
            toad_list_push(&lines, toad_val_str(vstr_from_buf(buf, end - start)));
            start = i + 1;
        }
    }
    return lines;
}

toad_value_t eval_list_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    toad_env_t *env = mc->env;

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(mc->obj);

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return toad_val_num((double)mc->obj.data.list.count);

    if (mc->obj.type == TOAD_VAL_LIST && strcmp(m, "push") == 0 && mc->argc > 0) {
        toad_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == TOAD_VAL_LIST) {
            toad_list_push(&var, mc->args[0]);
            toad_env_set(mc->env, vn, var);
            return toad_val_num((double)var.data.list.count);
        }
    }

    if (mc->obj.type == TOAD_VAL_LIST && strcmp(m, "pop") == 0 && mc->obj.data.list.count > 0) {
        toad_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == TOAD_VAL_LIST && var.data.list.count > 0) {
            toad_value_t popped = toad_value_clone_to_env(
                toad_value_borrow(var.data.list.items[var.data.list.count - 1]),
                mc->env);
            if (mc->env->aborted) return toad_val_num(0.0);
            var.data.list.count--;
            toad_env_set(mc->env, vn, var);
            return popped;
        }
    }

    if (strcmp(m, "indexOf") == 0 && mc->argc > 0) {
        for (size_t i = 0; i < mc->obj.data.list.count; ++i) {
            toad_value_t item = mc->obj.data.list.items[i];
            if (values_match(item, mc->args[0]))
                return toad_val_num((double)i);
        }
        return toad_val_num(-1);
    }

    if (strcmp(m, "contains") == 0 && mc->argc > 0) {
        for (size_t i = 0; i < mc->obj.data.list.count; ++i) {
            toad_value_t item = mc->obj.data.list.items[i];
            if (values_match(item, mc->args[0]))
                return toad_val_num(1);
        }
        return toad_val_num(0.0);
    }

    if (strcmp(m, "reverse") == 0) {
        toad_value_t reversed = mc->obj.type == TOAD_VAL_SET
                                      ? toad_val_set_empty()
                                      : toad_val_list_empty();
        for (size_t i = 0; i < mc->obj.data.list.count; ++i) {
            if (toad_list_push(
                    &reversed,
                    toad_value_borrow(
                        mc->obj.data.list.items[mc->obj.data.list.count - 1 - i])) != 0) {
                toad_value_destroy(&reversed);
                return throw_error(env, mc->obj_node, "failed to reverse list");
            }
        }
        return reversed;
    }

    return unknown_method_error(mc, mc->obj.type == TOAD_VAL_SET ? "set" : "list");
}

toad_value_t eval_map_method(mc_ctx_t *mc) {
    const char *m = mc->method;

    if (rt_stream_is_value(mc->obj))
        return toad_stream_member_call(mc->obj, m, mc->argc, mc->args, mc->env, mc->arena);

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(rt_stream_map_entries(mc->obj));

    if (toad_map_has(&mc->obj, m)) {
        toad_value_t callee = toad_map_get(&mc->obj, m);
        toad_value_t result = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
        if (eval_callable_value(callee, m, mc->argc, mc->args, mc->env, &result)) {
            return result;
        }
    }

    {
        toad_value_t result = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
        if (rt_dispatch_provider_method(mc, &result)) {
            return result;
        }
    }

    if (strcmp(m, "size") == 0 || strcmp(m, "length") == 0)
        return toad_val_num((double)toad_map_count(&mc->obj));

    if (strcmp(m, "keys") == 0) {
        toad_value_t list = toad_val_list_empty();
        toad_map_iter_t it = toad_map_iter_begin(&mc->obj);
        const char *key;
        while (toad_map_iter_next(&it, &key, NULL)) {
            if (toad_list_push(&list, toad_val_str(vstr_from_cstr(key))) != 0) {
                toad_value_destroy(&list);
                return toad_val_list_empty();
            }
        }
        return list;
    }

    if (strcmp(m, "values") == 0) {
        toad_value_t list = toad_val_list_empty();
        toad_map_iter_t it = toad_map_iter_begin(&mc->obj);
        toad_value_t val;
        while (toad_map_iter_next(&it, NULL, &val)) {
            if (toad_list_push(&list, val) != 0) {
                toad_value_destroy(&list);
                return toad_val_list_empty();
            }
        }
        return list;
    }

    if (strcmp(m, "has") == 0 && mc->argc > 0 && mc->args[0].type == TOAD_VAL_STRING) {
        char key_buf[256];
        size_t klen = mc->args[0].data.string.len < 255 ? mc->args[0].data.string.len : 255;
        memcpy(key_buf, mc->args[0].data.string.data, klen);
        key_buf[klen] = '\0';
        return toad_val_num(toad_map_has(&mc->obj, key_buf) ? 1.0 : 0.0);
    }

    if (strcmp(m, "delete") == 0 && mc->argc > 0 && mc->args[0].type == TOAD_VAL_STRING) {
        toad_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && toad_value_is_object_like(&var)) {
            char key_buf[256];
            size_t klen = mc->args[0].data.string.len < 255 ? mc->args[0].data.string.len : 255;
            memcpy(key_buf, mc->args[0].data.string.data, klen);
            key_buf[klen] = '\0';
            toad_value_t r = toad_val_num(toad_map_delete(&var, key_buf) ? 1.0 : 0.0);
            toad_env_set(mc->env, vn, var);
            return r;
        }
    }

    return unknown_method_error(mc, "map");
}

toad_value_t eval_string_method(mc_ctx_t *mc) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    const char *m = mc->method;
    toad_value_t stack_args[8];

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return toad_val_num((double)mc->obj.data.string.len);

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(rt_stream_string_lines(mc->obj, mc->arena));

    /* Dispatch to registry: string methods expect (this, ...args) */
    size_t call_argc = mc->argc + 1;
    toad_value_t *call_args = mc_prepare_call_args(mc, stack_args, 8);
    if (!call_args) return zero;

    char full_name[128];
    snprintf(full_name, sizeof(full_name), "string.%s", m);
    toad_builtin_fn fn = toad_registry_find(full_name);
    if (!fn) fn = toad_registry_find(m);

    toad_value_t result = zero;
    int handled = 0;
    if (fn) {
        result = toad_call_builtin(fn, call_argc, call_args, mc->env);
        handled = 1;
    } else {
        /* Inline implementations for essential methods when registry is empty */
        if (strcmp(m, "indexOf") == 0 && mc->argc > 0 && mc->args[0].type == TOAD_VAL_STRING) {
            result = toad_val_num(-1);
            if (mc->args[0].data.string.len <= mc->obj.data.string.len) {
                for (size_t i = 0; i <= mc->obj.data.string.len - mc->args[0].data.string.len; ++i) {
                    if (strncmp(mc->obj.data.string.data + i, mc->args[0].data.string.data, mc->args[0].data.string.len) == 0) {
                        result = toad_val_num((double)i); break;
                    }
                }
            }
            handled = 1;
        } else if (strcmp(m, "substr") == 0 && mc->argc >= 1) {
            int start = (int)val_to_double(mc->args[0]);
            int len = (mc->argc >= 2) ? (int)val_to_double(mc->args[1]) : (int)(mc->obj.data.string.len - start);
            if (start < 0) start = 0;
            if (start >= (int)mc->obj.data.string.len) { start = 0; len = 0; }
            if (start + len > (int)mc->obj.data.string.len) len = (int)mc->obj.data.string.len - start;
            if (len < 0) len = 0;
            char *buf = (char*)mem_alloc(mc->arena, len + 1);
            memcpy(buf, mc->obj.data.string.data + start, len);
            buf[len] = '\0';
            vstr sv; sv.data = buf; sv.len = len;
            result = toad_val_str(sv);
            handled = 1;
        } else if (strcmp(m, "toUpper") == 0) {
            char *buf = (char*)mem_alloc(mc->arena, mc->obj.data.string.len + 1);
            for (size_t i = 0; i < mc->obj.data.string.len; ++i)
                buf[i] = (char)toupper((unsigned char)mc->obj.data.string.data[i]);
            buf[mc->obj.data.string.len] = '\0';
            vstr sv; sv.data = buf; sv.len = mc->obj.data.string.len;
            result = toad_val_str(sv);
            handled = 1;
        } else if (strcmp(m, "toLower") == 0) {
            char *buf = (char*)mem_alloc(mc->arena, mc->obj.data.string.len + 1);
            for (size_t i = 0; i < mc->obj.data.string.len; ++i)
                buf[i] = (char)tolower((unsigned char)mc->obj.data.string.data[i]);
            buf[mc->obj.data.string.len] = '\0';
            vstr sv; sv.data = buf; sv.len = mc->obj.data.string.len;
            result = toad_val_str(sv);
            handled = 1;
        }
    }
    if (call_args != stack_args) free(call_args);
    if (!handled) return unknown_method_error(mc, "string");
    return result;
}

toad_value_t eval_bytes_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return toad_val_num((double)mc->obj.data.bytes.len);
    return unknown_method_error(mc, "bytes");
}

toad_value_t eval_uuid_method(mc_ctx_t *mc) {
    char text[SALTS_UUID_STRING_SIZE];
    char *buf;
    size_t len;

    if (strcmp(mc->method, "toString") != 0 && strcmp(mc->method, "to_string") != 0)
        return unknown_method_error(mc, "uuid");
    if (uuid_format(mc->obj.data.uuid, text, sizeof(text)) != SALTS_OK)
        return toad_val_num(0);
    len = strlen(text);
    buf = (char *)mem_alloc(mc->arena, len + 1);
    if (!buf) return toad_val_num(0);
    memcpy(buf, text, len + 1);
    return toad_val_str(vstr_from_buf(buf, len));
}

static toad_value_t runtime_string_value(mem_pool_t *arena, const char *text) {
    size_t len;
    char *buf;
    if (!text) text = "";
    len = strlen(text);
    buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return toad_val_num(0);
    memcpy(buf, text, len + 1);
    return toad_val_str(vstr_from_buf(buf, len));
}

int toad_datetime_member_get(toad_value_t value, const char *member, toad_value_t *out) {
    const datetime_t *dt;
    time_t ts;
    if (!out || value.type != TOAD_VAL_DATETIME || !member) return 0;
    dt = &value.data.datetime;
    if (strcmp(member, "year") == 0) *out = toad_val_int(dt->year);
    else if (strcmp(member, "month") == 0) *out = toad_val_int(dt->month);
    else if (strcmp(member, "day") == 0) *out = toad_val_int(dt->day);
    else if (strcmp(member, "hour") == 0) *out = toad_val_int(dt->hour);
    else if (strcmp(member, "minute") == 0) *out = toad_val_int(dt->minute);
    else if (strcmp(member, "second") == 0) *out = toad_val_int(dt->second);
    else if (strcmp(member, "millisecond") == 0) *out = toad_val_int(dt->millisecond);
    else if (strcmp(member, "tz_offset") == 0) *out = toad_val_int(dt->tz_offset);
    else if (strcmp(member, "has_tz") == 0) *out = toad_val_bool(dt->has_tz != 0);
    else if (strcmp(member, "day_of_week") == 0) *out = toad_val_int(dt->day_of_week);
    else if (strcmp(member, "timestamp") == 0) {
        ts = datetime_to_time(dt);
        *out = toad_val_num((double)ts);
    } else {
        return 0;
    }
    return 1;
}

toad_value_t eval_datetime_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    time_t ts;
    char buf[64];
    if (strcmp(m, "timestamp") == 0 || strcmp(m, "to_time") == 0) {
        ts = datetime_to_time(&mc->obj.data.datetime);
        return toad_val_num((double)ts);
    }
    if (strcmp(m, "toString") == 0 || strcmp(m, "to_string") == 0 ||
        strcmp(m, "format_rfc822") == 0) {
        ts = datetime_to_time(&mc->obj.data.datetime);
        if (ts == (time_t)-1 || datetime_format_rfc822(ts, buf, sizeof(buf)) < 0)
            return toad_val_str(vstr_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "datetime");
}

static int runtime_offset_datetime_text(toad_offset_datetime_t value,
                                        char *out, size_t out_size) {
    int offset = value.offset_minutes;
    char sign = '+';
    int hours;
    int minutes;

    if (!out || out_size == 0) return 0;
    if (offset < 0) {
        sign = '-';
        offset = -offset;
    }
    hours = offset / 60;
    minutes = offset % 60;
    return snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d%s%c%02d:%02d",
                    value.datetime.year, value.datetime.month, value.datetime.day,
                    value.datetime.hour, value.datetime.minute, value.datetime.second,
                    value.datetime.millisecond > 0 ? "" : "", sign, hours, minutes) > 0;
}

int toad_offset_datetime_member_get(toad_value_t value, const char *member,
                                      toad_value_t *out) {
    if (!out || value.type != TOAD_VAL_OFFSET_DATETIME || !member) return 0;
    if (strcmp(member, "offset_minutes") == 0 || strcmp(member, "tz_offset") == 0)
        *out = toad_val_int(value.data.offset_datetime.offset_minutes);
    else if (strcmp(member, "datetime") == 0)
        *out = toad_val_datetime(value.data.offset_datetime.datetime);
    else {
        toad_value_t datetime_value = toad_val_datetime(value.data.offset_datetime.datetime);
        return toad_datetime_member_get(datetime_value, member, out);
    }
    return 1;
}

toad_value_t eval_offset_datetime_method(mc_ctx_t *mc) {
    char buf[64];
    if (strcmp(mc->method, "timestamp") == 0 || strcmp(mc->method, "to_time") == 0)
        return toad_val_num((double)datetime_to_time(&mc->obj.data.offset_datetime.datetime));
    if (strcmp(mc->method, "toString") == 0 || strcmp(mc->method, "to_string") == 0) {
        if (!runtime_offset_datetime_text(mc->obj.data.offset_datetime, buf, sizeof(buf)))
            return toad_val_str(vstr_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "offset_datetime");
}

static int runtime_date_text(toad_date_t date, char *out, size_t out_size) {
    return out && out_size > 0 &&
           snprintf(out, out_size, "%04d-%02d-%02d", date.year, date.month, date.day) > 0;
}

static int runtime_time_text(toad_time_t time, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    if (time.millisecond > 0)
        return snprintf(out, out_size, "%02d:%02d:%02d.%03d", time.hour, time.minute,
                        time.second, time.millisecond) > 0;
    return snprintf(out, out_size, "%02d:%02d:%02d", time.hour, time.minute,
                    time.second) > 0;
}

static int runtime_duration_text(int64_t ms, char *out, size_t out_size) {
    int64_t sign = ms < 0 ? -1 : 1;
    int64_t rem = ms < 0 ? -ms : ms;
    int64_t h = rem / 3600000;
    int64_t m;
    int64_t s;
    rem %= 3600000;
    m = rem / 60000;
    rem %= 60000;
    s = rem / 1000;
    rem %= 1000;
    if (!out || out_size == 0) return 0;
    return snprintf(out, out_size, "%s%lld:%02lld:%02lld.%03lld",
                    sign < 0 ? "-" : "", (long long)h, (long long)m,
                    (long long)s, (long long)rem) > 0;
}

static int runtime_decimal_text(toad_decimal_t value, char *out, size_t out_size) {
    char digits[32];
    char *p = digits + sizeof(digits);
    uint64_t mag;
    size_t digit_count;
    size_t pos = 0;
    int negative;

    if (!out || out_size == 0 || value.scale < 0) return 0;
    while (value.scale > 0 && value.mantissa % 10 == 0) {
        value.mantissa /= 10;
        value.scale--;
    }
    if (value.mantissa == 0) value.scale = 0;
    negative = value.mantissa < 0;
    mag = negative ? (uint64_t)(-(value.mantissa + 1)) + 1ULL : (uint64_t)value.mantissa;
    *--p = '\0';
    do {
        *--p = (char)('0' + (mag % 10ULL));
        mag /= 10ULL;
    } while (mag != 0);
    digit_count = strlen(p);
    if (negative) {
        if (pos + 1 >= out_size) return 0;
        out[pos++] = '-';
    }
    if (value.scale == 0) {
        if (pos + digit_count >= out_size) return 0;
        memcpy(out + pos, p, digit_count + 1);
        return 1;
    }
    if ((size_t)value.scale >= digit_count) {
        size_t zeros = (size_t)value.scale - digit_count;
        if (pos + 2 + zeros + digit_count >= out_size) return 0;
        out[pos++] = '0';
        out[pos++] = '.';
        while (zeros-- > 0) out[pos++] = '0';
        memcpy(out + pos, p, digit_count);
        pos += digit_count;
        out[pos] = '\0';
        return 1;
    }
    {
        size_t whole = digit_count - (size_t)value.scale;
        if (pos + digit_count + 1 >= out_size) return 0;
        memcpy(out + pos, p, whole);
        pos += whole;
        out[pos++] = '.';
        memcpy(out + pos, p + whole, (size_t)value.scale);
        pos += (size_t)value.scale;
        out[pos] = '\0';
        return 1;
    }
}

int toad_date_member_get(toad_value_t value, const char *member, toad_value_t *out) {
    if (!out || value.type != TOAD_VAL_DATE || !member) return 0;
    if (strcmp(member, "year") == 0) *out = toad_val_int(value.data.date.year);
    else if (strcmp(member, "month") == 0) *out = toad_val_int(value.data.date.month);
    else if (strcmp(member, "day") == 0) *out = toad_val_int(value.data.date.day);
    else return 0;
    return 1;
}

int toad_time_member_get(toad_value_t value, const char *member, toad_value_t *out) {
    if (!out || value.type != TOAD_VAL_TIME || !member) return 0;
    if (strcmp(member, "hour") == 0) *out = toad_val_int(value.data.time.hour);
    else if (strcmp(member, "minute") == 0) *out = toad_val_int(value.data.time.minute);
    else if (strcmp(member, "second") == 0) *out = toad_val_int(value.data.time.second);
    else if (strcmp(member, "millisecond") == 0) *out = toad_val_int(value.data.time.millisecond);
    else return 0;
    return 1;
}

int toad_duration_member_get(toad_value_t value, const char *member, toad_value_t *out) {
    if (!out || value.type != TOAD_VAL_DURATION || !member) return 0;
    if (strcmp(member, "milliseconds") == 0 || strcmp(member, "ms") == 0)
        *out = toad_val_int(value.data.duration_ms);
    else if (strcmp(member, "seconds") == 0)
        *out = toad_val_num((double)value.data.duration_ms / 1000.0);
    else return 0;
    return 1;
}

int toad_decimal_member_get(toad_value_t value, const char *member, toad_value_t *out) {
    if (!out || value.type != TOAD_VAL_DECIMAL || !member) return 0;
    if (strcmp(member, "mantissa") == 0)
        *out = toad_val_int(value.data.decimal.mantissa);
    else if (strcmp(member, "scale") == 0)
        *out = toad_val_int(value.data.decimal.scale);
    else return 0;
    return 1;
}

int toad_money_member_get(toad_value_t value, const char *member, toad_value_t *out) {
    if (!out || value.type != TOAD_VAL_MONEY || !member) return 0;
    if (strcmp(member, "amount") == 0) *out = toad_val_decimal(value.data.money.amount);
    else if (strcmp(member, "currency") == 0) {
        vstr sv;
        sv.data = value.data.money.currency;
        sv.len = 3;
        *out = toad_val_str(sv);
    } else return 0;
    return 1;
}

int toad_enum_member_get(toad_value_t value, const char *member, toad_value_t *out) {
    if (!out || (value.type != TOAD_VAL_ENUM && value.type != TOAD_VAL_FLAGS) || !member)
        return 0;
    if (strcmp(member, "value") == 0) *out = toad_val_int(value.data.enum_val.value);
    else if (strcmp(member, "name") == 0 || strcmp(member, "symbol") == 0)
        *out = toad_val_str(value.data.enum_val.symbol);
    else if (strcmp(member, "type") == 0)
        *out = toad_val_str(value.data.enum_val.type_name);
    else if (strcmp(member, "is_flags") == 0)
        *out = toad_val_bool(value.type == TOAD_VAL_FLAGS);
    else return 0;
    return 1;
}

static const char *runtime_typed_array_kind_name(toad_typed_array_kind_t kind) {
    switch (kind) {
        case TOAD_TYPED_I32: return "i32";
        case TOAD_TYPED_I64: return "i64";
        case TOAD_TYPED_F32: return "f32";
        case TOAD_TYPED_F64: return "f64";
        default: return "";
    }
}

toad_value_t toad_typed_array_get_value(toad_value_t value, size_t index) {
    if (value.type != TOAD_VAL_TYPED_ARRAY || !value.data.typed_array.data ||
        index >= value.data.typed_array.count)
        return toad_val_num(0);
    switch (value.data.typed_array.kind) {
        case TOAD_TYPED_I32:
            return toad_val_int(((const int32_t *)value.data.typed_array.data)[index]);
        case TOAD_TYPED_I64:
            return toad_val_int(((const int64_t *)value.data.typed_array.data)[index]);
        case TOAD_TYPED_F32:
            return toad_val_num((double)((const float *)value.data.typed_array.data)[index]);
        case TOAD_TYPED_F64:
            return toad_val_num(((const double *)value.data.typed_array.data)[index]);
        default:
            return toad_val_num(0);
    }
}

int toad_typed_array_member_get(toad_value_t value, const char *member,
                                  toad_value_t *out) {
    if (!out || value.type != TOAD_VAL_TYPED_ARRAY || !member) return 0;
    if (strcmp(member, "length") == 0 || strcmp(member, "size") == 0)
        *out = toad_val_int((int64_t)value.data.typed_array.count);
    else if (strcmp(member, "kind") == 0)
        *out = toad_val_str(vstr_from_cstr(runtime_typed_array_kind_name(value.data.typed_array.kind)));
    else return 0;
    return 1;
}

toad_value_t eval_typed_array_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return toad_val_int((int64_t)mc->obj.data.typed_array.count);
    if (strcmp(m, "kind") == 0)
        return runtime_string_value(mc->arena,
                                    runtime_typed_array_kind_name(mc->obj.data.typed_array.kind));
    if (strcmp(m, "toList") == 0 || strcmp(m, "to_list") == 0) {
        toad_value_t list = toad_val_list_empty();
        for (size_t i = 0; i < mc->obj.data.typed_array.count; ++i)
            toad_list_push(&list, toad_typed_array_get_value(mc->obj, i));
        return list;
    }
    if (strcmp(m, "toVector") == 0 || strcmp(m, "to_vector") == 0) {
        double *data = (double *)mem_alloc(mc->arena,
                                           mc->obj.data.typed_array.count * sizeof(double));
        if (!data && mc->obj.data.typed_array.count > 0) return toad_val_vec(NULL, 0);
        for (size_t i = 0; i < mc->obj.data.typed_array.count; ++i)
            data[i] = val_to_double(toad_typed_array_get_value(mc->obj, i));
        return toad_val_vec(data, mc->obj.data.typed_array.count);
    }
    return unknown_method_error(mc, "typed_array");
}

toad_value_t eval_date_method(mc_ctx_t *mc) {
    char buf[32];
    if (strcmp(mc->method, "toString") != 0 && strcmp(mc->method, "to_string") != 0)
        return unknown_method_error(mc, "date");
    if (!runtime_date_text(mc->obj.data.date, buf, sizeof(buf)))
        return toad_val_str(vstr_from_cstr(""));
    return runtime_string_value(mc->arena, buf);
}

toad_value_t eval_time_method(mc_ctx_t *mc) {
    char buf[32];
    if (strcmp(mc->method, "toString") != 0 && strcmp(mc->method, "to_string") != 0)
        return unknown_method_error(mc, "time");
    if (!runtime_time_text(mc->obj.data.time, buf, sizeof(buf)))
        return toad_val_str(vstr_from_cstr(""));
    return runtime_string_value(mc->arena, buf);
}

toad_value_t eval_duration_method(mc_ctx_t *mc) {
    char buf[64];
    if (strcmp(mc->method, "milliseconds") == 0 || strcmp(mc->method, "ms") == 0)
        return toad_val_int(mc->obj.data.duration_ms);
    if (strcmp(mc->method, "seconds") == 0)
        return toad_val_num((double)mc->obj.data.duration_ms / 1000.0);
    if (strcmp(mc->method, "toString") == 0 || strcmp(mc->method, "to_string") == 0) {
        if (!runtime_duration_text(mc->obj.data.duration_ms, buf, sizeof(buf)))
            return toad_val_str(vstr_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "duration");
}

toad_value_t eval_decimal_method(mc_ctx_t *mc) {
    char buf[64];
    if (strcmp(mc->method, "mantissa") == 0)
        return toad_val_int(mc->obj.data.decimal.mantissa);
    if (strcmp(mc->method, "scale") == 0)
        return toad_val_int(mc->obj.data.decimal.scale);
    if (strcmp(mc->method, "toString") == 0 || strcmp(mc->method, "to_string") == 0) {
        if (!runtime_decimal_text(mc->obj.data.decimal, buf, sizeof(buf)))
            return toad_val_str(vstr_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "decimal");
}

toad_value_t eval_vector_method(mc_ctx_t *mc) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    const char *m = mc->method;
    toad_value_t stack_args[8];

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return toad_val_num((double)mc->obj.data.vector.size);

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(mc->obj);

    if (strcmp(m, "push") == 0 && mc->argc > 0 &&
        (mc->args[0].type == TOAD_VAL_NUMBER || mc->args[0].type == TOAD_VAL_INTEGER)) {
        toad_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == TOAD_VAL_VECTOR) {
            size_t new_sz = var.data.vector.size + 1;
            double *new_data = (double*)mem_alloc(mc->arena, new_sz * sizeof(double));
            if (var.data.vector.data)
                memcpy(new_data, var.data.vector.data, var.data.vector.size * sizeof(double));
            new_data[new_sz - 1] = val_to_double(mc->args[0]);
            var.data.vector.data = new_data;
            var.data.vector.size = new_sz;
            toad_env_set(mc->env, vn, var);
            return toad_val_num((double)new_sz);
        }
    }

    if (strcmp(m, "pop") == 0 && mc->obj.data.vector.size > 0) {
        toad_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == TOAD_VAL_VECTOR && var.data.vector.size > 0) {
            toad_value_t popped = toad_val_num(var.data.vector.data[var.data.vector.size - 1]);
            var.data.vector.size--;
            toad_env_set(mc->env, vn, var);
            return popped;
        }
    }

    if (strcmp(m, "indexOf") == 0 && mc->argc > 0 &&
        (mc->args[0].type == TOAD_VAL_NUMBER || mc->args[0].type == TOAD_VAL_INTEGER)) {
        double search_val = val_to_double(mc->args[0]);
        for (size_t i = 0; i < mc->obj.data.vector.size; ++i) {
            if (fabs(mc->obj.data.vector.data[i] - search_val) < 1e-9)
                return toad_val_num((double)i);
        }
        return toad_val_num(-1);
    }

    if (strcmp(m, "reverse") == 0) {
        double *data = (double*)mem_alloc(mc->arena, mc->obj.data.vector.size * sizeof(double));
        for (size_t i = 0; i < mc->obj.data.vector.size; ++i)
            data[i] = mc->obj.data.vector.data[mc->obj.data.vector.size - 1 - i];
        return toad_val_vec(data, mc->obj.data.vector.size);
    }

    /* Registry dispatch: try multiple namespace prefixes */
    size_t call_argc = mc->argc + 1;
    toad_value_t *call_args = mc_prepare_call_args(mc, stack_args, 8);
    if (!call_args) return zero;

    static const char *prefixes[] = { "vec_", "ta.", "ts.", "stats.", "math.", NULL };
    toad_builtin_fn fn = NULL;
    char full_name[128];

    for (const char **p = prefixes; *p && !fn; ++p) {
        snprintf(full_name, sizeof(full_name), "%s%s", *p, m);
        fn = toad_find_builtin(full_name, mc->env);
    }
    if (!fn) fn = toad_find_builtin(m, mc->env);

    toad_value_t result = zero;
    int handled = 0;
    if (fn) {
        result = toad_call_builtin(fn, call_argc, call_args, mc->env);
        handled = 1;
    } else {
        /* Inline implementations for basic vector ops */
        if (strcmp(m, "sum") == 0) {
            double sum = 0;
            for (size_t i = 0; i < mc->obj.data.vector.size; ++i) sum += mc->obj.data.vector.data[i];
            result = toad_val_num(sum);
            handled = 1;
        } else if (strcmp(m, "avg") == 0 || strcmp(m, "mean") == 0) {
            if (mc->obj.data.vector.size > 0) {
                double sum = 0;
                for (size_t i = 0; i < mc->obj.data.vector.size; ++i) sum += mc->obj.data.vector.data[i];
                result = toad_val_num(sum / (double)mc->obj.data.vector.size);
            } else {
                result = toad_val_num(0.0);
            }
            handled = 1;
        } else if (strcmp(m, "min") == 0 && mc->obj.data.vector.size > 0) {
            double v = mc->obj.data.vector.data[0];
            for (size_t i = 1; i < mc->obj.data.vector.size; ++i)
                if (mc->obj.data.vector.data[i] < v) v = mc->obj.data.vector.data[i];
            result = toad_val_num(v);
            handled = 1;
        } else if (strcmp(m, "max") == 0 && mc->obj.data.vector.size > 0) {
            double v = mc->obj.data.vector.data[0];
            for (size_t i = 1; i < mc->obj.data.vector.size; ++i)
                if (mc->obj.data.vector.data[i] > v) v = mc->obj.data.vector.data[i];
            result = toad_val_num(v);
            handled = 1;
        }
    }
    if (call_args != stack_args) free(call_args);
    if (!handled) return unknown_method_error(mc, "vector");
    return result;
}

TOAD_C_API toad_value_t toad_member_call_checked_values(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, toad_value_t *args, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!object_name || !method_name || !env || (argc > 0 && !args)) return zero;

    mc_ctx_t mc = {
        .method = method_name,
        .obj = toad_env_get(env, object_name),
        .args = args,
        .argc = argc,
        .obj_node = (toad_node_t *)object_node,
        .env = env,
        .arena = &env->arena,
    };

    switch (mc.obj.type) {
        case TOAD_VAL_LIST:
        case TOAD_VAL_SET:   return eval_list_method(&mc);
        case TOAD_VAL_MAP:
        case TOAD_VAL_OBJECT: return eval_map_method(&mc);
        case TOAD_VAL_BYTES:  return eval_bytes_method(&mc);
        case TOAD_VAL_UUID:   return eval_uuid_method(&mc);
        case TOAD_VAL_DATETIME: return eval_datetime_method(&mc);
        case TOAD_VAL_DATE: return eval_date_method(&mc);
        case TOAD_VAL_TIME: return eval_time_method(&mc);
        case TOAD_VAL_DURATION: return eval_duration_method(&mc);
        case TOAD_VAL_DECIMAL: return eval_decimal_method(&mc);
        case TOAD_VAL_OFFSET_DATETIME: return eval_offset_datetime_method(&mc);
        case TOAD_VAL_TYPED_ARRAY: return eval_typed_array_method(&mc);
        case TOAD_VAL_STRING: return eval_string_method(&mc);
        case TOAD_VAL_VECTOR: return eval_vector_method(&mc);
        case TOAD_VAL_INSTANCE:
        case TOAD_VAL_CLASS:
            return toad_oop_call_method_checked_values(object_name, method_name,
                                                         object_node, argc, args, env);
        default: {
            char full_name[256];
            int written = snprintf(full_name, sizeof(full_name), "%s.%s",
                                   object_name, method_name);
            if (written < 0 || (size_t)written >= sizeof(full_name)) {
                return throw_error(env, object_node, "Qualified function name is too long");
            }
            return toad_call_internal(full_name, argc, args, env);
        }
    }
}

TOAD_C_API toad_value_t toad_member_call_checked_numeric(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    size_t argc, const double *argv, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    toad_value_t *args = numeric_args_to_values(argc, argv);
    if (argc > 0 && !args) return zero;

    toad_value_t result = toad_member_call_checked_values(
        object_name, method_name, object_node, argc, args, env);
    free(args);
    return result;
}

TOAD_C_API toad_value_t toad_member_call_checked_value_nodes(
    const char *object_name, const char *method_name, const toad_node_t *object_node,
    const toad_node_t *call_node, toad_env_t *env) {
    toad_value_t zero = { .type = TOAD_VAL_NUMBER, .data.number = 0.0 };
    if (!call_node || call_node->type != TOAD_NODE_MEMBER_CALL || !env) return zero;

    size_t actual_count = 0;
    toad_value_t *args = eval_expand_args(call_node->data.member_call.args,
                                            call_node->data.member_call.arg_count,
                                            env, &actual_count);
    if (!args && actual_count == 0) return zero;

    toad_value_t result = toad_member_call_checked_values(
        object_name, method_name, object_node, actual_count, args, env);
    free(args);
    return result;
}

int toad_env_last_line(const toad_env_t *env) {
    return env ? env->last_line : 0;
}

int toad_env_last_column(const toad_env_t *env) {
    return env ? env->last_column : 0;
}


/* ========================================================================
 * OOP - Class Registration and Lookup
 * ======================================================================== */

void toad_env_register_class(toad_env_t *env, const char *name, toad_class_t *klass) {
    if (!env || !name || !klass) return;
    toad_env_set_local(env, name, toad_val_class(klass));
}

toad_class_t *toad_env_lookup_class(toad_env_t *env, const char *name) {
    if (!env || !name) return NULL;
    toad_value_t value = toad_env_get(env, name);
    return value.type == TOAD_VAL_CLASS ? value.data.class_val.klass : NULL;
}

/* =========================================================================
 * Closure free-variable collection (capture-by-need snapshots)
 * ========================================================================= */

typedef struct {
    char **names;
    size_t count;
    size_t capacity;
    mem_pool_t *arena;
} toad_free_var_collector_t;

static int toad_collector_add(toad_free_var_collector_t *c, const char *name) {
    char **new_names;
    size_t i;
    if (!c || !name || !*name) return 1;
    for (i = 0; i < c->count; ++i) {
        if (strcmp(c->names[i], name) == 0) return 1;
    }
    if (c->count == c->capacity) {
        size_t new_cap = c->capacity ? c->capacity * 2 : 16;
        new_names = (char **)mem_alloc(c->arena, new_cap * sizeof(char *));
        if (!new_names) return 0;
        if (c->names) memcpy(new_names, c->names, c->count * sizeof(char *));
        c->names = new_names;
        c->capacity = new_cap;
    }
    c->names[c->count] = (char *)mem_alloc(c->arena, strlen(name) + 1);
    if (!c->names[c->count]) return 0;
    memcpy(c->names[c->count], name, strlen(name) + 1);
    c->count++;
    return 1;
}

static void toad_collect_free_vars_node(const toad_node_t *node,
                                          toad_free_var_collector_t *c) {
    size_t i;
    if (!node) return;
    switch (node->type) {
        case TOAD_NODE_VARIABLE:
            (void)toad_collector_add(c, node->data.variable.name);
            break;
        case TOAD_NODE_BINARY_OP:
            toad_collect_free_vars_node(node->data.binary.left, c);
            toad_collect_free_vars_node(node->data.binary.right, c);
            break;
        case TOAD_NODE_FUNCTION_CALL:
            /* The callee name is a free-variable reference (e.g. a(1)). */
            (void)toad_collector_add(c, node->data.function.name);
            for (i = 0; i < node->data.function.arg_count; ++i)
                toad_collect_free_vars_node(node->data.function.args[i], c);
            break;
        case TOAD_NODE_ASSIGNMENT:
            /* target name is a local write; the value may reference free vars */
            toad_collect_free_vars_node(node->data.assignment.value, c);
            break;
        case TOAD_NODE_IF:
            toad_collect_free_vars_node(node->data.if_stmt.condition, c);
            toad_collect_free_vars_node(node->data.if_stmt.if_branch, c);
            toad_collect_free_vars_node(node->data.if_stmt.else_branch, c);
            break;
        case TOAD_NODE_WHILE:
            toad_collect_free_vars_node(node->data.while_loop.condition, c);
            toad_collect_free_vars_node(node->data.while_loop.body, c);
            break;
        case TOAD_NODE_FOR:
            toad_collect_free_vars_node(node->data.for_loop.init, c);
            toad_collect_free_vars_node(node->data.for_loop.condition, c);
            toad_collect_free_vars_node(node->data.for_loop.post, c);
            toad_collect_free_vars_node(node->data.for_loop.body, c);
            break;
        case TOAD_NODE_BLOCK:
            for (i = 0; i < node->data.block.count; ++i)
                toad_collect_free_vars_node(node->data.block.statements[i], c);
            break;
        case TOAD_NODE_FLOW:
            toad_collect_free_vars_node(node->data.flow.value, c);
            break;
        case TOAD_NODE_VECTOR:
            for (i = 0; i < node->data.vector.count; ++i)
                toad_collect_free_vars_node(node->data.vector.elements[i], c);
            break;
        case TOAD_NODE_INDEX:
            toad_collect_free_vars_node(node->data.index_access.array, c);
            toad_collect_free_vars_node(node->data.index_access.index, c);
            break;
        case TOAD_NODE_SLICE:
            toad_collect_free_vars_node(node->data.slice.array, c);
            toad_collect_free_vars_node(node->data.slice.start, c);
            toad_collect_free_vars_node(node->data.slice.end, c);
            break;
        case TOAD_NODE_FUNCTION_DEFINITION:
        case TOAD_NODE_FUNCTION_EXPRESSION:
        case TOAD_NODE_GENERATOR_FUNCTION:
            /* Recurse into the body but not the parameter list: nested bodies
             * may reference outer free variables, and over-capturing is safe. */
            if (node->type == TOAD_NODE_FUNCTION_DEFINITION)
                toad_collect_free_vars_node(node->data.func_def.body, c);
            else if (node->type == TOAD_NODE_FUNCTION_EXPRESSION)
                toad_collect_free_vars_node(node->data.func_def.body, c);
            else
                toad_collect_free_vars_node(node->data.generator_def.body, c);
            break;
        case TOAD_NODE_MEMBER_CALL:
            toad_collect_free_vars_node(node->data.member_call.object, c);
            for (i = 0; i < node->data.member_call.arg_count; ++i)
                toad_collect_free_vars_node(node->data.member_call.args[i], c);
            break;
        case TOAD_NODE_CONSTANT_DECL:
            toad_collect_free_vars_node(node->data.assignment.value, c);
            break;
        case TOAD_NODE_SWITCH:
            toad_collect_free_vars_node(node->data.switch_stmt.value, c);
            for (i = 0; i < node->data.switch_stmt.case_count; ++i)
                toad_collect_free_vars_node(node->data.switch_stmt.cases[i], c);
            toad_collect_free_vars_node(node->data.switch_stmt.default_case, c);
            break;
        case TOAD_NODE_DO_WHILE:
            toad_collect_free_vars_node(node->data.do_while.body, c);
            toad_collect_free_vars_node(node->data.do_while.condition, c);
            break;
        case TOAD_NODE_MAP_LITERAL:
            for (i = 0; i < node->data.map_literal.count; ++i)
                toad_collect_free_vars_node(node->data.map_literal.values[i], c);
            break;
        case TOAD_NODE_MEMBER_ACCESS:
            toad_collect_free_vars_node(node->data.member_access.object, c);
            break;
        case TOAD_NODE_MEMBER_SET:
            toad_collect_free_vars_node(node->data.member_set.object, c);
            toad_collect_free_vars_node(node->data.member_set.value, c);
            break;
        case TOAD_NODE_FOR_IN:
            toad_collect_free_vars_node(node->data.for_in.collection, c);
            toad_collect_free_vars_node(node->data.for_in.body, c);
            break;
        case TOAD_NODE_DESTRUCTURING_ASSIGNMENT:
            toad_collect_free_vars_node(node->data.destructuring.targets, c);
            toad_collect_free_vars_node(node->data.destructuring.value, c);
            break;
        case TOAD_NODE_TRY_CATCH:
            toad_collect_free_vars_node(node->data.try_catch.try_body, c);
            toad_collect_free_vars_node(node->data.try_catch.catch_body, c);
            break;
        case TOAD_NODE_THROW:
            toad_collect_free_vars_node(node->data.throw_stmt.value, c);
            break;
        case TOAD_NODE_YIELD:
            toad_collect_free_vars_node(node->data.yield_expr.value, c);
            break;
        case TOAD_NODE_CLASS_DEF:
            toad_collect_free_vars_node(node->data.class_def.constructor, c);
            for (i = 0; i < node->data.class_def.method_count; ++i)
                toad_collect_free_vars_node(node->data.class_def.methods[i], c);
            for (i = 0; i < node->data.class_def.static_method_count; ++i)
                toad_collect_free_vars_node(node->data.class_def.static_methods[i], c);
            break;
        case TOAD_NODE_METHOD:
            toad_collect_free_vars_node(node->data.method.body, c);
            break;
        case TOAD_NODE_FIELD_DECL:
            toad_collect_free_vars_node(node->data.field_decl.initializer, c);
            break;
        case TOAD_NODE_NEW:
            for (i = 0; i < node->data.new_expr.arg_count; ++i)
                toad_collect_free_vars_node(node->data.new_expr.args[i], c);
            break;
        case TOAD_NODE_SUPER:
            for (i = 0; i < node->data.super_expr.arg_count; ++i)
                toad_collect_free_vars_node(node->data.super_expr.args[i], c);
            break;
        case TOAD_NODE_INSTANCEOF:
            toad_collect_free_vars_node(node->data.instanceof_expr.object, c);
            toad_collect_free_vars_node(node->data.instanceof_expr.class_expr, c);
            break;
        case TOAD_NODE_SPREAD:
            toad_collect_free_vars_node(node->data.spread.child, c);
            break;
        default:
            break;
    }
}

char **toad_collect_closure_free_vars(const toad_node_t *body,
                                        toad_node_t *const *arg_params,
                                        size_t arg_count,
                                        mem_pool_t *arena) {
    toad_free_var_collector_t c;
    char **result;
    size_t i, j, out = 0;
    if (!arena || !body) return NULL;
    memset(&c, 0, sizeof(c));
    c.arena = arena;
    toad_collect_free_vars_node(body, &c);
    if (c.count == 0) {
        result = (char **)mem_alloc(arena, sizeof(char *));
        if (result) result[0] = NULL;
        return result;
    }
    result = (char **)mem_alloc(arena, (c.count + 1) * sizeof(char *));
    if (!result) return NULL;
    for (i = 0; i < c.count; ++i) {
        int is_param = 0;
        for (j = 0; j < arg_count; ++j) {
            if (arg_params[j] && arg_params[j]->type == TOAD_NODE_VARIABLE &&
                arg_params[j]->data.variable.name &&
                strcmp(c.names[i], arg_params[j]->data.variable.name) == 0) {
                is_param = 1;
                break;
            }
        }
        if (!is_param) result[out++] = c.names[i];
    }
    result[out] = NULL;
    return result;
}
