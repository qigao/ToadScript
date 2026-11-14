/**
 * @file oop.c
 * @brief OOP System for MIR Backend
 *
 * This module handles Object-Oriented Programming features in the Toad MIR backend.
 * Extracted from mir.c as part of modularization effort.
 */

#include "internal.h"
#include <toad/grammar.h>
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * OOP helpers
 * ========================================================================= */

/**
 * @brief Get predicate kind from function name
 * @param name Function name to check
 * @return 1=is_class, 2=is_instance, 3=is_function, 4=is_null, 0=not a predicate
 */
int ts_oop_predicate_kind(const char *name) {
  if (!name) return 0;
  if (strcmp(name, "is_class") == 0) return 1;
  if (strcmp(name, "is_instance") == 0) return 2;
  if (strcmp(name, "is_function") == 0) return 3;
  if (strcmp(name, "is_null") == 0) return 4;
  return 0;
}

/**
 * @brief Check if predicate can preserve value (avoid runtime bridge)
 * @param kind Predicate kind (from ts_oop_predicate_kind)
 * @param arg Argument node
 * @return 1 if can preserve value, 0 otherwise
 *
 * is_null with null literal or variable can be optimized.
 * Other predicates can always preserve value.
 */
int ts_oop_predicate_can_preserve_value(int kind, toad_node_t *arg) {
  if (!arg) return 0;
  if (kind != 4) return 1;  // Non-null predicates can preserve value
  return arg->type == TOAD_NODE_NULL || arg->type == TOAD_NODE_VARIABLE ||
         arg->type == TOAD_NODE_MEMBER_ACCESS;
}

/**
 * @brief Get OOP kind from typeof string node
 * @param node String literal node containing type name
 * @return 1=class, 2=instance, 3=function, 0=not OOP type
 */
int ts_typeof_oop_kind(toad_node_t *node) {
  if (!node || node->type != TOAD_NODE_STRING || !node->data.string.value.data) return 0;

  vstr value = node->data.string.value;
  if (value.len == 5 && memcmp(value.data, "class", 5) == 0) return 1;
  if (value.len == 8 && memcmp(value.data, "instance", 8) == 0) return 2;
  if (value.len == 8 && memcmp(value.data, "function", 8) == 0) return 3;
  return 0;
}

/**
 * @brief Extract argument from typeof() call
 * @param node Function call node to check
 * @param out_arg Output parameter for argument node
 * @return 1 if node is typeof(arg), 0 otherwise
 */
int ts_typeof_arg(toad_node_t *node, toad_node_t **out_arg) {
  if (!node || node->type != TOAD_NODE_FUNCTION_CALL || !node->data.function.name ||
      strcmp(node->data.function.name, "typeof") != 0 || node->data.function.arg_count != 1) {
    return 0;
  }

  if (out_arg) *out_arg = node->data.function.args[0];
  return 1;
}

/* =========================================================================
 * Method Reload Analysis
 * ========================================================================= */

