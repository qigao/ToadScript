/**
 * @file internal.h
 * @brief Internal shared header for engine modules
 *
 * This header is used by the split source files (tbs_core.c,
 * tbs_math.c, finance.c, tbs_ta.c, registry.c,
 * and tbs_mod_*.c modules) to share internal types, helpers, and
 * declarations.
 */

#ifndef TBS_INTERNAL_H
#define TBS_INTERNAL_H

#include <turboscript/engine.h>
#include <turboscript/class.h>
#include <turboscript/module.h>
#include <turboscript/types.h>
#include <turboscript/simd.h>
#include "memory/buffer.h"


#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Internal registry */
void tbs_registry_init(void);
tbs_builtin_fn tbs_registry_find(const char *name);
tbs_value_t tbs_env_eval_node(const tbs_node_t *node, tbs_env_t *env);
tbs_env_t *tbs_env_snapshot(tbs_env_t *env);
void tbs_env_import_vars(tbs_env_t *dst, tbs_env_t *src);
void tbs_env_set_local_borrowed(tbs_env_t *env, const char *name,
                                   tbs_value_t value);
tbs_value_t throw_error(tbs_env_t *env, const tbs_node_t *node, const char *fmt, ...);
tbs_value_t throw_method_access_error(tbs_env_t *env, const tbs_node_t *node,
                                         const char *method_name,
                                         tbs_func_t *method);
tbs_value_t throw_field_access_error(tbs_env_t *env, const tbs_node_t *node,
                                        const char *field_name, int access_level);
const char *type_name(int type);
double val_to_double(tbs_value_t val);
int values_match(tbs_value_t lhs, tbs_value_t rhs);
tbs_value_t eval_script_function(tbs_func_t *func, size_t argc,
                                    tbs_value_t *args, tbs_env_t *parent_env,
                                    tbs_env_t *caller_env);
tbs_value_t *eval_expand_args(tbs_node_t **nodes, size_t count,
                                 tbs_env_t *env, size_t *out_count);
tbs_class_t *eval_current_class(tbs_env_t *env);
tbs_func_t *eval_find_constructor_typed(tbs_class_t *klass, size_t argc,
                                           tbs_value_t *args);
tbs_value_t eval_class_value_instantiation(tbs_class_t *klass,
                                              const char *class_name,
                                              tbs_node_t **arg_nodes,
                                              size_t arg_count,
                                              tbs_env_t *env);
tbs_value_t eval_class_instantiation(const char *class_name,
                                        tbs_node_t **arg_nodes, size_t arg_count,
                                        tbs_env_t *env);
tbs_value_t eval_instance_script_method(tbs_instance_t *instance,
                                           tbs_func_t *method, size_t argc,
                                           tbs_value_t *args, tbs_env_t *env);
tbs_value_t eval_class_def_node(const tbs_node_t *node, tbs_env_t *env);
int can_access_method(tbs_env_t *env, tbs_func_t *method,
                      const tbs_node_t *object_node);
int can_access_declared_field(tbs_env_t *env, tbs_class_t *owner_class,
                              int access_level,
                              const tbs_node_t *object_node,
                              const char *field_name);

typedef struct {
    const char      *method;
    tbs_value_t   obj;
    tbs_value_t  *args;
    size_t           argc;
    tbs_node_t   *obj_node;
    tbs_env_t    *env;
    mem_pool_t      *arena;
} mc_ctx_t;

tbs_value_t eval_list_method(mc_ctx_t *mc);
tbs_value_t eval_map_method(mc_ctx_t *mc);
tbs_value_t eval_bytes_method(mc_ctx_t *mc);
tbs_value_t eval_uuid_method(mc_ctx_t *mc);
tbs_value_t eval_datetime_method(mc_ctx_t *mc);
int tbs_datetime_member_get(tbs_value_t value, const char *member, tbs_value_t *out);
tbs_value_t eval_date_method(mc_ctx_t *mc);
tbs_value_t eval_time_method(mc_ctx_t *mc);
tbs_value_t eval_duration_method(mc_ctx_t *mc);
tbs_value_t eval_decimal_method(mc_ctx_t *mc);
int tbs_date_member_get(tbs_value_t value, const char *member, tbs_value_t *out);
int tbs_time_member_get(tbs_value_t value, const char *member, tbs_value_t *out);
int tbs_duration_member_get(tbs_value_t value, const char *member, tbs_value_t *out);
int tbs_decimal_member_get(tbs_value_t value, const char *member, tbs_value_t *out);
int tbs_money_member_get(tbs_value_t value, const char *member, tbs_value_t *out);
int tbs_enum_member_get(tbs_value_t value, const char *member, tbs_value_t *out);
tbs_value_t eval_offset_datetime_method(mc_ctx_t *mc);
int tbs_offset_datetime_member_get(tbs_value_t value, const char *member,
                                      tbs_value_t *out);
tbs_value_t eval_typed_array_method(mc_ctx_t *mc);
int tbs_typed_array_member_get(tbs_value_t value, const char *member,
                                  tbs_value_t *out);
tbs_value_t tbs_typed_array_get_value(tbs_value_t value, size_t index);
tbs_value_t eval_string_method(mc_ctx_t *mc);
tbs_value_t eval_vector_method(mc_ctx_t *mc);

/* Built-in module accessors */
const tbs_module_t *tbs_module_math(void);
const tbs_module_t *tbs_module_string(void);
const tbs_module_t *tbs_module_stats(void);
const tbs_module_t *tbs_module_io(void);
const tbs_module_t *tbs_module_core(void);
const tbs_module_t *tbs_module_regex(void);

void tbs_regex_ctx_destroy(void *ctx);

#endif /* TBS_INTERNAL_H */
