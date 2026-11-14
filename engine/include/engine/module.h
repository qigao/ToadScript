/**
 * @file module.h
 * @brief Public module descriptor API for engine.
 *
 * External modules (e.g. fin under turbo_script) include this header
 * to define tbs_module_t instances without depending on any private
 * engine internals.
 */
#ifndef TBS_MODULE_H
#define TBS_MODULE_H

#include <turboscript/types.h>
#include "memory/buffer.h"
#include <cmeta/function.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Module function signature & descriptor types
 * ========================================================================= */

typedef tbs_value_t (*tbs_builtin_fn)(
    size_t argc, tbs_value_t *args,
    tbs_env_t *env, mem_pool_t *scratch);

typedef struct {
    const char *name;
    tbs_builtin_fn fn;
} tbs_func_entry_t;

/*
 * Admission-time CMeta projection of the exact native Engine builtin adapter.
 *
 * The logical language remains dynamically typed. This descriptor therefore
 * reflects the concrete VM adapter contract once, before execution:
 *
 *   tbs_value_t fn(size_t argc,
 *                     tbs_value_t *args,
 *                     tbs_env_t *env,
 *                     mem_pool_t *scratch)
 *
 * Result ownership is intentionally CMETA_RESULT_UNKNOWN at this raw adapter
 * boundary. tbs_value_t is a dynamic carrier whose payload may be borrowed
 * from the per-call scratch pool or may already own retained storage. A raw
 * builtin result must therefore not escape directly. tbs_call_builtin()
 * clones/promotes the result into the target env owner, destroys the raw value,
 * and only then destroys scratch.
 *
 * Consumers may retain the copied metadata only as long as this reflection
 * storage remains alive. Invocation continues through the exact entry->fn
 * pointer. Callers that need an escaping result must preserve the
 * tbs_call_builtin() promotion boundary rather than infer ownership from the
 * by-value C return type.
 */
typedef struct tbs_function_reflection {
    cmeta_param_desc params[4];
    cmeta_function_desc function;
    tbs_builtin_fn invoke;
} tbs_function_reflection_t;

TBS_C_API int tbs_func_entry_reflect(
    const tbs_func_entry_t *entry,
    tbs_function_reflection_t *out);
TBS_C_API int tbs_module_function_reflect(
    const tbs_module_t *module,
    size_t index,
    tbs_function_reflection_t *out);

struct tbs_module_s {
    const char *module_name;
    const tbs_func_entry_t *entries;
    size_t count;
};

/* =========================================================================
 * Value constructors (inline helpers for module implementations)
 * ========================================================================= */

static inline tbs_value_t tbs_val_num(double v) {
    tbs_value_t val = {0};
    val.type = TBS_VAL_NUMBER;
    val.data.number = v;
    return val;
}

static inline tbs_value_t tbs_val_int(int64_t v) {
    tbs_value_t val = {0};
    val.type = TBS_VAL_INTEGER;
    val.data.integer = v;
    return val;
}

static inline tbs_value_t tbs_val_bool(int v) {
    tbs_value_t val = {0};
    val.type = TBS_VAL_BOOL;
    val.data.boolean = v ? 1 : 0;
    return val;
}

static inline tbs_value_t tbs_val_vec(double *data, size_t size) {
    tbs_value_t val = {0};
    val.type = TBS_VAL_VECTOR;
    val.data.vector.data = data;
    val.data.vector.size = size;
    return val;
}

static inline tbs_value_t tbs_val_str(vstr v) {
    tbs_value_t val = {0};
    val.type = TBS_VAL_STRING;
    val.data.string = v;
    return val;
}

static inline tbs_value_t tbs_val_bytes(vstr v) {
    tbs_value_t val = {0};
    val.type = TBS_VAL_BYTES;
    val.data.bytes = v;
    return val;
}

/* Copy the caller's canonical 16-byte UUID; no pointer is retained. */
static inline tbs_value_t tbs_val_uuid(const uuid_t v) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_UUID;
    memcpy(val.data.uuid, v, sizeof(val.data.uuid));
    return val;
}

static inline tbs_value_t tbs_val_datetime(datetime_t v) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_DATETIME;
    val.data.datetime = v;
    return val;
}

static inline tbs_value_t tbs_val_date(tbs_date_t v) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_DATE;
    val.data.date = v;
    return val;
}

static inline tbs_value_t tbs_val_time(tbs_time_t v) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_TIME;
    val.data.time = v;
    return val;
}

static inline tbs_value_t tbs_val_duration(int64_t ms) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_DURATION;
    val.data.duration_ms = ms;
    return val;
}

static inline tbs_value_t tbs_val_decimal(tbs_decimal_t v) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_DECIMAL;
    val.data.decimal = v;
    return val;
}

static inline tbs_value_t tbs_val_bigint(vstr v) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_BIGINT;
    val.data.bigint.text = v;
    return val;
}

static inline tbs_value_t tbs_val_money(tbs_money_t v) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_MONEY;
    val.data.money = v;
    return val;
}

static inline tbs_value_t tbs_val_enum(vstr type_name, vstr symbol, int64_t value, int is_flags) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = is_flags ? TBS_VAL_FLAGS : TBS_VAL_ENUM;
    val.data.enum_val.type_name = type_name;
    val.data.enum_val.symbol = symbol;
    val.data.enum_val.value = value;
    val.data.enum_val.is_flags = is_flags ? 1 : 0;
    return val;
}

