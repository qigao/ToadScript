/**
 * @file parser.c
 * @brief DSL Grammar: Lexer, Parser, and AST nodes
 * Separated from monolithic tbs_core.c.
 */

#include "../internal.h"
#include "../../parser/lexer.h"
#include <turboscript/detail/grammar_gen.h>

void tbsParse(void *yyp, int yymajor, tbs_token_t yyminor, tbs_parse_ctx_t *ctx);
void *tbsParseAlloc(void *(*mallocProc)(size_t));
void tbsParseFree(void *p, void (*freeProc)(void*));
void tbsParseTrace(FILE *TraceFILE, char *zTracePrompt);


// Deep copy an AST node and its children into a new arena
tbs_node_t *tbs_node_copy(const tbs_node_t *src, mem_pool_t *dest_arena) {
    if (!src) return NULL;
    tbs_node_t *dst = (tbs_node_t*)mem_alloc(dest_arena, sizeof(tbs_node_t));
    if (!dst) return NULL;

    memcpy(dst, src, sizeof(tbs_node_t));
    dst->arena = dest_arena;

    switch (src->type) {
        case TBS_NODE_NUMBER:
        case TBS_NODE_VARIABLE:
            if (src->type == TBS_NODE_VARIABLE && src->data.variable.name) {
                dst->data.variable.name = mem_strdup(dest_arena, src->data.variable.name);
            }
            break;

        case TBS_NODE_BINARY_OP:
            if (src->data.binary.left) dst->data.binary.left = tbs_node_copy(src->data.binary.left, dest_arena);
            dst->data.binary.right = tbs_node_copy(src->data.binary.right, dest_arena);
            break;

        case TBS_NODE_FUNCTION_CALL:
            if (src->data.function.name) dst->data.function.name = mem_strdup(dest_arena, src->data.function.name);
            if (src->data.function.arg_count > 0) {
                dst->data.function.args = (tbs_node_t**)mem_alloc(dest_arena, src->data.function.arg_count * sizeof(tbs_node_t*));
                for (size_t i = 0; i < src->data.function.arg_count; ++i) {
                    dst->data.function.args[i] = tbs_node_copy(src->data.function.args[i], dest_arena);
                }
            }
            break;

        case TBS_NODE_CONSTANT_DECL:
        case TBS_NODE_ASSIGNMENT:
            if (src->data.assignment.name) dst->data.assignment.name = mem_strdup(dest_arena, src->data.assignment.name);
            dst->data.assignment.value = tbs_node_copy(src->data.assignment.value, dest_arena);
            break;

        case TBS_NODE_IF:
            dst->data.if_stmt.condition = tbs_node_copy(src->data.if_stmt.condition, dest_arena);
            dst->data.if_stmt.if_branch = tbs_node_copy(src->data.if_stmt.if_branch, dest_arena);
            if (src->data.if_stmt.else_branch) dst->data.if_stmt.else_branch = tbs_node_copy(src->data.if_stmt.else_branch, dest_arena);
            break;

        case TBS_NODE_WHILE:
            dst->data.while_loop.condition = tbs_node_copy(src->data.while_loop.condition, dest_arena);
            dst->data.while_loop.body = tbs_node_copy(src->data.while_loop.body, dest_arena);
            break;

        case TBS_NODE_FOR:
            if (src->data.for_loop.init) dst->data.for_loop.init = tbs_node_copy(src->data.for_loop.init, dest_arena);
            if (src->data.for_loop.condition) dst->data.for_loop.condition = tbs_node_copy(src->data.for_loop.condition, dest_arena);
            if (src->data.for_loop.post) dst->data.for_loop.post = tbs_node_copy(src->data.for_loop.post, dest_arena);
            dst->data.for_loop.body = tbs_node_copy(src->data.for_loop.body, dest_arena);
            break;

        case TBS_NODE_BLOCK:
            if (src->data.block.count > 0) {
                dst->data.block.statements = (tbs_node_t**)mem_alloc(dest_arena, src->data.block.count * sizeof(tbs_node_t*));
                for (size_t i = 0; i < src->data.block.count; ++i) {
                    dst->data.block.statements[i] = tbs_node_copy(src->data.block.statements[i], dest_arena);
                }
            }
            break;

        case TBS_NODE_FLOW:
            if (src->data.flow.value) dst->data.flow.value = tbs_node_copy(src->data.flow.value, dest_arena);
            break;

        case TBS_NODE_STRING:
            if (src->data.string.value.data) {
                char *buf = (char*)mem_alloc(dest_arena, src->data.string.value.len + 1);
                memcpy(buf, src->data.string.value.data, src->data.string.value.len);
                buf[src->data.string.value.len] = '\0';
                dst->data.string.value.data = buf;
            }
            break;

        case TBS_NODE_VECTOR:
            if (src->data.vector.count > 0) {
                dst->data.vector.elements = (tbs_node_t**)mem_alloc(dest_arena, src->data.vector.count * sizeof(tbs_node_t*));
                for (size_t i = 0; i < src->data.vector.count; ++i) {
                    dst->data.vector.elements[i] = tbs_node_copy(src->data.vector.elements[i], dest_arena);
                }
            }
            break;

        case TBS_NODE_INDEX:
            dst->data.index_access.array = tbs_node_copy(src->data.index_access.array, dest_arena);
            dst->data.index_access.index = tbs_node_copy(src->data.index_access.index, dest_arena);
            break;

        case TBS_NODE_SLICE:
            dst->data.slice.array = tbs_node_copy(src->data.slice.array, dest_arena);
            if (src->data.slice.start) dst->data.slice.start = tbs_node_copy(src->data.slice.start, dest_arena);
            if (src->data.slice.end) dst->data.slice.end = tbs_node_copy(src->data.slice.end, dest_arena);
            break;

        case TBS_NODE_FUNCTION_EXPRESSION:
        case TBS_NODE_FUNCTION_DEFINITION:
            if (src->data.func_def.name) dst->data.func_def.name = mem_strdup(dest_arena, src->data.func_def.name);
            if (src->data.func_def.arg_count > 0) {
                dst->data.func_def.arg_params = (tbs_node_t**)mem_alloc(dest_arena, src->data.func_def.arg_count * sizeof(tbs_node_t*));
                for (size_t i = 0; i < src->data.func_def.arg_count; ++i) {
                    dst->data.func_def.arg_params[i] = tbs_node_copy(src->data.func_def.arg_params[i], dest_arena);
                }
            }
            dst->data.func_def.body = tbs_node_copy(src->data.func_def.body, dest_arena);
            break;

        case TBS_NODE_MEMBER_CALL:
            dst->data.member_call.object = tbs_node_copy(src->data.member_call.object, dest_arena);
            if (src->data.member_call.method)
                dst->data.member_call.method = mem_strdup(dest_arena, src->data.member_call.method);
            if (src->data.member_call.arg_count > 0) {
                dst->data.member_call.args = (tbs_node_t**)mem_alloc(dest_arena,
                    src->data.member_call.arg_count * sizeof(tbs_node_t*));
                for (size_t i = 0; i < src->data.member_call.arg_count; ++i)
                    dst->data.member_call.args[i] = tbs_node_copy(src->data.member_call.args[i], dest_arena);
            }
            break;

        case TBS_NODE_INSTANCEOF:
            dst->data.instanceof_expr.object =
                tbs_node_copy(src->data.instanceof_expr.object, dest_arena);
            if (src->data.instanceof_expr.class_name)
                dst->data.instanceof_expr.class_name =
                    mem_strdup(dest_arena, src->data.instanceof_expr.class_name);
            dst->data.instanceof_expr.class_expr =
                tbs_node_copy(src->data.instanceof_expr.class_expr, dest_arena);
            break;

        case TBS_NODE_SPREAD:
            dst->data.spread.child = tbs_node_copy(src->data.spread.child, dest_arena);
            break;

        case TBS_NODE_DESTRUCTURING_ASSIGNMENT:
            dst->data.destructuring.targets = tbs_node_copy(src->data.destructuring.targets, dest_arena);
            dst->data.destructuring.value = tbs_node_copy(src->data.destructuring.value, dest_arena);
            dst->data.destructuring.is_constant = src->data.destructuring.is_constant;
            break;

        case TBS_NODE_REST_PARAMETER:
            /* No extra data to copy besides type and positional info done in entry */
            break;

        case TBS_NODE_SWITCH:
            dst->data.switch_stmt.value = tbs_node_copy(src->data.switch_stmt.value, dest_arena);
            if (src->data.switch_stmt.case_count > 0) {
                dst->data.switch_stmt.cases = (tbs_node_t**)mem_alloc(dest_arena, src->data.switch_stmt.case_count * 2 * sizeof(tbs_node_t*));
                for (size_t i = 0; i < src->data.switch_stmt.case_count * 2; ++i) {
                    dst->data.switch_stmt.cases[i] = tbs_node_copy(src->data.switch_stmt.cases[i], dest_arena);
                }
            }
            if (src->data.switch_stmt.default_case) {
                dst->data.switch_stmt.default_case = tbs_node_copy(src->data.switch_stmt.default_case, dest_arena);
            }
            break;

        case TBS_NODE_DO_WHILE:
            dst->data.do_while.body = tbs_node_copy(src->data.do_while.body, dest_arena);
            dst->data.do_while.condition = tbs_node_copy(src->data.do_while.condition, dest_arena);
            break;

        case TBS_NODE_MAP_LITERAL:
            if (src->data.map_literal.count > 0) {
                dst->data.map_literal.keys = (char**)mem_alloc(dest_arena, src->data.map_literal.count * sizeof(char*));
                dst->data.map_literal.values = (tbs_node_t**)mem_alloc(dest_arena, src->data.map_literal.count * sizeof(tbs_node_t*));
                for (size_t i = 0; i < src->data.map_literal.count; ++i) {
                    dst->data.map_literal.keys[i] = mem_strdup(dest_arena, src->data.map_literal.keys[i]);
                    dst->data.map_literal.values[i] = tbs_node_copy(src->data.map_literal.values[i], dest_arena);
                }
            }
            break;

        case TBS_NODE_MEMBER_ACCESS:
            dst->data.member_access.object = tbs_node_copy(src->data.member_access.object, dest_arena);
            if (src->data.member_access.member)
                dst->data.member_access.member = mem_strdup(dest_arena, src->data.member_access.member);
            break;

        case TBS_NODE_MEMBER_SET:
            dst->data.member_set.object = tbs_node_copy(src->data.member_set.object, dest_arena);
            if (src->data.member_set.member)
                dst->data.member_set.member = mem_strdup(dest_arena, src->data.member_set.member);
            dst->data.member_set.value = tbs_node_copy(src->data.member_set.value, dest_arena);
            break;

        case TBS_NODE_FOR_IN:
            if (src->data.for_in.var_name)
                dst->data.for_in.var_name = mem_strdup(dest_arena, src->data.for_in.var_name);
            dst->data.for_in.collection = tbs_node_copy(src->data.for_in.collection, dest_arena);
            dst->data.for_in.body = tbs_node_copy(src->data.for_in.body, dest_arena);
            break;

        case TBS_NODE_NULL:
            /* No data to copy */
            break;

        case TBS_NODE_TEMPLATE_STRING:
            if (src->data.template_string.template_str) {
                dst->data.template_string.template_str = mem_strdup(dest_arena, src->data.template_string.template_str);
            }
            dst->data.template_string.len = src->data.template_string.len;
            break;

        case TBS_NODE_TRY_CATCH:
            dst->data.try_catch.try_body = tbs_node_copy(src->data.try_catch.try_body, dest_arena);
            if (src->data.try_catch.catch_var)
                dst->data.try_catch.catch_var = mem_strdup(dest_arena, src->data.try_catch.catch_var);
            dst->data.try_catch.catch_body = tbs_node_copy(src->data.try_catch.catch_body, dest_arena);
            break;

        case TBS_NODE_THROW:
            dst->data.throw_stmt.value = tbs_node_copy(src->data.throw_stmt.value, dest_arena);
            break;
    }
    return dst;
}