static int ts_name_is_param(const char *name, toad_node_t **params, size_t param_count) {
  if (!name) return 0;
  for (size_t i = 0; i < param_count; ++i) {
    if (params[i] && params[i]->type == TOAD_NODE_VARIABLE &&
        params[i]->data.variable.name && strcmp(params[i]->data.variable.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ts_oop_method_expr_is_read_only(toad_node_t *node);

static int ts_oop_method_body_needs_reload(toad_node_t *node, toad_node_t **params,
                                           size_t param_count) {
  if (!node) return 0;

  switch (node->type) {
  case TOAD_NODE_NUMBER:
  case TOAD_NODE_INTEGER:
  case TOAD_NODE_VARIABLE:
  case TOAD_NODE_STRING:
  case TOAD_NODE_NULL:
  case TOAD_NODE_THIS:
    return 0;

  case TOAD_NODE_BINARY_OP:
    return ts_oop_method_body_needs_reload(node->data.binary.left, params, param_count) ||
           ts_oop_method_body_needs_reload(node->data.binary.right, params, param_count);

  case TOAD_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_oop_method_body_needs_reload(node->data.block.statements[i], params, param_count)) {
        return 1;
      }
    }
    return 0;

  case TOAD_NODE_FLOW:
    return ts_oop_method_expr_is_read_only(node->data.flow.value) ? 0 : 1;

  case TOAD_NODE_ASSIGNMENT:
    if (!ts_name_is_param(node->data.assignment.name, params, param_count)) return 1;
    return ts_oop_method_expr_is_read_only(node->data.assignment.value) ? 0 : 1;

  case TOAD_NODE_MEMBER_ACCESS:
    return ts_oop_method_expr_is_read_only(node->data.member_access.object) ? 0 : 1;

  case TOAD_NODE_MEMBER_SET:
    if (!node->data.member_set.object ||
        node->data.member_set.object->type != TOAD_NODE_THIS) {
      return 1;
    }
    return ts_oop_method_expr_is_read_only(node->data.member_set.value) ? 0 : 1;

  default:
    return 1;
  }
}

static int ts_oop_method_expr_is_read_only(toad_node_t *node) {
  return !ts_oop_method_body_needs_reload(node, NULL, 0);
}

int ts_oop_member_call_can_skip_reload(ts_mir_compiler_t *c, const char *class_name,
                                       const char *method_name, size_t argc) {
  toad_value_t class_value;
  toad_value_t args[16];
  toad_func_t *method;

  if (!c || !class_name || !method_name || argc > 16) return 0;
  class_value = toad_env_get(&c->ts_ctx->env, class_name);
  if (class_value.type != TOAD_VAL_CLASS || !class_value.data.class_val.klass) return 0;

  for (size_t i = 0; i < argc; ++i) {
    args[i] = toad_val_num(0.0);
  }
  method = toad_class_lookup_method_typed(class_value.data.class_val.klass, method_name, 0,
                                            argc, argc ? args : NULL);
  if (!method || !method->is_script) return 0;
  return !ts_oop_method_body_needs_reload(method->data.script.body,
                                          method->data.script.arg_params,
                                          method->data.script.arg_count);
}

/* =========================================================================
 * Class AST Analysis
 * ========================================================================= */

toad_node_t *ts_find_top_level_class_def(ts_mir_compiler_t *c, const char *class_name) {
  if (!c || !c->ast_root || !class_name || c->ast_root->type != TOAD_NODE_BLOCK) return NULL;

  for (size_t i = 0; i < c->ast_root->data.block.count; ++i) {
    toad_node_t *stmt = c->ast_root->data.block.statements[i];
    if (stmt && stmt->type == TOAD_NODE_CLASS_DEF && stmt->data.class_def.name &&
        strcmp(stmt->data.class_def.name, class_name) == 0) {
      return stmt;
    }
  }
  return NULL;
}

int ts_top_level_class_name_is_assigned(ts_mir_compiler_t *c, const char *class_name) {
  if (!c || !c->ast_root || !class_name || c->ast_root->type != TOAD_NODE_BLOCK) return 0;

  for (size_t i = 0; i < c->ast_root->data.block.count; ++i) {
    toad_node_t *stmt = c->ast_root->data.block.statements[i];
    if (!stmt) continue;
    if ((stmt->type == TOAD_NODE_ASSIGNMENT || stmt->type == TOAD_NODE_CONSTANT_DECL) &&
        stmt->data.assignment.name && strcmp(stmt->data.assignment.name, class_name) == 0) {
      return 1;
    }
  }
  return 0;
}

int ts_inline_param_index(toad_node_t **params, size_t param_count, const char *name,
                          size_t *out_index) {
  if (!name) return 0;
  for (size_t i = 0; i < param_count; ++i) {
    if (params[i] && params[i]->type == TOAD_NODE_VARIABLE &&
        params[i]->data.variable.name && strcmp(params[i]->data.variable.name, name) == 0) {
      if (out_index) *out_index = i;
      return 1;
    }
  }
  return 0;
}

const char *ts_direct_parent_class_name(ts_mir_compiler_t *c, const char *class_name) {
  toad_node_t *class_node = ts_find_top_level_class_def(c, class_name);
  if (!class_node || !class_node->data.class_def.parent_name) return NULL;
  if (ts_top_level_class_name_is_assigned(c, class_node->data.class_def.parent_name)) return NULL;
  return class_node->data.class_def.parent_name;
}

static int ts_node_mentions_this_field(toad_node_t *node, const char *member_name) {
  if (!node || !member_name) return 0;

  switch (node->type) {
  case TOAD_NODE_MEMBER_ACCESS:
    if (node->data.member_access.object &&
        node->data.member_access.object->type == TOAD_NODE_THIS &&
        node->data.member_access.member &&
        strcmp(node->data.member_access.member, member_name) == 0) {
      return 1;
    }
    return ts_node_mentions_this_field(node->data.member_access.object, member_name);

  case TOAD_NODE_MEMBER_SET:
    if (node->data.member_set.object &&
        node->data.member_set.object->type == TOAD_NODE_THIS &&
        node->data.member_set.member &&
        strcmp(node->data.member_set.member, member_name) == 0) {
      return 1;
    }
    return ts_node_mentions_this_field(node->data.member_set.object, member_name) ||
           ts_node_mentions_this_field(node->data.member_set.value, member_name);

  case TOAD_NODE_BINARY_OP:
    return ts_node_mentions_this_field(node->data.binary.left, member_name) ||
           ts_node_mentions_this_field(node->data.binary.right, member_name);

  case TOAD_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_mentions_this_field(node->data.block.statements[i], member_name)) return 1;
    }
    return 0;

  case TOAD_NODE_FLOW:
    return ts_node_mentions_this_field(node->data.flow.value, member_name);

  case TOAD_NODE_IF:
    return ts_node_mentions_this_field(node->data.if_stmt.condition, member_name) ||
           ts_node_mentions_this_field(node->data.if_stmt.if_branch, member_name) ||
           ts_node_mentions_this_field(node->data.if_stmt.else_branch, member_name);

  case TOAD_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_mentions_this_field(node->data.function.args[i], member_name)) return 1;
    }
    return 0;

  case TOAD_NODE_MEMBER_CALL:
    if (ts_node_mentions_this_field(node->data.member_call.object, member_name)) return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_mentions_this_field(node->data.member_call.args[i], member_name)) return 1;
    }
    return 0;

  default:
    return 0;
  }
}

static int ts_oop_cmeta_num_field_admission(
    ts_mir_compiler_t *c, const char *class_name, const char *member_name) {
  toad_value_t class_value;
  toad_class_t *klass;
  toad_class_t *owner = NULL;
  const cmeta_data_desc *data;
  const cmeta_data_struct_shape *shape;
  const cmeta_data_field_desc *field;
  int access;

  if (!c || !class_name || !member_name) return 0;
  class_value = toad_env_get(&c->ts_ctx->env, class_name);
  if (class_value.type != TOAD_VAL_CLASS ||
      !class_value.data.class_val.klass)
    return 0; /* same-source class may not exist until generated MIR executes */

  klass = class_value.data.class_val.klass;
  access = toad_class_get_instance_field_access(
      klass, member_name, &owner);
  if (access != TOAD_ACCESS_PUBLIC)
    return -1;
  if (!toad_class_finalize_cmeta_data(klass))
    return -1;

  data = toad_class_cmeta_data(klass);
  if (!cmeta_data_desc_valid(data) ||
      data->kind != CMETA_DATA_STRUCT || data->shape == NULL)
    return -1;
  shape = (const cmeta_data_struct_shape *)data->shape;
  field = cmeta_data_struct_find_field(shape, member_name);
  if (!field || !cmeta_data_desc_valid(field->value))
    return -1;

  return cmeta_data_desc_equal(field->value, &cmeta_data_double) ? 1 : -1;
}

static int ts_class_has_public_instance_field_hint(ts_mir_compiler_t *c,
                                                   const char *class_name,
                                                   const char *member_name) {
  toad_node_t *class_node = ts_find_top_level_class_def(c, class_name);
  if (!class_node || !member_name) return 0;

  for (size_t i = 0; i < class_node->data.class_def.method_count; ++i) {
    toad_node_t *entry = class_node->data.class_def.methods[i];
    if (!entry) continue;
    if (entry->type == TOAD_NODE_FIELD_DECL && entry->data.field_decl.name &&
        strcmp(entry->data.field_decl.name, member_name) == 0 &&
        !entry->data.field_decl.is_static) {
      /* Only an explicitly numeric-double declaration may propose the slot
       * fast path. Runtime cache fill still requires canonical CMeta
       * cmeta_data_double admission before direct slot access. */
      if (entry->data.field_decl.declared_type) {
        const char *type = entry->data.field_decl.declared_type;
        const int is_double =
            strcmp(type, "number") == 0 ||
            strcmp(type, "float") == 0 ||
            strcmp(type, "double") == 0;
        return is_double &&
                       entry->data.field_decl.access_level == TOAD_ACCESS_PUBLIC
                   ? 1
                   : -1;
      }
      /* Untyped fields are dynamic CUSTOM CMeta values. They must never be
       * frozen into a direct double slot merely because their current value is
       * numeric. */
      return -1;
    }
  }

  const char *parent_name = ts_direct_parent_class_name(c, class_name);
  if (parent_name) return ts_class_has_public_instance_field_hint(c, parent_name, member_name);
  return 0;
}

