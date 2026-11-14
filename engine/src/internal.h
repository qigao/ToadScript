/**
 * @file internal.h
 * @brief Internal shared header for engine modules
 *
 * This header is used by the split source files (toad_core.c,
 * toad_math.c, finance.c, toad_ta.c, registry.c,
 * and toad_mod_*.c modules) to share internal types, helpers, and
 * declarations.
 */

#ifndef TOAD_INTERNAL_H
#define TOAD_INTERNAL_H

#include <toad/engine.h>
#include <toad/class.h>
#include <toad/module.h>
#include <toad/types.h>
#include <toad/simd.h>
#include "memory/buffer.h"


#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Internal registry */
void toad_registry_init(void);
toad_builtin_fn toad_registry_find(const char *name);
toad_value_t toad_env_eval_node(const toad_node_t *node, toad_env_t *env);
toad_env_t *toad_env_snapshot(toad_env_t *env);
void toad_env_import_vars(toad_env_t *dst, toad_env_t *src);
void toad_env_set_local_borrowed(toad_env_t *env, const char *name,
                                   toad_value_t value);
toad_value_t throw_error(toad_env_t *env, const toad_node_t *node, const char *fmt, ...);
toad_value_t throw_method_access_error(toad_env_t *env, const toad_node_t *node,
                                         const char *method_name,
                                         toad_func_t *method);
toad_value_t throw_field_access_error(toad_env_t *env, const toad_node_t *node,
                                        const char *field_name, int access_level);
const char *type_name(int type);
double val_to_double(toad_value_t val);
int values_match(toad_value_t lhs, toad_value_t rhs);
toad_value_t eval_script_function(toad_func_t *func, size_t argc,
                                    toad_value_t *args, toad_env_t *parent_env,
                                    toad_env_t *caller_env);
toad_value_t *eval_expand_args(toad_node_t **nodes, size_t count,
                                 toad_env_t *env, size_t *out_count);
toad_class_t *eval_current_class(toad_env_t *env);
toad_func_t *eval_find_constructor_typed(toad_class_t *klass, size_t argc,
                                           toad_value_t *args);
toad_value_t eval_class_value_instantiation(toad_class_t *klass,
                                              const char *class_name,
                                              toad_node_t **arg_nodes,
                                              size_t arg_count,
                                              toad_env_t *env);
toad_value_t eval_class_instantiation(const char *class_name,
                                        toad_node_t **arg_nodes, size_t arg_count,
                                        toad_env_t *env);
toad_value_t eval_instance_script_method(toad_instance_t *instance,
                                           toad_func_t *method, size_t argc,
                                           toad_value_t *args, toad_env_t *env);
toad_value_t eval_class_def_node(const toad_node_t *node, toad_env_t *env);
int can_access_method(toad_env_t *env, toad_func_t *method,
                      const toad_node_t *object_node);
int can_access_declared_field(toad_env_t *env, toad_class_t *owner_class,
                              int access_level,
                              const toad_node_t *object_node,
                              const char *field_name);

typedef struct {
    const char      *method;
    toad_value_t   obj;
    toad_value_t  *args;
    size_t           argc;
    toad_node_t   *obj_node;
    toad_env_t    *env;
    mem_pool_t      *arena;
} mc_ctx_t;

toad_value_t eval_list_method(mc_ctx_t *mc);
toad_value_t eval_map_method(mc_ctx_t *mc);
toad_value_t eval_bytes_method(mc_ctx_t *mc);
toad_value_t eval_uuid_method(mc_ctx_t *mc);
toad_value_t eval_datetime_method(mc_ctx_t *mc);
int toad_datetime_member_get(toad_value_t value, const char *member, toad_value_t *out);
toad_value_t eval_date_method(mc_ctx_t *mc);
toad_value_t eval_time_method(mc_ctx_t *mc);
toad_value_t eval_duration_method(mc_ctx_t *mc);
toad_value_t eval_decimal_method(mc_ctx_t *mc);
int toad_date_member_get(toad_value_t value, const char *member, toad_value_t *out);
int toad_time_member_get(toad_value_t value, const char *member, toad_value_t *out);
int toad_duration_member_get(toad_value_t value, const char *member, toad_value_t *out);
int toad_decimal_member_get(toad_value_t value, const char *member, toad_value_t *out);
int toad_money_member_get(toad_value_t value, const char *member, toad_value_t *out);
int toad_enum_member_get(toad_value_t value, const char *member, toad_value_t *out);
toad_value_t eval_offset_datetime_method(mc_ctx_t *mc);
int toad_offset_datetime_member_get(toad_value_t value, const char *member,
                                      toad_value_t *out);
toad_value_t eval_typed_array_method(mc_ctx_t *mc);
int toad_typed_array_member_get(toad_value_t value, const char *member,
                                  toad_value_t *out);
toad_value_t toad_typed_array_get_value(toad_value_t value, size_t index);
toad_value_t eval_string_method(mc_ctx_t *mc);
toad_value_t eval_vector_method(mc_ctx_t *mc);

/* Built-in module accessors */
const toad_module_t *toad_module_math(void);
const toad_module_t *toad_module_string(void);
const toad_module_t *toad_module_stats(void);
const toad_module_t *toad_module_io(void);
const toad_module_t *toad_module_core(void);
const toad_module_t *toad_module_regex(void);

void toad_regex_ctx_destroy(void *ctx);

#endif /* TOAD_INTERNAL_H */
