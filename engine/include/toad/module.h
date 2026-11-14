/**
 * @file module.h
 * @brief Public module descriptor API for engine.
 *
 * External modules (e.g. fin under toad_script) include this header
 * to define toad_module_t instances without depending on any private
 * engine internals.
 */
#ifndef TOAD_MODULE_H
#define TOAD_MODULE_H

#include <toad/types.h>
#include "memory/buffer.h"
#include <cmeta/function.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Module function signature & descriptor types
 * ========================================================================= */

typedef toad_value_t (*toad_builtin_fn)(
    size_t argc, toad_value_t *args,
    toad_env_t *env, mem_pool_t *scratch);

typedef struct {
    const char *name;
    toad_builtin_fn fn;
} toad_func_entry_t;

/*
 * Admission-time CMeta projection of the exact native Engine builtin adapter.
 *
 * The logical language remains dynamically typed. This descriptor therefore
 * reflects the concrete VM adapter contract once, before execution:
 *
 *   toad_value_t fn(size_t argc,
 *                     toad_value_t *args,
 *                     toad_env_t *env,
 *                     mem_pool_t *scratch)
 *
 * Result ownership is intentionally CMETA_RESULT_UNKNOWN at this raw adapter
 * boundary. toad_value_t is a dynamic carrier whose payload may be borrowed
 * from the per-call scratch pool or may already own retained storage. A raw
 * builtin result must therefore not escape directly. toad_call_builtin()
 * clones/promotes the result into the target env owner, destroys the raw value,
 * and only then destroys scratch.
 *
 * Consumers may retain the copied metadata only as long as this reflection
 * storage remains alive. Invocation continues through the exact entry->fn
 * pointer. Callers that need an escaping result must preserve the
 * toad_call_builtin() promotion boundary rather than infer ownership from the
 * by-value C return type.
 */
typedef struct toad_function_reflection {
    cmeta_param_desc params[4];
    cmeta_function_desc function;
    toad_builtin_fn invoke;
} toad_function_reflection_t;

TOAD_C_API int toad_func_entry_reflect(
    const toad_func_entry_t *entry,
    toad_function_reflection_t *out);
TOAD_C_API int toad_module_function_reflect(
    const toad_module_t *module,
    size_t index,
    toad_function_reflection_t *out);

struct toad_module_s {
    const char *module_name;
    const toad_func_entry_t *entries;
    size_t count;
};

/* =========================================================================
 * Value constructors (inline helpers for module implementations)
 * ========================================================================= */

static inline toad_value_t toad_val_num(double v) {
    toad_value_t val = {0};
    val.type = TOAD_VAL_NUMBER;
    val.data.number = v;
    return val;
}

static inline toad_value_t toad_val_int(int64_t v) {
    toad_value_t val = {0};
    val.type = TOAD_VAL_INTEGER;
    val.data.integer = v;
    return val;
}

static inline toad_value_t toad_val_bool(int v) {
    toad_value_t val = {0};
    val.type = TOAD_VAL_BOOL;
    val.data.boolean = v ? 1 : 0;
    return val;
}

static inline toad_value_t toad_val_vec(double *data, size_t size) {
    toad_value_t val = {0};
    val.type = TOAD_VAL_VECTOR;
    val.data.vector.data = data;
    val.data.vector.size = size;
    return val;
}

static inline toad_value_t toad_val_str(vstr v) {
    toad_value_t val = {0};
    val.type = TOAD_VAL_STRING;
    val.data.string = v;
    return val;
}

static inline toad_value_t toad_val_bytes(vstr v) {
    toad_value_t val = {0};
    val.type = TOAD_VAL_BYTES;
    val.data.bytes = v;
    return val;
}

/* Copy the caller's canonical 16-byte UUID; no pointer is retained. */
static inline toad_value_t toad_val_uuid(const uuid_t v) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_UUID;
    memcpy(val.data.uuid, v, sizeof(val.data.uuid));
    return val;
}

static inline toad_value_t toad_val_datetime(datetime_t v) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_DATETIME;
    val.data.datetime = v;
    return val;
}

static inline toad_value_t toad_val_date(toad_date_t v) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_DATE;
    val.data.date = v;
    return val;
}

static inline toad_value_t toad_val_time(toad_time_t v) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_TIME;
    val.data.time = v;
    return val;
}

static inline toad_value_t toad_val_duration(int64_t ms) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_DURATION;
    val.data.duration_ms = ms;
    return val;
}

static inline toad_value_t toad_val_decimal(toad_decimal_t v) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_DECIMAL;
    val.data.decimal = v;
    return val;
}

static inline toad_value_t toad_val_bigint(vstr v) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_BIGINT;
    val.data.bigint.text = v;
    return val;
}

static inline toad_value_t toad_val_money(toad_money_t v) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_MONEY;
    val.data.money = v;
    return val;
}

static inline toad_value_t toad_val_enum(vstr type_name, vstr symbol, int64_t value, int is_flags) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = is_flags ? TOAD_VAL_FLAGS : TOAD_VAL_ENUM;
    val.data.enum_val.type_name = type_name;
    val.data.enum_val.symbol = symbol;
    val.data.enum_val.value = value;
    val.data.enum_val.is_flags = is_flags ? 1 : 0;
    return val;
}

