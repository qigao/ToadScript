/* Compile-time constant folding helpers - MIR Backend. */

#include "internal.h"
#include <toad/grammar.h>
#include <math.h>
/* =========================================================================
 *  Compile-time constant folding
 * Recursively evaluates constant expression trees, returns 1 if foldable.
 * ========================================================================= */

int ts_binary_is_null_eq_compare(toad_node_t *node) {
  if (!node || node->type != TOAD_NODE_BINARY_OP || !node->data.binary.left) return 0;
  if (node->data.binary.op != toad_TOKEN_EQ && node->data.binary.op != toad_TOKEN_NE) return 0;
  return (node->data.binary.left && node->data.binary.left->type == TOAD_NODE_NULL) ||
         (node->data.binary.right && node->data.binary.right->type == TOAD_NODE_NULL);
}

int ts_try_fold_constant(toad_node_t *node, double *out) {
  if (!node) return 0;

  if (node->type == TOAD_NODE_NUMBER) {
    *out = node->data.number;
    return 1;
  }

  if (node->type == TOAD_NODE_INTEGER) {
    *out = (double)node->data.integer;
    return 1;
  }

  if (node->type == TOAD_NODE_NULL) {
    *out = 0.0;
    return 1;
  }

  if (node->type == TOAD_NODE_BINARY_OP) {
    /* Unary operators */
    if (node->data.binary.left == NULL) {
      double r;
      if (!ts_try_fold_constant(node->data.binary.right, &r)) return 0;
      switch (node->data.binary.op) {
      case toad_TOKEN_MINUS:
        *out = -r;
        return 1;
      case toad_TOKEN_PLUS:
        *out = r;
        return 1;
      case toad_TOKEN_NOT:
        *out = (r == 0.0) ? 1.0 : 0.0;
        return 1;
      default:
        return 0;
      }
    }

    if (ts_binary_is_null_eq_compare(node)) {
      toad_node_t *left = node->data.binary.left;
      toad_node_t *right = node->data.binary.right;
      int left_const = left && (left->type == TOAD_NODE_NULL ||
                                left->type == TOAD_NODE_NUMBER ||
                                left->type == TOAD_NODE_INTEGER ||
                                left->type == TOAD_NODE_STRING);
      int right_const = right && (right->type == TOAD_NODE_NULL ||
                                  right->type == TOAD_NODE_NUMBER ||
                                  right->type == TOAD_NODE_INTEGER ||
                                  right->type == TOAD_NODE_STRING);
      int both_null = 0;
      if (!left_const || !right_const) return 0;
      both_null = left->type == TOAD_NODE_NULL && right->type == TOAD_NODE_NULL;
      *out = node->data.binary.op == toad_TOKEN_EQ ? (double)both_null
                                                     : (double)!both_null;
      return 1;
    }

    double l, r;
    if (!ts_try_fold_constant(node->data.binary.left, &l)) return 0;
    if (!ts_try_fold_constant(node->data.binary.right, &r)) return 0;

    switch (node->data.binary.op) {
    case toad_TOKEN_PLUS:
      *out = l + r;
      return 1;
    case toad_TOKEN_MINUS:
      *out = l - r;
      return 1;
    case toad_TOKEN_MULTIPLY:
      *out = l * r;
      return 1;
    case toad_TOKEN_DIVIDE:
      *out = (r != 0.0) ? l / r : 0.0;
      return 1;
    case toad_TOKEN_MOD:
      *out = fmod(l, r);
      return 1;
    case toad_TOKEN_POWER:
      *out = pow(l, r);
      return 1;
    case toad_TOKEN_LT:
      *out = (l < r) ? 1.0 : 0.0;
      return 1;
    case toad_TOKEN_GT:
      *out = (l > r) ? 1.0 : 0.0;
      return 1;
    case toad_TOKEN_LE:
      *out = (l <= r) ? 1.0 : 0.0;
      return 1;
    case toad_TOKEN_GE:
      *out = (l >= r) ? 1.0 : 0.0;
      return 1;
    case toad_TOKEN_EQ:
      *out = (l == r) ? 1.0 : 0.0;
      return 1;
    case toad_TOKEN_NE:
      *out = (l != r) ? 1.0 : 0.0;
      return 1;
    case toad_TOKEN_AND:
      *out = (l != 0.0 && r != 0.0) ? 1.0 : 0.0;
      return 1;
    case toad_TOKEN_OR:
      *out = (l != 0.0 || r != 0.0) ? 1.0 : 0.0;
      return 1;
    default:
      return 0;
    }
  }

  return 0;
}