// Create a new node using the arena
tbs_node_t *tbs_node_create(mem_pool_t *arena, tbs_node_type_t type) {
    tbs_node_t *n = MEM_ALLOC(arena, tbs_node_t);
    if (n) {
        memset(n, 0, sizeof(tbs_node_t));
        n->type = type;
        n->arena = arena;
        n->line = 0;
        n->column = 0;
    }
    return n;
}

tbs_node_t *tbs_fold_binary(tbs_parse_ctx_t *ctx, int op, tbs_node_t *left, tbs_node_t *right) {
    if (!left || !right) return NULL;

    if (left->type == TBS_NODE_NUMBER && right->type == TBS_NODE_NUMBER) {
        double l = left->data.number;
        double r = right->data.number;
        double res = 0;
        int folded = 1;

        switch (op) {
            case tbs_TOKEN_PLUS: res = l + r; break;
            case tbs_TOKEN_MINUS: res = l - r; break;
            case tbs_TOKEN_MULTIPLY: res = l * r; break;
            case tbs_TOKEN_DIVIDE: if (r != 0) res = l / r; else folded = 0; break;
            case tbs_TOKEN_MOD: if (r != 0) res = fmod(l, r); else folded = 0; break;
            case tbs_TOKEN_POWER: res = pow(l, r); break;
            case tbs_TOKEN_EQ: res = (l == r); break;
            case tbs_TOKEN_NE: res = (l != r); break;
            case tbs_TOKEN_LT: res = (l < r); break;
            case tbs_TOKEN_LE: res = (l <= r); break;
            case tbs_TOKEN_GT: res = (l > r); break;
            case tbs_TOKEN_GE: res = (l >= r); break;
            case tbs_TOKEN_AND: res = (l && r); break;
            case tbs_TOKEN_OR: res = (l || r); break;
            default: folded = 0; break;
        }

        if (folded) {
            tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_NUMBER);
            if (n) {
                n->data.number = res;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }

    // String concatenation folding
    if (op == tbs_TOKEN_PLUS && left->type == TBS_NODE_STRING && right->type == TBS_NODE_STRING) {
        size_t new_len = left->data.string.value.len + right->data.string.value.len;
        char *buf = (char*)mem_alloc(ctx->arena, new_len + 1);
        if (buf) {
            memcpy(buf, left->data.string.value.data, left->data.string.value.len);
            memcpy(buf + left->data.string.value.len, right->data.string.value.data, right->data.string.value.len);
            buf[new_len] = '\0';
            tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_STRING);
            if (n) {
                n->data.string.value.data = buf;
                n->data.string.value.len = new_len;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }

    // Number + String concatenation folding
    if (op == tbs_TOKEN_PLUS &&
        ((left->type == TBS_NODE_STRING && right->type == TBS_NODE_NUMBER) ||
         (left->type == TBS_NODE_NUMBER && right->type == TBS_NODE_STRING))) {
        char n_buf[32];
        const char *l_data, *r_data;
        size_t l_len, r_len;

        if (left->type == TBS_NODE_NUMBER) {
            l_len = snprintf(n_buf, sizeof(n_buf), "%g", left->data.number);
            l_data = n_buf;
        } else {
            l_data = left->data.string.value.data;
            l_len = left->data.string.value.len;
        }

        char r_buf[32];
        if (right->type == TBS_NODE_NUMBER) {
            r_len = snprintf(r_buf, sizeof(r_buf), "%g", right->data.number);
            r_data = r_buf;
        } else {
            r_data = right->data.string.value.data;
            r_len = right->data.string.value.len;
        }

        size_t new_len = l_len + r_len;
        char *buf = (char*)mem_alloc(ctx->arena, new_len + 1);
        if (buf) {
            memcpy(buf, l_data, l_len);
            memcpy(buf + l_len, r_data, r_len);
            buf[new_len] = '\0';
            tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_STRING);
            if (n) {
                n->data.string.value.data = buf;
                n->data.string.value.len = new_len;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }

    tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = left;
    n->data.binary.right = right;
    return n;
}

tbs_node_t *tbs_fold_unary(tbs_parse_ctx_t *ctx, int op, tbs_node_t *child) {
    if (!child) return NULL;
    if (child->type == TBS_NODE_NUMBER) {
        double v = child->data.number;
        if (op == tbs_TOKEN_MINUS) {
            tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_NUMBER);
            n->data.number = -v;
            return n;
        }
        if (op == tbs_TOKEN_PLUS) return child;
        if (op == tbs_TOKEN_NOT) {
            tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_NUMBER);
            n->data.number = (fabs(v) < 1e-9 ? 1.0 : 0.0);
            return n;
        }
    }

    tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = NULL;
    n->data.binary.right = child;
    return n;
}

tbs_node_t *tbs_fold_if(tbs_parse_ctx_t *ctx, tbs_node_t *cond, tbs_node_t *if_branch, tbs_node_t *else_branch) {
    if (cond->type == TBS_NODE_NUMBER) {
        double v = cond->data.number;
        if (fabs(v) > 1e-9) {
            return if_branch;
        } else {
            return else_branch ? else_branch : tbs_node_create(ctx->arena, TBS_NODE_BLOCK);
        }
    }
    tbs_node_t *n = tbs_node_create(ctx->arena, TBS_NODE_IF);
    n->data.if_stmt.condition = cond;
    n->data.if_stmt.if_branch = if_branch;
    n->data.if_stmt.else_branch = else_branch;
    return n;
}

tbs_node_t *tbs_parse_ext(const char *input, size_t length,
                                 mem_pool_t *arena, int *error,
                                 char *error_msg, size_t error_msg_len) {
    if (!input) return NULL;
    if (length == 0) length = strlen(input);

    tbs_lexer_t lexer;
    tbs_lexer_init(&lexer, input, length);

    tbs_parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena = arena;

    void *parser = tbsParseAlloc(malloc);
    if (!parser) {
        if (error) *error = 1;
        return NULL;
    }

    tbs_token_t token;
    memset(&token, 0, sizeof(token));
    int ret;
    while ((ret = tbs_lexer_next(&lexer, &token)) > 0) {
        tbsParse(parser, ret, token, &ctx);
        if (ctx.fatal_error) break;
    }

    if (ret < 0) {
        ctx.error = 1;
        ctx.fatal_error = 1;
        if (ctx.error_msg[0] == '\0') {
            if (lexer.error[0] != '\0') {
                snprintf(ctx.error_msg, sizeof(ctx.error_msg), "%s", lexer.error);
            } else {
                snprintf(ctx.error_msg, sizeof(ctx.error_msg), "Lexer error at line %d", lexer.line);
            }
        }
    } else if (!ctx.fatal_error) {
        tbs_token_t end_token;
        memset(&end_token, 0, sizeof(end_token));
        tbsParse(parser, 0, end_token, &ctx);
    }

    tbsParseFree(parser, free);

    if (error) *error = ctx.error;
    if (ctx.error && error_msg && error_msg_len > 0) {
        strncpy(error_msg, ctx.error_msg, error_msg_len - 1);
        error_msg[error_msg_len - 1] = '\0';
    }

    return ctx.error ? NULL : ctx.root;
}

tbs_node_t *tbs_parse(const char *input, size_t length) {
    mem_pool_t *arena = (mem_pool_t*)malloc(sizeof(mem_pool_t));
    if (!arena || mem_init(arena, 4096) != 0) {
        if (arena) free(arena);
        return NULL;
    }
    int err = 0;
    tbs_node_t *root = tbs_parse_ext(input, length, arena, &err, NULL, 0);
    if (err || !root) {
        mem_destroy(arena);
        free(arena);
        return NULL;
    }
    return root;
}

void tbs_free(tbs_node_t *node) {
    if (!node || !node->arena) return;
    mem_pool_t *arena = node->arena;
    mem_destroy(arena);
    free(arena);
}

size_t tbs_node_count(const tbs_node_t *node) {
    if (!node) return 0;
    size_t count = 1;
    switch (node->type) {
        case TBS_NODE_BINARY_OP:
            if (node->data.binary.left) count += tbs_node_count(node->data.binary.left);
            count += tbs_node_count(node->data.binary.right);
            break;
        case TBS_NODE_IF:
            count += tbs_node_count(node->data.if_stmt.condition);
            count += tbs_node_count(node->data.if_stmt.if_branch);
            if (node->data.if_stmt.else_branch) count += tbs_node_count(node->data.if_stmt.else_branch);
            break;
        case TBS_NODE_WHILE:
            count += tbs_node_count(node->data.while_loop.condition);
            count += tbs_node_count(node->data.while_loop.body);
            break;
        case TBS_NODE_FOR:
            if (node->data.for_loop.init) count += tbs_node_count(node->data.for_loop.init);
            if (node->data.for_loop.condition) count += tbs_node_count(node->data.for_loop.condition);
            if (node->data.for_loop.post) count += tbs_node_count(node->data.for_loop.post);
            count += tbs_node_count(node->data.for_loop.body);
            break;
        case TBS_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                count += tbs_node_count(node->data.block.statements[i]);
            }
            break;
        case TBS_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                count += tbs_node_count(node->data.function.args[i]);
            }
            break;
        case TBS_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                count += tbs_node_count(node->data.vector.elements[i]);
            }
            break;
        case TBS_NODE_INDEX:
            count += tbs_node_count(node->data.index_access.array);
            count += tbs_node_count(node->data.index_access.index);
            break;
        case TBS_NODE_SLICE:
            count += tbs_node_count(node->data.slice.array);
            if (node->data.slice.start) count += tbs_node_count(node->data.slice.start);
            if (node->data.slice.end) count += tbs_node_count(node->data.slice.end);
            break;
        case TBS_NODE_FUNCTION_EXPRESSION:
        case TBS_NODE_FUNCTION_DEFINITION:
            for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
                count += tbs_node_count(node->data.func_def.arg_params[i]);
            }
            count += tbs_node_count(node->data.func_def.body);
            break;
        case TBS_NODE_MEMBER_CALL:
            count += tbs_node_count(node->data.member_call.object);
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i)
                count += tbs_node_count(node->data.member_call.args[i]);
            break;
        case TBS_NODE_ASSIGNMENT:
            count += tbs_node_count(node->data.assignment.value);
            break;
        case TBS_NODE_FLOW:
            if (node->data.flow.value) count += tbs_node_count(node->data.flow.value);
            break;
        case TBS_NODE_CONSTANT_DECL:
            count += tbs_node_count(node->data.assignment.value);
            break;
        case TBS_NODE_SWITCH:
            count += tbs_node_count(node->data.switch_stmt.value);
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                count += tbs_node_count(node->data.switch_stmt.cases[i]);
            }
            if (node->data.switch_stmt.default_case) count += tbs_node_count(node->data.switch_stmt.default_case);
            break;
        case TBS_NODE_DO_WHILE:
            count += tbs_node_count(node->data.do_while.body);
            count += tbs_node_count(node->data.do_while.condition);
            break;
        case TBS_NODE_MAP_LITERAL:
            for (size_t i = 0; i < node->data.map_literal.count; ++i)
                count += tbs_node_count(node->data.map_literal.values[i]);
            break;
        case TBS_NODE_MEMBER_ACCESS:
            count += tbs_node_count(node->data.member_access.object);
            break;
        case TBS_NODE_MEMBER_SET:
            count += tbs_node_count(node->data.member_set.object);
            count += tbs_node_count(node->data.member_set.value);
            break;
        case TBS_NODE_FOR_IN:
            count += tbs_node_count(node->data.for_in.collection);
            count += tbs_node_count(node->data.for_in.body);
            break;
        case TBS_NODE_SPREAD:
            count += tbs_node_count(node->data.spread.child);
            break;

        case TBS_NODE_DESTRUCTURING_ASSIGNMENT:
            count += tbs_node_count(node->data.destructuring.targets);
            count += tbs_node_count(node->data.destructuring.value);
            break;
        case TBS_NODE_REST_PARAMETER:
            break;

        case TBS_NODE_TRY_CATCH:
            count += tbs_node_count(node->data.try_catch.try_body);
            count += tbs_node_count(node->data.try_catch.catch_body);
            break;
        case TBS_NODE_THROW:
            if (node->data.throw_stmt.value) count += tbs_node_count(node->data.throw_stmt.value);
            break;

        case TBS_NODE_NULL:
            break;
        default: break;
    }
    return count;
}