int ts_oop_member_can_use_num_slot(ts_mir_compiler_t *c, const char *object_name,
                                   const char *member_name) {
  const char *class_name = ts_mir_find_var_class(c, object_name);
  int cmeta_admission;
  if (!class_name || !member_name) return 0;
  if (member_name[0] == '_') return 0;

  /* Prefer already-finalized runtime reflection when the class exists in the
   * compile environment. For same-source classes, class definition executes
   * later, so the AST can only propose a typed-number candidate; runtime cache
   * fill performs the mandatory CMeta admission before exposing a slot. */
  cmeta_admission =
      ts_oop_cmeta_num_field_admission(c, class_name, member_name);
  if (cmeta_admission != 0) return cmeta_admission > 0;
  return ts_class_has_public_instance_field_hint(
             c, class_name, member_name) > 0;
}

static int ts_oop_inline_expr_supported(ts_mir_compiler_t *c, const char *class_name,
                                        toad_node_t *node, toad_node_t **params,
                                        size_t param_count) {
  if (!node) return 1;

  switch (node->type) {
  case TOAD_NODE_NUMBER:
  case TOAD_NODE_INTEGER:
  case TOAD_NODE_NULL:
    return 1;

  case TOAD_NODE_VARIABLE:
    return ts_inline_param_index(params, param_count, node->data.variable.name, NULL);

  case TOAD_NODE_MEMBER_ACCESS:
    return node->data.member_access.object &&
           node->data.member_access.object->type == TOAD_NODE_THIS &&
           node->data.member_access.member != NULL;

  case TOAD_NODE_SUPER: {
    const char *parent_name;
    if (!node->data.super_expr.is_call || !node->data.super_expr.member) return 0;
    parent_name = ts_direct_parent_class_name(c, class_name);
    if (!parent_name) return 0;
    for (size_t i = 0; i < node->data.super_expr.arg_count; ++i) {
      if (!ts_oop_inline_expr_supported(c, class_name, node->data.super_expr.args[i],
                                        params, param_count)) {
        return 0;
      }
    }
    return ts_find_inlineable_instance_method(c, parent_name, node->data.super_expr.member,
                                              node->data.super_expr.arg_count) != NULL;
  }

  case TOAD_NODE_BINARY_OP:
    if (node->data.binary.left == NULL) {
      return node->data.binary.op == toad_TOKEN_PLUS ||
             node->data.binary.op == toad_TOKEN_MINUS;
    }
    switch (node->data.binary.op) {
    case toad_TOKEN_PLUS:
    case toad_TOKEN_MINUS:
    case toad_TOKEN_MULTIPLY:
    case toad_TOKEN_DIVIDE:
    case toad_TOKEN_LT:
    case toad_TOKEN_GT:
    case toad_TOKEN_LE:
    case toad_TOKEN_GE:
    case toad_TOKEN_EQ:
    case toad_TOKEN_NE:
      return ts_oop_inline_expr_supported(c, class_name, node->data.binary.left, params,
                                          param_count) &&
             ts_oop_inline_expr_supported(c, class_name, node->data.binary.right, params,
                                          param_count);
    default:
      return 0;
    }

  default:
    return 0;
  }
}

static int ts_oop_inline_stmt_supported(ts_mir_compiler_t *c, const char *class_name,
                                        toad_node_t *node, toad_node_t **params,
                                        size_t param_count) {
  if (!node) return 1;

  switch (node->type) {
  case TOAD_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (!ts_oop_inline_stmt_supported(c, class_name, node->data.block.statements[i],
                                        params, param_count)) {
        return 0;
      }
    }
    return 1;

  case TOAD_NODE_FLOW:
    return node->data.flow.type == toad_TOKEN_RETURN &&
           ts_oop_inline_expr_supported(c, class_name, node->data.flow.value, params,
                                        param_count);

  case TOAD_NODE_MEMBER_SET:
    return node->data.member_set.object &&
           node->data.member_set.object->type == TOAD_NODE_THIS &&
           node->data.member_set.member &&
           ts_oop_inline_expr_supported(c, class_name, node->data.member_set.value, params,
                                        param_count);

  default:
    return ts_oop_inline_expr_supported(c, class_name, node, params, param_count);
  }
}

toad_node_t *ts_find_inlineable_instance_method(ts_mir_compiler_t *c,
                                                  const char *class_name,
                                                  const char *method_name,
                                                  size_t argc) {
  toad_node_t *class_node = ts_find_top_level_class_def(c, class_name);
  toad_node_t *found = NULL;

  if (!class_node || !method_name) return NULL;
  if (method_name[0] == '_') return NULL;
  if (ts_top_level_class_name_is_assigned(c, class_name)) return NULL;
  if (class_node->data.class_def.interface_count > 0 || class_node->data.class_def.is_abstract ||
      class_node->data.class_def.is_interface) {
    return NULL;
  }

  for (size_t i = 0; i < class_node->data.class_def.method_count; ++i) {
    toad_node_t *method = class_node->data.class_def.methods[i];
    if (method && method->type == TOAD_NODE_FIELD_DECL &&
        method->data.field_decl.access_level != TOAD_ACCESS_PUBLIC) {
      return NULL;
    }
    if (!method || method->type != TOAD_NODE_METHOD || !method->data.method.name) continue;
    if (strcmp(method->data.method.name, method_name) != 0) continue;
    if (method->data.method.is_static || method->data.method.is_abstract) continue;
    if (method->data.method.access_level != TOAD_ACCESS_PUBLIC) continue;
    if (method->data.method.arg_count != argc) continue;
    if (found) return NULL;
    found = method;
  }

  if (!found) return NULL;
  for (size_t i = 0; i < found->data.method.arg_count; ++i) {
    if (!found->data.method.arg_params[i] ||
        found->data.method.arg_params[i]->type != TOAD_NODE_VARIABLE) {
      return NULL;
    }
  }
  return ts_oop_inline_stmt_supported(c, class_name, found->data.method.body,
                                      found->data.method.arg_params,
                                      found->data.method.arg_count)
             ? found
             : NULL;
}

/* =========================================================================
 * Runtime Bridges
 * ========================================================================= */