static inline tbs_value_t tbs_val_offset_datetime(datetime_t datetime,
                                                        int offset_minutes) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_OFFSET_DATETIME;
    val.data.offset_datetime.datetime = datetime;
    val.data.offset_datetime.offset_minutes = offset_minutes;
    return val;
}

static inline tbs_value_t tbs_val_typed_array(tbs_typed_array_kind_t kind,
                                                    void *data, size_t count,
                                                    int heap_owned) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_TYPED_ARRAY;
    val.data.typed_array.kind = kind;
    val.data.typed_array.data = data;
    val.data.typed_array.count = count;
    val.data.typed_array.heap_owned = heap_owned;
    return val;
}

tbs_value_t tbs_typed_array_get_value(tbs_value_t value, size_t index);

/* =========================================================================
 * Script value ownership
 *
 * Borrowed constructors above are valid only for the immediate call.  Values
 * crossing an env/container/task boundary must use copy/retain/release.  The
 * retain operation is for mem_buffer-backed scalar/vector payloads; containers
 * cross owner boundaries through copy_to_pool/copy_to_env.
 * ========================================================================= */

int tbs_value_copy_to_pool(tbs_value_t value, mem_pool_t *pool,
                              tbs_value_t *out);
/* Captured-scope reference counting used by function values (see engine.h). */
TBS_C_API tbs_env_t *tbs_env_snapshot_names(tbs_env_t *env,
                                              const char *const *names,
                                              size_t name_count);
TBS_C_API char **tbs_collect_closure_free_vars(const tbs_node_t *body,
                                                tbs_node_t *const *arg_params,
                                                size_t arg_count,
                                                mem_pool_t *arena);
TBS_C_API void tbs_env_retain(tbs_env_t *env);
TBS_C_API void tbs_env_release(tbs_env_t *env);
TBS_C_API void tbs_env_sweep_closures(tbs_env_t *root);

int tbs_value_copy_to_env(tbs_value_t value, tbs_env_t *env,
                             tbs_value_t *out);
tbs_value_t tbs_value_retain(tbs_value_t value);
tbs_value_t tbs_value_borrow(tbs_value_t value);
void tbs_value_release_storage(tbs_value_t *value);
void tbs_value_destroy(tbs_value_t *value);
void tbs_values_destroy(tbs_value_t *values, size_t count);

/* =========================================================================
 * Map helpers — O(1) hash table backed map
 *
 * Implementation lives in map.c (needs MIR HTAB internals).
 * ========================================================================= */

tbs_value_t  tbs_val_map(void);
tbs_value_t  tbs_val_object(void);
int             tbs_value_is_object_like(const tbs_value_t *value);
tbs_value_t  tbs_map_get(const tbs_value_t *map, const char *key);
int             tbs_map_set(tbs_value_t *map, const char *key, tbs_value_t value);
int             tbs_map_has(const tbs_value_t *map, const char *key);
int             tbs_map_delete(tbs_value_t *map, const char *key);
size_t          tbs_map_count(const tbs_value_t *map);
void            tbs_map_free(tbs_value_t *map);

/* Returns a pointer to the value inside the hash table (for JIT direct memory access).
 * The pointer is stable as long as no insertions/deletions occur. */
tbs_value_t *tbs_map_get_ptr(const tbs_value_t *map, const char *key);

/* Iteration: call tbs_map_iter_begin, then loop while _next returns 1 */
typedef struct {
    void *htab;
    size_t pos;
    size_t bound;
} tbs_map_iter_t;

tbs_map_iter_t tbs_map_iter_begin(const tbs_value_t *map);
int               tbs_map_iter_next(tbs_map_iter_t *it, const char **key, tbs_value_t *value);

/* =========================================================================
 * List helpers — heterogeneous array of tbs_value_t
 * ========================================================================= */

static inline tbs_value_t tbs_val_list_empty(void) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_LIST;
    val.ownership = TBS_VALUE_OWNED;
    val.data.list.items = NULL;
    val.data.list.count = 0;
    val.data.list.capacity = 0;
    val.data.list.heap_owned = 0;
    val.data.list.value_pool = NULL;
    return val;
}

static inline tbs_value_t tbs_val_set_empty(void) {
    tbs_value_t val = tbs_val_list_empty();
    val.type = TBS_VAL_SET;
    return val;
}

static inline tbs_value_t tbs_val_list_ex(tbs_value_t *items, size_t n, int heap_owned) {
    tbs_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = TBS_VAL_LIST;
    val.ownership = heap_owned ? TBS_VALUE_OWNED : TBS_VALUE_BORROWED;
    val.data.list.items = items;
    val.data.list.count = n;
    val.data.list.capacity = n;
    val.data.list.heap_owned = heap_owned;
    val.data.list.value_pool = NULL;
    return val;
}

static inline tbs_value_t tbs_val_list(tbs_value_t *items, size_t n) {
    return tbs_val_list_ex(items, n, 1);
}

int tbs_list_push(tbs_value_t *list, tbs_value_t item);

static inline tbs_value_t tbs_list_get(const tbs_value_t *list, size_t idx) {
    if ((list->type != TBS_VAL_LIST && list->type != TBS_VAL_SET) ||
        idx >= list->data.list.count)
        return tbs_val_num(0);
    return tbs_value_borrow(list->data.list.items[idx]);
}

#endif /* TBS_MODULE_H */