static inline toad_value_t toad_val_offset_datetime(datetime_t datetime,
                                                        int offset_minutes) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_OFFSET_DATETIME;
    val.data.offset_datetime.datetime = datetime;
    val.data.offset_datetime.offset_minutes = offset_minutes;
    return val;
}

static inline toad_value_t toad_val_typed_array(toad_typed_array_kind_t kind,
                                                    void *data, size_t count,
                                                    int heap_owned) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_TYPED_ARRAY;
    val.data.typed_array.kind = kind;
    val.data.typed_array.data = data;
    val.data.typed_array.count = count;
    val.data.typed_array.heap_owned = heap_owned;
    return val;
}

toad_value_t toad_typed_array_get_value(toad_value_t value, size_t index);

/* =========================================================================
 * Script value ownership
 *
 * Borrowed constructors above are valid only for the immediate call.  Values
 * crossing an env/container/task boundary must use copy/retain/release.  The
 * retain operation is for mem_buffer-backed scalar/vector payloads; containers
 * cross owner boundaries through copy_to_pool/copy_to_env.
 * ========================================================================= */

int toad_value_copy_to_pool(toad_value_t value, mem_pool_t *pool,
                              toad_value_t *out);
/* Captured-scope reference counting used by function values (see engine.h). */
TOAD_C_API toad_env_t *toad_env_snapshot_names(toad_env_t *env,
                                              const char *const *names,
                                              size_t name_count);
TOAD_C_API char **toad_collect_closure_free_vars(const toad_node_t *body,
                                                toad_node_t *const *arg_params,
                                                size_t arg_count,
                                                mem_pool_t *arena);
TOAD_C_API void toad_env_retain(toad_env_t *env);
TOAD_C_API void toad_env_release(toad_env_t *env);
TOAD_C_API void toad_env_sweep_closures(toad_env_t *root);

int toad_value_copy_to_env(toad_value_t value, toad_env_t *env,
                             toad_value_t *out);
toad_value_t toad_value_retain(toad_value_t value);
toad_value_t toad_value_borrow(toad_value_t value);
void toad_value_release_storage(toad_value_t *value);
void toad_value_destroy(toad_value_t *value);
void toad_values_destroy(toad_value_t *values, size_t count);

/* =========================================================================
 * Map helpers — O(1) hash table backed map
 *
 * Implementation lives in map.c (needs MIR HTAB internals).
 * ========================================================================= */

toad_value_t  toad_val_map(void);
toad_value_t  toad_val_object(void);
int             toad_value_is_object_like(const toad_value_t *value);
toad_value_t  toad_map_get(const toad_value_t *map, const char *key);
int             toad_map_set(toad_value_t *map, const char *key, toad_value_t value);
int             toad_map_has(const toad_value_t *map, const char *key);
int             toad_map_delete(toad_value_t *map, const char *key);
size_t          toad_map_count(const toad_value_t *map);
void            toad_map_free(toad_value_t *map);

/* Returns a pointer to the value inside the hash table (for JIT direct memory access).
 * The pointer is stable as long as no insertions/deletions occur. */
toad_value_t *toad_map_get_ptr(const toad_value_t *map, const char *key);

/* Iteration: call toad_map_iter_begin, then loop while _next returns 1 */
typedef struct {
    void *htab;
    size_t pos;
    size_t bound;
} toad_map_iter_t;

toad_map_iter_t toad_map_iter_begin(const toad_value_t *map);
int               toad_map_iter_next(toad_map_iter_t *it, const char **key, toad_value_t *value);

/* =========================================================================
 * List helpers — heterogeneous array of toad_value_t
 * ========================================================================= */

static inline toad_value_t toad_val_list_empty(void) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_LIST;
    val.ownership = TOAD_VALUE_OWNED;
    val.data.list.items = NULL;
    val.data.list.count = 0;
    val.data.list.capacity = 0;
    val.data.list.heap_owned = 0;
    val.data.list.value_pool = NULL;
    return val;
}

static inline toad_value_t toad_val_set_empty(void) {
    toad_value_t val = toad_val_list_empty();
    val.type = TOAD_VAL_SET;
    return val;
}

static inline toad_value_t toad_val_list_ex(toad_value_t *items, size_t n, int heap_owned) {
    toad_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TOAD_VAL_LIST;
    val.ownership = heap_owned ? TOAD_VALUE_OWNED : TOAD_VALUE_BORROWED;
    val.data.list.items = items;
    val.data.list.count = n;
    val.data.list.capacity = n;
    val.data.list.heap_owned = heap_owned;
    val.data.list.value_pool = NULL;
    return val;
}

static inline toad_value_t toad_val_list(toad_value_t *items, size_t n) {
    return toad_val_list_ex(items, n, 1);
}

int toad_list_push(toad_value_t *list, toad_value_t item);

static inline toad_value_t toad_list_get(const toad_value_t *list, size_t idx) {
    if ((list->type != TOAD_VAL_LIST && list->type != TOAD_VAL_SET) ||
        idx >= list->data.list.count)
        return toad_val_num(0);
    return toad_value_borrow(list->data.list.items[idx]);
}

#endif /* TOAD_MODULE_H */