/* =========================================================================
 * MIR OOP emission
 * ========================================================================= */

void ts_emit_oop_define_class(ts_mir_compiler_t *c, toad_node_t *node) {
  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 4,
                                    MIR_new_ref_op(c->ctx, c->ext.oop_define_class_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.oop_define_class_import),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
}

MIR_reg_t ts_emit_oop_class_alias(ts_mir_compiler_t *c, const char *target_name,
                                  const char *source_name) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.oop_alias_class_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_alias_class_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)source_name)));
  ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_oop_new_assign(ts_mir_compiler_t *c, const char *target_name,
                                 toad_node_t *new_node) {
  size_t argc = new_node->data.new_expr.arg_count;
  MIR_reg_t arg_regs[16];
  argc = ts_compile_call_args(c, argc, new_node->data.new_expr.args, arg_regs);
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)new_node->data.new_expr.class_name),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_oop_class_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                        toad_node_t *call_node) {
  size_t argc = call_node->data.function.arg_count;
  MIR_reg_t arg_regs[16];
  argc = ts_compile_call_args(c, argc, call_node->data.function.args, arg_regs);
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node->data.function.name),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  return res;
}

int ts_oop_arg_needs_value_bridge(ts_mir_compiler_t *c, toad_node_t *arg) {
  if (!arg) return 0;

  switch (arg->type) {
  case TOAD_NODE_NULL:
  case TOAD_NODE_STRING:
  case TOAD_NODE_VECTOR:
  case TOAD_NODE_MAP_LITERAL:
  case TOAD_NODE_SLICE:
  case TOAD_NODE_TEMPLATE_STRING:
  case TOAD_NODE_NEW:
  case TOAD_NODE_THIS:
  case TOAD_NODE_SUPER:
  case TOAD_NODE_FUNCTION_EXPRESSION:
  case TOAD_NODE_CLASS_DEF:
    return 1;

  case TOAD_NODE_VARIABLE:
    return ts_mir_find_var_class(c, arg->data.variable.name) ||
           ts_mir_is_known_class_name(c, arg->data.variable.name);

  case TOAD_NODE_FUNCTION_CALL:
    if (arg->data.function.name) {
      const char *name = arg->data.function.name;
      const char *alias_name = ts_find_func_alias(c, name);
      if (alias_name) name = alias_name;
      return ts_function_call_is_known_class(c, name) ||
             ts_call_assignment_needs_value_bridge(c, name, arg->data.function.arg_count);
    }
    return 0;

  case TOAD_NODE_MEMBER_ACCESS:
  case TOAD_NODE_MEMBER_CALL:
    return 1;

  default:
    return 0;
  }
}

int ts_oop_call_args_need_value_bridge(ts_mir_compiler_t *c, size_t argc,
                                       toad_node_t **args) {
  if (!c || !args) return 0;
  for (size_t i = 0; i < argc; ++i) {
    if (ts_oop_arg_needs_value_bridge(c, args[i])) return 1;
  }
  return 0;
}

const char *ts_emit_oop_receiver_to_temp(ts_mir_compiler_t *c, toad_node_t *object_node) {
  const char *target_name = NULL;
  if (!c || !object_node) return NULL;

  if (object_node->type == TOAD_NODE_VARIABLE) {
    return object_node->data.variable.name;
  }

  target_name = ts_mir_new_hidden_receiver_name(c);
  if (!target_name) return NULL;

  if (object_node->type == TOAD_NODE_NEW && object_node->data.new_expr.class_name) {
    if (ts_oop_call_args_need_value_bridge(c, object_node->data.new_expr.arg_count,
                                           object_node->data.new_expr.args)) {
      ts_emit_value_expr_assign(c, target_name, object_node);
    } else {
      ts_emit_oop_new_assign(c, target_name, object_node);
    }
    ts_mir_set_var_class(c, target_name, object_node->data.new_expr.class_name);
    return target_name;
  }

  if (object_node->type == TOAD_NODE_FUNCTION_CALL && object_node->data.function.name) {
    const char *name = object_node->data.function.name;
    if (ts_mir_is_known_class_name(c, name)) {
      if (ts_oop_call_args_need_value_bridge(c, object_node->data.function.arg_count,
                                             object_node->data.function.args)) {
        ts_emit_value_expr_assign(c, target_name, object_node);
      } else {
        ts_emit_oop_class_call_assign(c, target_name, object_node);
      }
      ts_mir_set_var_class(c, target_name, name);
      return target_name;
    }

    if (ts_call_assignment_needs_value_bridge(c, name, object_node->data.function.arg_count)) {
      ts_emit_runtime_call_assign(c, target_name, name, object_node->data.function.arg_count,
                                  object_node->data.function.args);
      return target_name;
    }
  }

  if (object_node->type == TOAD_NODE_MEMBER_CALL && object_node->data.member_call.object &&
      object_node->data.member_call.method) {
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, object_node->data.member_call.object);
    if (!obj_name) return NULL;

    if (ts_oop_call_args_need_value_bridge(c, object_node->data.member_call.arg_count,
                                           object_node->data.member_call.args)) {
      ts_emit_oop_member_call_assign_value(c, target_name, obj_name,
                                           object_node->data.member_call.method,
                                           object_node->data.member_call.object, object_node);
      return target_name;
    }

    MIR_reg_t arg_regs[16];
    size_t argc =
        ts_compile_call_args(c, object_node->data.member_call.arg_count,
                             object_node->data.member_call.args, arg_regs);
    ts_emit_oop_member_call_assign(c, target_name, obj_name, object_node->data.member_call.method,
                                   object_node->data.member_call.object, argc, arg_regs);
    return target_name;
  }

  if (object_node->type == TOAD_NODE_MEMBER_ACCESS && object_node->data.member_access.object &&
      object_node->data.member_access.member) {
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, object_node->data.member_access.object);
    if (!obj_name) return NULL;

    ts_emit_member_access_assign(c, target_name, obj_name, object_node->data.member_access.member,
                                 object_node->data.member_access.object);
    return target_name;
  }

  ts_emit_unsupported_node(c, object_node);
  return NULL;
}