size_t tbs_node_depth(const tbs_node_t *node) {
    if (!node) return 0;
    size_t max_d = 0;
    size_t d;
    switch (node->type) {
        case TBS_NODE_BINARY_OP:
            if (node->data.binary.left) { d = tbs_node_depth(node->data.binary.left); if (d > max_d) max_d = d; }
            d = tbs_node_depth(node->data.binary.right); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_IF:
            d = tbs_node_depth(node->data.if_stmt.condition); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.if_stmt.if_branch); if (d > max_d) max_d = d;
            if (node->data.if_stmt.else_branch) {
                d = tbs_node_depth(node->data.if_stmt.else_branch); if (d > max_d) max_d = d;
            }
            break;
        case TBS_NODE_WHILE:
            d = tbs_node_depth(node->data.while_loop.condition); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.while_loop.body); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_FOR:
            if (node->data.for_loop.init) { d = tbs_node_depth(node->data.for_loop.init); if (d > max_d) max_d = d; }
            if (node->data.for_loop.condition) { d = tbs_node_depth(node->data.for_loop.condition); if (d > max_d) max_d = d; }
            if (node->data.for_loop.post) { d = tbs_node_depth(node->data.for_loop.post); if (d > max_d) max_d = d; }
            d = tbs_node_depth(node->data.for_loop.body); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                d = tbs_node_depth(node->data.block.statements[i]); if (d > max_d) max_d = d;
            }
            break;
        case TBS_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                d = tbs_node_depth(node->data.function.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case TBS_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                d = tbs_node_depth(node->data.vector.elements[i]); if (d > max_d) max_d = d;
            }
            break;
        case TBS_NODE_INDEX:
            d = tbs_node_depth(node->data.index_access.array); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.index_access.index); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_SLICE:
            d = tbs_node_depth(node->data.slice.array); if (d > max_d) max_d = d;
            if (node->data.slice.start) { d = tbs_node_depth(node->data.slice.start); if (d > max_d) max_d = d; }
            if (node->data.slice.end) { d = tbs_node_depth(node->data.slice.end); if (d > max_d) max_d = d; }
            break;
        case TBS_NODE_FUNCTION_EXPRESSION:
        case TBS_NODE_FUNCTION_DEFINITION:
            for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
                d = tbs_node_depth(node->data.func_def.arg_params[i]); if (d > max_d) max_d = d;
            }
            d = tbs_node_depth(node->data.func_def.body); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_MEMBER_CALL:
            d = tbs_node_depth(node->data.member_call.object); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
                d = tbs_node_depth(node->data.member_call.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case TBS_NODE_INSTANCEOF:
            d = tbs_node_depth(node->data.instanceof_expr.object); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.instanceof_expr.class_expr); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_ASSIGNMENT:
            d = tbs_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_FLOW:
            if (node->data.flow.value) { d = tbs_node_depth(node->data.flow.value); if (d > max_d) max_d = d; }
            break;
        case TBS_NODE_CONSTANT_DECL:
            d = tbs_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_SWITCH:
            d = tbs_node_depth(node->data.switch_stmt.value); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                d = tbs_node_depth(node->data.switch_stmt.cases[i]); if (d > max_d) max_d = d;
            }
            if (node->data.switch_stmt.default_case) { d = tbs_node_depth(node->data.switch_stmt.default_case); if (d > max_d) max_d = d; }
            break;
        case TBS_NODE_DO_WHILE:
            d = tbs_node_depth(node->data.do_while.body); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.do_while.condition); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_MAP_LITERAL:
            for (size_t i = 0; i < node->data.map_literal.count; ++i) {
                d = tbs_node_depth(node->data.map_literal.values[i]); if (d > max_d) max_d = d;
            }
            break;
        case TBS_NODE_MEMBER_ACCESS:
            d = tbs_node_depth(node->data.member_access.object); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_MEMBER_SET:
            d = tbs_node_depth(node->data.member_set.object); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.member_set.value); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_FOR_IN:
            d = tbs_node_depth(node->data.for_in.collection); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.for_in.body); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_SPREAD:
            d = tbs_node_depth(node->data.spread.child); if (d > max_d) max_d = d;
            break;

        case TBS_NODE_DESTRUCTURING_ASSIGNMENT:
            d = tbs_node_depth(node->data.destructuring.targets); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.destructuring.value); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_REST_PARAMETER:
            break;

        case TBS_NODE_TRY_CATCH:
            d = tbs_node_depth(node->data.try_catch.try_body); if (d > max_d) max_d = d;
            d = tbs_node_depth(node->data.try_catch.catch_body); if (d > max_d) max_d = d;
            break;
        case TBS_NODE_THROW:
            if (node->data.throw_stmt.value) { d = tbs_node_depth(node->data.throw_stmt.value); if (d > max_d) max_d = d; }
            break;

        case TBS_NODE_NULL:
            break;
        default: break;
    }
    return 1 + max_d;
}