MIR_reg_t ts_emit_oop_predicate(ts_mir_compiler_t *c, int kind, toad_node_t *arg) {
  const char *name = NULL;
  const char *member = NULL;
  toad_node_t *object_node = NULL;
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  if (!c || !arg || kind == 0) return ts_emit_unsupported_node(c, arg);

  if (kind == 4 && arg->type == TOAD_NODE_NULL) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 1.0)));
    return res;
  }

  if (arg->type == TOAD_NODE_VARIABLE) {
    name = arg->data.variable.name;
    object_node = arg;
  } else if (arg->type == TOAD_NODE_MEMBER_ACCESS && arg->data.member_access.object &&
             arg->data.member_access.member) {
    name = ts_emit_oop_receiver_to_temp(c, arg->data.member_access.object);
    member = arg->data.member_access.member;
    object_node = arg->data.member_access.object;
  } else {
    name = ts_emit_oop_receiver_to_temp(c, arg);
    object_node = arg;
  }

  if (!name) return ts_emit_unsupported_node(c, arg);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_predicate_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_predicate_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_int_op(c->ctx, (int64_t)kind),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  ts_emit_reload_from_env(c);
  return res;
}

int ts_try_emit_typeof_oop_compare(ts_mir_compiler_t *c, toad_node_t *left,
                                   toad_node_t *right, int op, MIR_reg_t *out) {
  toad_node_t *arg = NULL;
  int kind = 0;

  if (!c || !left || !right || !out ||
      (op != toad_TOKEN_EQ && op != toad_TOKEN_NE)) {
    return 0;
  }

  if (ts_typeof_arg(left, &arg)) {
    kind = ts_typeof_oop_kind(right);
  } else if (ts_typeof_arg(right, &arg)) {
    kind = ts_typeof_oop_kind(left);
  }
  if (!kind || !arg) return 0;

  MIR_reg_t pred = ts_emit_oop_predicate(c, kind, arg);
  if (op == toad_TOKEN_EQ) {
    *out = pred;
    return 1;
  }

  MIR_reg_t ireg = ts_mir_new_temp_ireg(c);
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DEQ, MIR_new_reg_op(c->ctx, ireg),
                               MIR_new_reg_op(c->ctx, pred), MIR_new_double_op(c->ctx, 0.0)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                               MIR_new_reg_op(c->ctx, ireg)));
  *out = res;
  return 1;
}

static MIR_reg_t ts_emit_oop_member_call(ts_mir_compiler_t *c, const char *object_name,
                                         const char *method_name, toad_node_t *object_node,
                                         size_t argc, const MIR_reg_t *arg_regs) {
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_oop_member_call_value(ts_mir_compiler_t *c, const char *object_name,
                                        const char *method_name,
                                        toad_node_t *object_node,
                                        toad_node_t *call_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_value_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_value_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_oop_member_call_mono_value(ts_mir_compiler_t *c,
                                             const char *object_name,
                                             const char *expected_class_name,
                                             const char *method_name,
                                             toad_node_t *object_node,
                                             toad_node_t *call_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9,
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_mono_value_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_mono_value_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)expected_class_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_oop_member_call_cached(ts_mir_compiler_t *c, const char *object_name,
                                         const char *method_name, toad_node_t *object_node,
                                         size_t argc, const MIR_reg_t *arg_regs,
                                         int reload_after) {
  toad_oop_method_cache_t *cache =
      (toad_oop_method_cache_t *)calloc(1, sizeof(toad_oop_method_cache_t));
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  if (!reload_after) ts_mir_clear_dirty_flags(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 10, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_cached_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_cached_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  if (reload_after) ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_oop_member_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                         const char *object_name, const char *method_name,
                                         toad_node_t *object_node, size_t argc,
                                         const MIR_reg_t *arg_regs) {
  toad_oop_method_cache_t *cache =
      (toad_oop_method_cache_t *)calloc(1, sizeof(toad_oop_method_cache_t));
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 11, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  return res;
}

MIR_reg_t ts_emit_oop_member_call_assign_value(ts_mir_compiler_t *c,
                                               const char *target_name,
                                               const char *object_name,
                                               const char *method_name,
                                               toad_node_t *object_node,
                                               toad_node_t *call_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9,
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_value_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_value_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  return res;
}

static MIR_reg_t ts_try_emit_oop_member_set_slot(ts_mir_compiler_t *c, const char *object_name,
                                                 const char *member_name,
                                                 toad_node_t *object_node,
                                                 MIR_reg_t value_reg);

static MIR_reg_t ts_emit_oop_member_get(ts_mir_compiler_t *c, const char *object_name,
                                        const char *member_name, toad_node_t *object_node) {
  MIR_reg_t slot_value = ts_try_emit_oop_member_get_slot(c, object_name, member_name, object_node);
  if (slot_value) return slot_value;

  MIR_reg_t res = ts_mir_new_temp_reg(c);
  toad_oop_field_cache_t *cache =
      (toad_oop_field_cache_t *)calloc(1, sizeof(toad_oop_field_cache_t));
  ts_emit_sync_var_to_env(c, object_name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_member_get_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_get_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  return res;
}

MIR_reg_t ts_emit_oop_member_set(ts_mir_compiler_t *c, const char *object_name,
                                 const char *member_name, toad_node_t *object_node,
                                 toad_node_t *value_node) {
  MIR_reg_t value_reg = ts_compile_expr(c, value_node);
  MIR_reg_t slot_result =
      ts_try_emit_oop_member_set_slot(c, object_name, member_name, object_node, value_reg);
  if (slot_result) return slot_result;

  MIR_reg_t res = ts_mir_new_temp_reg(c);
  toad_oop_field_cache_t *cache =
      (toad_oop_field_cache_t *)calloc(1, sizeof(toad_oop_field_cache_t));

  ts_emit_sync_var_to_env(c, object_name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.oop_member_set_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_set_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_reg_op(c->ctx, value_reg)));
  return res;
}

static MIR_reg_t ts_emit_oop_member_set_reg(ts_mir_compiler_t *c, const char *object_name,
                                            const char *member_name, toad_node_t *object_node,
                                            MIR_reg_t value_reg) {
  MIR_reg_t slot_result =
      ts_try_emit_oop_member_set_slot(c, object_name, member_name, object_node, value_reg);
  if (slot_result) return slot_result;

  MIR_reg_t res = ts_mir_new_temp_reg(c);
  toad_oop_field_cache_t *cache =
      (toad_oop_field_cache_t *)calloc(1, sizeof(toad_oop_field_cache_t));

  ts_emit_sync_var_to_env(c, object_name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.oop_member_set_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_set_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_reg_op(c->ctx, value_reg)));
  return res;
}

/* Resolve the current receiver on every access. The returned slot is
 * borrowed only for the immediately following MIR load/store; never retain an
 * exprtk_instance_t pointer across script assignments or invocations. */
static MIR_reg_t ts_emit_oop_numeric_slot(
    ts_mir_compiler_t *c, const char *object_name, const char *member_name,
    int create_if_missing, MIR_label_t missing_label) {
  MIR_reg_t slot_reg = ts_mir_new_temp_preg(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.oop_num_ptr_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_num_ptr_import),
                        MIR_new_reg_op(c->ctx, slot_reg), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_int_op(c->ctx, create_if_missing ? 1 : 0)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, missing_label),
                               MIR_new_reg_op(c->ctx, slot_reg), MIR_new_int_op(c->ctx, 0)));
  return slot_reg;
}

MIR_reg_t ts_try_emit_oop_member_get_slot(ts_mir_compiler_t *c, const char *object_name,
                                                 const char *member_name,
                                                 exprtk_node_t *object_node) {
  (void)object_node;
  if (!ts_oop_member_can_use_num_slot(c, object_name, member_name)) return 0;

  MIR_label_t missing_label = MIR_new_label(c->ctx);
  MIR_label_t done_label = MIR_new_label(c->ctx);
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_var_to_env(c, object_name);
  MIR_reg_t slot_reg =
      ts_emit_oop_numeric_slot(c, object_name, member_name, 0, missing_label);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                               MIR_new_mem_op(c->ctx, MIR_T_D,
                                              (int64_t)offsetof(exprtk_value_t, data.number),
                                              slot_reg, 0, 1)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done_label)));

  MIR_append_insn(c->ctx, c->func, missing_label);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                               MIR_new_double_op(c->ctx, 0.0)));
  MIR_append_insn(c->ctx, c->func, done_label);
  return res;
}

static MIR_reg_t ts_try_emit_oop_member_set_slot(ts_mir_compiler_t *c, const char *object_name,
                                                 const char *member_name,
                                                 toad_node_t *object_node,
                                                 MIR_reg_t value_reg) {
  (void)object_node;
  if (!ts_oop_member_can_use_num_slot(c, object_name, member_name)) return 0;

  MIR_label_t missing_label = MIR_new_label(c->ctx);
  MIR_label_t done_label = MIR_new_label(c->ctx);

  ts_emit_sync_var_to_env(c, object_name);
  MIR_reg_t slot_reg =
      ts_emit_oop_numeric_slot(c, object_name, member_name, 1, missing_label);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV,
                               MIR_new_mem_op(c->ctx, MIR_T_D,
                                              (int64_t)offsetof(exprtk_value_t, data.number),
                                              slot_reg, 0, 1),
                               MIR_new_reg_op(c->ctx, value_reg)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done_label)));
  MIR_append_insn(c->ctx, c->func, missing_label);
  MIR_append_insn(c->ctx, c->func, done_label);
  return value_reg;
}

static MIR_reg_t ts_emit_oop_inline_method_expr(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name,
                                                toad_node_t **params, size_t param_count,
                                                const MIR_reg_t *arg_regs, toad_node_t *expr);
static MIR_reg_t ts_emit_oop_inline_method_stmt(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name, toad_node_t **params,
                                                size_t param_count, const MIR_reg_t *arg_regs,
                                                toad_node_t *stmt, int *returned);

static MIR_reg_t ts_emit_oop_inline_binary(ts_mir_compiler_t *c, const char *object_name,
                                           const char *class_name, toad_node_t **params,
                                           size_t param_count, const MIR_reg_t *arg_regs,
                                           toad_node_t *expr) {
  MIR_reg_t left;
  MIR_reg_t right;
  MIR_reg_t res;
  MIR_insn_code_t op;
  int is_cmp = 0;

  if (!expr->data.binary.left) {
    MIR_reg_t operand = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count,
                                                       arg_regs, expr->data.binary.right);
    if (expr->data.binary.op == toad_TOKEN_PLUS) return operand;
    res = ts_mir_new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DSUB, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 0.0),
                                 MIR_new_reg_op(c->ctx, operand)));
    return res;
  }

  left = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count, arg_regs,
                                        expr->data.binary.left);
  right = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count, arg_regs,
                                         expr->data.binary.right);
  switch (expr->data.binary.op) {
  case toad_TOKEN_PLUS: op = MIR_DADD; break;
  case toad_TOKEN_MINUS: op = MIR_DSUB; break;
  case toad_TOKEN_MULTIPLY: op = MIR_DMUL; break;
  case toad_TOKEN_DIVIDE: op = MIR_DDIV; break;
  case toad_TOKEN_LT: op = MIR_DLT; is_cmp = 1; break;
  case toad_TOKEN_GT: op = MIR_DGT; is_cmp = 1; break;
  case toad_TOKEN_LE: op = MIR_DLE; is_cmp = 1; break;
  case toad_TOKEN_GE: op = MIR_DGE; is_cmp = 1; break;
  case toad_TOKEN_EQ: op = MIR_DEQ; is_cmp = 1; break;
  case toad_TOKEN_NE: op = MIR_DNE; is_cmp = 1; break;
  default: op = MIR_DADD; break;
  }

  if (is_cmp) {
    MIR_reg_t ireg = ts_mir_new_temp_ireg(c);
    res = ts_mir_new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, ireg),
                                 MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_reg_op(c->ctx, ireg)));
    return res;
  }

  res = ts_mir_new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, res),
                               MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
  return res;
}

static MIR_reg_t ts_emit_oop_inline_method_expr(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name,
                                                toad_node_t **params, size_t param_count,
                                                const MIR_reg_t *arg_regs, toad_node_t *expr) {
  if (!expr || expr->type == TOAD_NODE_NULL) return ts_emit_zero_reg(c);

  switch (expr->type) {
  case TOAD_NODE_NUMBER: {
    MIR_reg_t r = ts_mir_new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, expr->data.number)));
    return r;
  }

  case TOAD_NODE_INTEGER: {
    MIR_reg_t r = ts_mir_new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, (double)expr->data.integer)));
    return r;
  }

  case TOAD_NODE_VARIABLE: {
    size_t idx = 0;
    if (ts_inline_param_index(params, param_count, expr->data.variable.name, &idx)) {
      return arg_regs[idx];
    }
    return ts_emit_zero_reg(c);
  }

  case TOAD_NODE_MEMBER_ACCESS:
    return ts_emit_oop_member_get(c, object_name, expr->data.member_access.member,
                                  expr->data.member_access.object);

  case TOAD_NODE_BINARY_OP:
    return ts_emit_oop_inline_binary(c, object_name, class_name, params, param_count, arg_regs,
                                     expr);

  case TOAD_NODE_SUPER: {
    const char *parent_name = ts_direct_parent_class_name(c, class_name);
    toad_node_t *method = NULL;
    MIR_reg_t super_arg_regs[16];
    int returned = 0;

    if (!parent_name || !expr->data.super_expr.is_call || !expr->data.super_expr.member ||
        expr->data.super_expr.arg_count > 16) {
      return ts_emit_zero_reg(c);
    }
    method = ts_find_inlineable_instance_method(c, parent_name, expr->data.super_expr.member,
                                                expr->data.super_expr.arg_count);
    if (!method) return ts_emit_zero_reg(c);

    for (size_t i = 0; i < expr->data.super_expr.arg_count; ++i) {
      super_arg_regs[i] = ts_emit_oop_inline_method_expr(
          c, object_name, class_name, params, param_count, arg_regs, expr->data.super_expr.args[i]);
    }

    return ts_emit_oop_inline_method_stmt(c, object_name, parent_name,
                                          method->data.method.arg_params,
                                          method->data.method.arg_count, super_arg_regs,
                                          method->data.method.body, &returned);
  }

  default:
    return ts_emit_zero_reg(c);
  }
}

static MIR_reg_t ts_emit_oop_inline_method_stmt(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name, toad_node_t **params,
                                                size_t param_count, const MIR_reg_t *arg_regs,
                                                toad_node_t *stmt, int *returned) {
  MIR_reg_t last = 0;

  if (!stmt) return ts_emit_zero_reg(c);
  switch (stmt->type) {
  case TOAD_NODE_BLOCK:
    for (size_t i = 0; i < stmt->data.block.count; ++i) {
      last = ts_emit_oop_inline_method_stmt(c, object_name, class_name, params, param_count, arg_regs,
                                            stmt->data.block.statements[i], returned);
      if (returned && *returned) return last;
    }
    return last ? last : ts_emit_zero_reg(c);

  case TOAD_NODE_FLOW:
    if (stmt->data.flow.type == toad_TOKEN_RETURN) {
      if (returned) *returned = 1;
      return ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count, arg_regs,
                                            stmt->data.flow.value);
    }
    return ts_emit_zero_reg(c);

  case TOAD_NODE_MEMBER_SET: {
    MIR_reg_t value = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count,
                                                     arg_regs, stmt->data.member_set.value);
    return ts_emit_oop_member_set_reg(c, object_name, stmt->data.member_set.member,
                                      stmt->data.member_set.object, value);
  }

  default:
    return ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count,
                                          arg_regs, stmt);
  }
}

MIR_reg_t ts_try_emit_oop_inline_method_call(ts_mir_compiler_t *c,
                                             const char *object_name,
                                             const char *class_name,
                                             const char *method_name, size_t argc,
                                             const MIR_reg_t *arg_regs) {
  int returned = 0;
  toad_node_t *method =
      ts_find_inlineable_instance_method(c, class_name, method_name, argc);
  if (!method) return 0;

  return ts_emit_oop_inline_method_stmt(c, object_name, class_name, method->data.method.arg_params,
                                        method->data.method.arg_count, arg_regs,
                                        method->data.method.body, &returned);
}

MIR_reg_t ts_emit_oop_instanceof(ts_mir_compiler_t *c, const char *object_name,
                                 const char *class_name) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.oop_instanceof_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_instanceof_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)class_name)));
  ts_emit_reload_from_env(c);
  return res;
}

void ts_mir_oop_define_class(void *ctx_ptr, void *node_ptr) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  toad_node_t *node = (toad_node_t *)node_ptr;
  if (!ctx || !node) return;
  toad_oop_define_class(node, &ctx->env);
  if (ctx->env.flow != toad_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
}

double ts_mir_oop_class_alias(void *ctx_ptr, const char *target_name, const char *source_name) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !source_name) return 0.0;

  toad_value_t result = toad_oop_alias_class(target_name, source_name, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_new_assign(void *ctx_ptr, const char *target_name, const char *class_name,
                             int64_t argc, double *argv) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !class_name) return 0.0;

  toad_value_t instance = toad_oop_instantiate_numeric(class_name, (size_t)argc, argv, &ctx->env);
  if (ctx->env.aborted) return 0.0;
  toad_env_set(&ctx->env, target_name, instance);
  return ts_mir_numeric_value(instance);
}

double ts_mir_oop_member_call(void *ctx_ptr, const char *object_name, const char *method_name,
                              void *object_node, int64_t argc, double *argv) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  toad_env_t *env;
  if (!ctx || !object_name || !method_name) return 0.0;
  env = ts_task_execution_env(ctx);

  toad_value_t result = toad_member_call_checked_numeric(
      object_name, method_name, (toad_node_t *)object_node, (size_t)argc, argv, env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_value(void *ctx_ptr, const char *object_name,
                                    const char *method_name, void *object_node,
                                    void *call_node) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  toad_env_t *env;
  if (!ctx || !object_name || !method_name) return 0.0;
  env = ts_task_execution_env(ctx);

  toad_value_t result = toad_member_call_checked_value_nodes(
      object_name, method_name, (toad_node_t *)object_node,
      (toad_node_t *)call_node, env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_mono_value(void *ctx_ptr, const char *object_name,
                                         const char *expected_class_name,
                                         const char *method_name, void *object_node,
                                         void *call_node) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !expected_class_name || !method_name) return 0.0;

  toad_value_t result = toad_oop_call_method_mono_checked_value_nodes(
      object_name, expected_class_name, method_name, (toad_node_t *)object_node,
      (toad_node_t *)call_node, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_cached(void *ctx_ptr, const char *object_name,
                                     const char *method_name, void *cache_ptr, void *object_node,
                                     int64_t argc, double *argv) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !method_name) return 0.0;

  toad_value_t object = toad_env_get(&ctx->env, object_name);
  toad_value_t result;
  if (object.type == TOAD_VAL_INSTANCE || object.type == TOAD_VAL_CLASS) {
    result = toad_oop_call_method_cached_checked_numeric(
        object_name, method_name, (toad_oop_method_cache_t *)cache_ptr,
        (toad_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  } else {
    result = toad_member_call_checked_numeric(
        object_name, method_name, (toad_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  }
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_assign(void *ctx_ptr, const char *target_name,
                                     const char *object_name, const char *method_name,
                                     void *cache_ptr, void *object_node, int64_t argc,
                                     double *argv) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !object_name || !method_name) return 0.0;

  toad_value_t object = toad_env_get(&ctx->env, object_name);
  toad_value_t result;
  if (object.type == TOAD_VAL_INSTANCE || object.type == TOAD_VAL_CLASS) {
    result = toad_oop_call_method_cached_checked_numeric(
        object_name, method_name, (toad_oop_method_cache_t *)cache_ptr,
        (toad_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  } else {
    result = toad_member_call_checked_numeric(
        object_name, method_name, (toad_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  }
  toad_env_set(&ctx->env, target_name, result);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_assign_value(void *ctx_ptr, const char *target_name,
                                           const char *object_name, const char *method_name,
                                           void *object_node, void *call_node) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !object_name || !method_name) return 0.0;

  toad_value_t result = toad_member_call_checked_value_nodes(
      object_name, method_name, (toad_node_t *)object_node,
      (toad_node_t *)call_node, &ctx->env);
  toad_env_set(&ctx->env, target_name, result);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_get(void *ctx_ptr, const char *object_name, const char *member_name,
                             void *cache_ptr, void *object_node) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !member_name) return 0.0;

  toad_value_t result = toad_oop_get_member_cached_checked(
      object_name, member_name, (toad_oop_field_cache_t *)cache_ptr,
      (toad_node_t *)object_node, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_set(void *ctx_ptr, const char *object_name, const char *member_name,
                             void *cache_ptr, void *object_node, double value) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !member_name) return 0.0;

  toad_value_t result = toad_oop_set_member_cached_checked_numeric(
      object_name, member_name, value, (toad_oop_field_cache_t *)cache_ptr,
      (toad_node_t *)object_node, &ctx->env);
  return ts_mir_numeric_value(result);
}

void *ts_mir_oop_num_ptr(void *ctx_ptr, const char *object_name,
                         const char *member_name, int64_t create_if_missing) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_instance_t *instance;
  exprtk_value_t object;
  exprtk_class_t *field_owner = NULL;
  const cmeta_data_desc *class_data;
  const cmeta_data_struct_shape *shape;
  const cmeta_data_field_desc *field;
  cmeta_object_ref reflected = CMETA_OBJECT_REF_INIT;
  exprtk_value_t *slot;
  int access;

  if (!ctx || !object_name || !member_name ||
      (create_if_missing != 0 && create_if_missing != 1))
    return NULL;

  object = exprtk_env_get(&ctx->env, object_name);
  if (object.type != EXPRTK_VAL_INSTANCE || !object.data.instance_val.instance) {
    ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT runtime error: '%s.%s' requires an instance numeric field",
             object_name, member_name);
    snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
    ctx->env.aborted = 1;
    return NULL;
  }
  instance = object.data.instance_val.instance;

  /*
   * Resolve the current instance for every access. The prior implementation
   * cached an exprtk_instance_t pointer in MIR-immediate memory, then reused it
   * after the script variable could be reassigned and the old instance freed.
   */
  access = exprtk_class_get_instance_field_access(
      instance->klass, member_name, &field_owner);
  if (access != TOAD_ACCESS_PUBLIC ||
      toad_instance_borrow_cmeta_object(instance, &reflected) != CMETA_OK) {
    ctx->error_code = TOAD_SCRIPT_ERROR_RUNTIME;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT runtime error: field '%s' is not admitted for direct numeric access",
             member_name);
    snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
    ctx->env.aborted = 1;
    return NULL;
  }

  class_data = reflected.data;
  shape = class_data && class_data->kind == CMETA_DATA_STRUCT
              ? (const cmeta_data_struct_shape *)class_data->shape
              : NULL;
  field = cmeta_data_struct_find_field(shape, member_name);
  if (!field || !cmeta_data_desc_valid(field->value) ||
      !cmeta_data_desc_equal(field->value, &cmeta_data_double)) {
    cmeta_object_release(&reflected);
    ctx->error_code = TOAD_SCRIPT_ERROR_RUNTIME;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT runtime error: field '%s' is not a canonical number field",
             member_name);
    snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
    ctx->env.aborted = 1;
    return NULL;
  }
  cmeta_object_release(&reflected);

  slot = exprtk_instance_get_field_slot(instance, member_name);
  if (!slot && create_if_missing) {
    toad_instance_set_field(instance, member_name, toad_val_num(0.0));
    slot = toad_instance_get_field_slot(instance, member_name);
  }
  {
    uintptr_t slot_addr = (uintptr_t)slot;
    uintptr_t slots_begin = (uintptr_t)instance->field_slots;
    uintptr_t slots_end =
        slots_begin + instance->field_slot_count * sizeof(*instance->field_slots);
    if (!slot || !instance->field_slots ||
        slot_addr < slots_begin || slot_addr >= slots_end) {
      ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
      snprintf(ctx->error_msg, sizeof(ctx->error_msg),
               "JIT runtime error: instance has no numeric slot field '%s'", member_name);
      snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
      ctx->env.aborted = 1;
      return NULL;
    }
  }

  if (slot->type != TOAD_VAL_NUMBER) {
    ctx->error_code = TOAD_SCRIPT_ERROR_RUNTIME;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT runtime error: field '%s' violates its canonical number storage",
             member_name);
    snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
    ctx->env.aborted = 1;
    return NULL;
  }

  (void)field_owner;
  return slot;
}

double ts_mir_oop_instanceof(void *ctx_ptr, const char *object_name, const char *class_name) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !class_name) return 0.0;

  toad_value_t result = toad_oop_instanceof_name(object_name, class_name, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_predicate(void *ctx_ptr, int64_t kind, const char *name, const char *member,
                            void *object_node) {
  toad_script_ctx_t *ctx = (toad_script_ctx_t *)ctx_ptr;
  if (!ctx || !name) return 0.0;

  toad_value_t value;
  if (member) {
    toad_value_t object = toad_env_get(&ctx->env, name);
    if (toad_value_is_object_like(&object) && toad_map_has(&object, member)) {
      value = toad_map_get(&object, member);
    } else {
      value = toad_oop_get_member_checked(name, member, (toad_node_t *)object_node, &ctx->env);
    }
  } else {
    value = toad_env_get(&ctx->env, name);
  }

  switch (kind) {
    case 1:
      return value.type == TOAD_VAL_CLASS ? 1.0 : 0.0;
    case 2:
      return value.type == TOAD_VAL_INSTANCE ? 1.0 : 0.0;
    case 3:
      return value.type == TOAD_VAL_FUNCTION || value.type == TOAD_VAL_BOUND_METHOD ? 1.0
                                                                                         : 0.0;
    case 4:
      return value.type == TOAD_VAL_NULL ? 1.0 : 0.0;
    default:
      return 0.0;
  }
}
