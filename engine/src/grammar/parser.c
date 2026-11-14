/**
 * @file parser.c
 * @brief DSL Grammar: Lexer, Parser, and AST nodes
 * Separated from monolithic toad_core.c.
 */

#include "../internal.h"
#include "../../parser/lexer.h"
#include <toad/detail/grammar_gen.h>

void toadParse(void *yyp, int yymajor, toad_token_t yyminor, toad_parse_ctx_t *ctx);
void *toadParseAlloc(void *(*mallocProc)(size_t));
void toadParseFree(void *p, void (*freeProc)(void*));
void toadParseTrace(FILE *TraceFILE, char *zTracePrompt);


// Deep copy an AST node and its children into a new arena
toad_node_t *toad_node_copy(const toad_node_t *src, mem_pool_t *dest_arena) {
    if (!src) return NULL;
    toad_node_t *dst = (toad_node_t*)mem_alloc(dest_arena, sizeof(toad_node_t));
    if (!dst) return NULL;

    memcpy(dst, src, sizeof(toad_node_t));
    dst->arena = dest_arena;

    switch (src->type) {
        case TOAD_NODE_NUMBER:
        case TOAD_NODE_VARIABLE:
            if (src->type == TOAD_NODE_VARIABLE && src->data.variable.name) {
                dst->data.variable.name = mem_strdup(dest_arena, src->data.variable.name);
            }
            break;

        case TOAD_NODE_BINARY_OP:
            if (src->data.binary.left) dst->data.binary.left = toad_node_copy(src->data.binary.left, dest_arena);
            dst->data.binary.right = toad_node_copy(src->data.binary.right, dest_arena);
            break;

        case TOAD_NODE_FUNCTION_CALL:
            if (src->data.function.name) dst->data.function.name = mem_strdup(dest_arena, src->data.function.name);
            if (src->data.function.arg_count > 0) {
                dst->data.function.args = (toad_node_t**)mem_alloc(dest_arena, src->data.function.arg_count * sizeof(toad_node_t*));
                for (size_t i = 0; i < src->data.function.arg_count; ++i) {
                    dst->data.function.args[i] = toad_node_copy(src->data.function.args[i], dest_arena);
                }
            }
            break;

        case TOAD_NODE_CONSTANT_DECL:
        case TOAD_NODE_ASSIGNMENT:
            if (src->data.assignment.name) dst->data.assignment.name = mem_strdup(dest_arena, src->data.assignment.name);
            dst->data.assignment.value = toad_node_copy(src->data.assignment.value, dest_arena);
            break;

        case TOAD_NODE_IF:
            dst->data.if_stmt.condition = toad_node_copy(src->data.if_stmt.condition, dest_arena);
            dst->data.if_stmt.if_branch = toad_node_copy(src->data.if_stmt.if_branch, dest_arena);
            if (src->data.if_stmt.else_branch) dst->data.if_stmt.else_branch = toad_node_copy(src->data.if_stmt.else_branch, dest_arena);
            break;

        case TOAD_NODE_WHILE:
            dst->data.while_loop.condition = toad_node_copy(src->data.while_loop.condition, dest_arena);
            dst->data.while_loop.body = toad_node_copy(src->data.while_loop.body, dest_arena);
            break;

        case TOAD_NODE_FOR:
            if (src->data.for_loop.init) dst->data.for_loop.init = toad_node_copy(src->data.for_loop.init, dest_arena);
            if (src->data.for_loop.condition) dst->data.for_loop.condition = toad_node_copy(src->data.for_loop.condition, dest_arena);
            if (src->data.for_loop.post) dst->data.for_loop.post = toad_node_copy(src->data.for_loop.post, dest_arena);
            dst->data.for_loop.body = toad_node_copy(src->data.for_loop.body, dest_arena);
            break;

        case TOAD_NODE_BLOCK:
            if (src->data.block.count > 0) {
                dst->data.block.statements = (toad_node_t**)mem_alloc(dest_arena, src->data.block.count * sizeof(toad_node_t*));
                for (size_t i = 0; i < src->data.block.count; ++i) {
                    dst->data.block.statements[i] = toad_node_copy(src->data.block.statements[i], dest_arena);
                }
            }
            break;

        case TOAD_NODE_FLOW:
            if (src->data.flow.value) dst->data.flow.value = toad_node_copy(src->data.flow.value, dest_arena);
            break;

        case TOAD_NODE_STRING:
            if (src->data.string.value.data) {
                char *buf = (char*)mem_alloc(dest_arena, src->data.string.value.len + 1);
                memcpy(buf, src->data.string.value.data, src->data.string.value.len);
                buf[src->data.string.value.len] = '\0';
                dst->data.string.value.data = buf;
            }
            break;

        case TOAD_NODE_VECTOR:
            if (src->data.vector.count > 0) {
                dst->data.vector.elements = (toad_node_t**)mem_alloc(dest_arena, src->data.vector.count * sizeof(toad_node_t*));
                for (size_t i = 0; i < src->data.vector.count; ++i) {
                    dst->data.vector.elements[i] = toad_node_copy(src->data.vector.elements[i], dest_arena);
                }
            }
            break;

        case TOAD_NODE_INDEX:
            dst->data.index_access.array = toad_node_copy(src->data.index_access.array, dest_arena);
            dst->data.index_access.index = toad_node_copy(src->data.index_access.index, dest_arena);
            break;

        case TOAD_NODE_SLICE:
            dst->data.slice.array = toad_node_copy(src->data.slice.array, dest_arena);
            if (src->data.slice.start) dst->data.slice.start = toad_node_copy(src->data.slice.start, dest_arena);
            if (src->data.slice.end) dst->data.slice.end = toad_node_copy(src->data.slice.end, dest_arena);
            break;

        case TOAD_NODE_FUNCTION_EXPRESSION:
        case TOAD_NODE_FUNCTION_DEFINITION:
            if (src->data.func_def.name) dst->data.func_def.name = mem_strdup(dest_arena, src->data.func_def.name);
            if (src->data.func_def.arg_count > 0) {
                dst->data.func_def.arg_params = (toad_node_t**)mem_alloc(dest_arena, src->data.func_def.arg_count * sizeof(toad_node_t*));
                for (size_t i = 0; i < src->data.func_def.arg_count; ++i) {
                    dst->data.func_def.arg_params[i] = toad_node_copy(src->data.func_def.arg_params[i], dest_arena);
                }
            }
            dst->data.func_def.body = toad_node_copy(src->data.func_def.body, dest_arena);
            break;

        case TOAD_NODE_MEMBER_CALL:
            dst->data.member_call.object = toad_node_copy(src->data.member_call.object, dest_arena);
            if (src->data.member_call.method)
                dst->data.member_call.method = mem_strdup(dest_arena, src->data.member_call.method);
            if (src->data.member_call.arg_count > 0) {
                dst->data.member_call.args = (toad_node_t**)mem_alloc(dest_arena,
                    src->data.member_call.arg_count * sizeof(toad_node_t*));
                for (size_t i = 0; i < src->data.member_call.arg_count; ++i)
                    dst->data.member_call.args[i] = toad_node_copy(src->data.member_call.args[i], dest_arena);
            }
            break;

        case TOAD_NODE_INSTANCEOF:
            dst->data.instanceof_expr.object =
                toad_node_copy(src->data.instanceof_expr.object, dest_arena);
            if (src->data.instanceof_expr.class_name)
                dst->data.instanceof_expr.class_name =
                    mem_strdup(dest_arena, src->data.instanceof_expr.class_name);
            dst->data.instanceof_expr.class_expr =
                toad_node_copy(src->data.instanceof_expr.class_expr, dest_arena);
            break;

        case TOAD_NODE_SPREAD:
            dst->data.spread.child = toad_node_copy(src->data.spread.child, dest_arena);
            break;

        case TOAD_NODE_DESTRUCTURING_ASSIGNMENT:
            dst->data.destructuring.targets = toad_node_copy(src->data.destructuring.targets, dest_arena);
            dst->data.destructuring.value = toad_node_copy(src->data.destructuring.value, dest_arena);
            dst->data.destructuring.is_constant = src->data.destructuring.is_constant;
            break;

        case TOAD_NODE_REST_PARAMETER:
            /* No extra data to copy besides type and positional info done in entry */
            break;

        case TOAD_NODE_SWITCH:
            dst->data.switch_stmt.value = toad_node_copy(src->data.switch_stmt.value, dest_arena);
            if (src->data.switch_stmt.case_count > 0) {
                dst->data.switch_stmt.cases = (toad_node_t**)mem_alloc(dest_arena, src->data.switch_stmt.case_count * 2 * sizeof(toad_node_t*));
                for (size_t i = 0; i < src->data.switch_stmt.case_count * 2; ++i) {
                    dst->data.switch_stmt.cases[i] = toad_node_copy(src->data.switch_stmt.cases[i], dest_arena);
                }
            }
            if (src->data.switch_stmt.default_case) {
                dst->data.switch_stmt.default_case = toad_node_copy(src->data.switch_stmt.default_case, dest_arena);
            }
            break;

        case TOAD_NODE_DO_WHILE:
            dst->data.do_while.body = toad_node_copy(src->data.do_while.body, dest_arena);
            dst->data.do_while.condition = toad_node_copy(src->data.do_while.condition, dest_arena);
            break;

        case TOAD_NODE_MAP_LITERAL:
            if (src->data.map_literal.count > 0) {
                dst->data.map_literal.keys = (char**)mem_alloc(dest_arena, src->data.map_literal.count * sizeof(char*));
                dst->data.map_literal.values = (toad_node_t**)mem_alloc(dest_arena, src->data.map_literal.count * sizeof(toad_node_t*));
                for (size_t i = 0; i < src->data.map_literal.count; ++i) {
                    dst->data.map_literal.keys[i] = mem_strdup(dest_arena, src->data.map_literal.keys[i]);
                    dst->data.map_literal.values[i] = toad_node_copy(src->data.map_literal.values[i], dest_arena);
                }
            }
            break;

        case TOAD_NODE_MEMBER_ACCESS:
            dst->data.member_access.object = toad_node_copy(src->data.member_access.object, dest_arena);
            if (src->data.member_access.member)
                dst->data.member_access.member = mem_strdup(dest_arena, src->data.member_access.member);
            break;

        case TOAD_NODE_MEMBER_SET:
            dst->data.member_set.object = toad_node_copy(src->data.member_set.object, dest_arena);
            if (src->data.member_set.member)
                dst->data.member_set.member = mem_strdup(dest_arena, src->data.member_set.member);
            dst->data.member_set.value = toad_node_copy(src->data.member_set.value, dest_arena);
            break;

        case TOAD_NODE_FOR_IN:
            if (src->data.for_in.var_name)
                dst->data.for_in.var_name = mem_strdup(dest_arena, src->data.for_in.var_name);
            dst->data.for_in.collection = toad_node_copy(src->data.for_in.collection, dest_arena);
            dst->data.for_in.body = toad_node_copy(src->data.for_in.body, dest_arena);
            break;

        case TOAD_NODE_NULL:
            /* No data to copy */
            break;

        case TOAD_NODE_TEMPLATE_STRING:
            if (src->data.template_string.template_str) {
                dst->data.template_string.template_str = mem_strdup(dest_arena, src->data.template_string.template_str);
            }
            dst->data.template_string.len = src->data.template_string.len;
            break;

        case TOAD_NODE_TRY_CATCH:
            dst->data.try_catch.try_body = toad_node_copy(src->data.try_catch.try_body, dest_arena);
            if (src->data.try_catch.catch_var)
                dst->data.try_catch.catch_var = mem_strdup(dest_arena, src->data.try_catch.catch_var);
            dst->data.try_catch.catch_body = toad_node_copy(src->data.try_catch.catch_body, dest_arena);
            break;

        case TOAD_NODE_THROW:
            dst->data.throw_stmt.value = toad_node_copy(src->data.throw_stmt.value, dest_arena);
            break;
    }
    return dst;
}

// Create a new node using the arena
toad_node_t *toad_node_create(mem_pool_t *arena, toad_node_type_t type) {
    toad_node_t *n = MEM_ALLOC(arena, toad_node_t);
    if (n) {
        memset(n, 0, sizeof(toad_node_t));
        n->type = type;
        n->arena = arena;
        n->line = 0;
        n->column = 0;
    }
    return n;
}

toad_node_t *toad_fold_binary(toad_parse_ctx_t *ctx, int op, toad_node_t *left, toad_node_t *right) {
    if (!left || !right) return NULL;

    if (left->type == TOAD_NODE_NUMBER && right->type == TOAD_NODE_NUMBER) {
        double l = left->data.number;
        double r = right->data.number;
        double res = 0;
        int folded = 1;

        switch (op) {
            case toad_TOKEN_PLUS: res = l + r; break;
            case toad_TOKEN_MINUS: res = l - r; break;
            case toad_TOKEN_MULTIPLY: res = l * r; break;
            case toad_TOKEN_DIVIDE: if (r != 0) res = l / r; else folded = 0; break;
            case toad_TOKEN_MOD: if (r != 0) res = fmod(l, r); else folded = 0; break;
            case toad_TOKEN_POWER: res = pow(l, r); break;
            case toad_TOKEN_EQ: res = (l == r); break;
            case toad_TOKEN_NE: res = (l != r); break;
            case toad_TOKEN_LT: res = (l < r); break;
            case toad_TOKEN_LE: res = (l <= r); break;
            case toad_TOKEN_GT: res = (l > r); break;
            case toad_TOKEN_GE: res = (l >= r); break;
            case toad_TOKEN_AND: res = (l && r); break;
            case toad_TOKEN_OR: res = (l || r); break;
            default: folded = 0; break;
        }

        if (folded) {
            toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_NUMBER);
            if (n) {
                n->data.number = res;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }

    // String concatenation folding
    if (op == toad_TOKEN_PLUS && left->type == TOAD_NODE_STRING && right->type == TOAD_NODE_STRING) {
        size_t new_len = left->data.string.value.len + right->data.string.value.len;
        char *buf = (char*)mem_alloc(ctx->arena, new_len + 1);
        if (buf) {
            memcpy(buf, left->data.string.value.data, left->data.string.value.len);
            memcpy(buf + left->data.string.value.len, right->data.string.value.data, right->data.string.value.len);
            buf[new_len] = '\0';
            toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_STRING);
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
    if (op == toad_TOKEN_PLUS &&
        ((left->type == TOAD_NODE_STRING && right->type == TOAD_NODE_NUMBER) ||
         (left->type == TOAD_NODE_NUMBER && right->type == TOAD_NODE_STRING))) {
        char n_buf[32];
        const char *l_data, *r_data;
        size_t l_len, r_len;

        if (left->type == TOAD_NODE_NUMBER) {
            l_len = snprintf(n_buf, sizeof(n_buf), "%g", left->data.number);
            l_data = n_buf;
        } else {
            l_data = left->data.string.value.data;
            l_len = left->data.string.value.len;
        }

        char r_buf[32];
        if (right->type == TOAD_NODE_NUMBER) {
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
            toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_STRING);
            if (n) {
                n->data.string.value.data = buf;
                n->data.string.value.len = new_len;
                n->line = left->line;
                n->column = left->column;
            }
            return n;
        }
    }

    toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = left;
    n->data.binary.right = right;
    return n;
}

toad_node_t *toad_fold_unary(toad_parse_ctx_t *ctx, int op, toad_node_t *child) {
    if (!child) return NULL;
    if (child->type == TOAD_NODE_NUMBER) {
        double v = child->data.number;
        if (op == toad_TOKEN_MINUS) {
            toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_NUMBER);
            n->data.number = -v;
            return n;
        }
        if (op == toad_TOKEN_PLUS) return child;
        if (op == toad_TOKEN_NOT) {
            toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_NUMBER);
            n->data.number = (fabs(v) < 1e-9 ? 1.0 : 0.0);
            return n;
        }
    }

    toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_BINARY_OP);
    n->data.binary.op = op;
    n->data.binary.left = NULL;
    n->data.binary.right = child;
    return n;
}

toad_node_t *toad_fold_if(toad_parse_ctx_t *ctx, toad_node_t *cond, toad_node_t *if_branch, toad_node_t *else_branch) {
    if (cond->type == TOAD_NODE_NUMBER) {
        double v = cond->data.number;
        if (fabs(v) > 1e-9) {
            return if_branch;
        } else {
            return else_branch ? else_branch : toad_node_create(ctx->arena, TOAD_NODE_BLOCK);
        }
    }
    toad_node_t *n = toad_node_create(ctx->arena, TOAD_NODE_IF);
    n->data.if_stmt.condition = cond;
    n->data.if_stmt.if_branch = if_branch;
    n->data.if_stmt.else_branch = else_branch;
    return n;
}

toad_node_t *toad_parse_ext(const char *input, size_t length,
                                 mem_pool_t *arena, int *error,
                                 char *error_msg, size_t error_msg_len) {
    if (!input) return NULL;
    if (length == 0) length = strlen(input);

    toad_lexer_t lexer;
    toad_lexer_init(&lexer, input, length);

    toad_parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena = arena;

    void *parser = toadParseAlloc(malloc);
    if (!parser) {
        if (error) *error = 1;
        return NULL;
    }

    toad_token_t token;
    memset(&token, 0, sizeof(token));
    int ret;
    while ((ret = toad_lexer_next(&lexer, &token)) > 0) {
        toadParse(parser, ret, token, &ctx);
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
        toad_token_t end_token;
        memset(&end_token, 0, sizeof(end_token));
        toadParse(parser, 0, end_token, &ctx);
    }

    toadParseFree(parser, free);

    if (error) *error = ctx.error;
    if (ctx.error && error_msg && error_msg_len > 0) {
        strncpy(error_msg, ctx.error_msg, error_msg_len - 1);
        error_msg[error_msg_len - 1] = '\0';
    }

    return ctx.error ? NULL : ctx.root;
}

toad_node_t *toad_parse(const char *input, size_t length) {
    mem_pool_t *arena = (mem_pool_t*)malloc(sizeof(mem_pool_t));
    if (!arena || mem_init(arena, 4096) != 0) {
        if (arena) free(arena);
        return NULL;
    }
    int err = 0;
    toad_node_t *root = toad_parse_ext(input, length, arena, &err, NULL, 0);
    if (err || !root) {
        mem_destroy(arena);
        free(arena);
        return NULL;
    }
    return root;
}

void toad_free(toad_node_t *node) {
    if (!node || !node->arena) return;
    mem_pool_t *arena = node->arena;
    mem_destroy(arena);
    free(arena);
}

size_t toad_node_count(const toad_node_t *node) {
    if (!node) return 0;
    size_t count = 1;
    switch (node->type) {
        case TOAD_NODE_BINARY_OP:
            if (node->data.binary.left) count += toad_node_count(node->data.binary.left);
            count += toad_node_count(node->data.binary.right);
            break;
        case TOAD_NODE_IF:
            count += toad_node_count(node->data.if_stmt.condition);
            count += toad_node_count(node->data.if_stmt.if_branch);
            if (node->data.if_stmt.else_branch) count += toad_node_count(node->data.if_stmt.else_branch);
            break;
        case TOAD_NODE_WHILE:
            count += toad_node_count(node->data.while_loop.condition);
            count += toad_node_count(node->data.while_loop.body);
            break;
        case TOAD_NODE_FOR:
            if (node->data.for_loop.init) count += toad_node_count(node->data.for_loop.init);
            if (node->data.for_loop.condition) count += toad_node_count(node->data.for_loop.condition);
            if (node->data.for_loop.post) count += toad_node_count(node->data.for_loop.post);
            count += toad_node_count(node->data.for_loop.body);
            break;
        case TOAD_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                count += toad_node_count(node->data.block.statements[i]);
            }
            break;
        case TOAD_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                count += toad_node_count(node->data.function.args[i]);
            }
            break;
        case TOAD_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                count += toad_node_count(node->data.vector.elements[i]);
            }
            break;
        case TOAD_NODE_INDEX:
            count += toad_node_count(node->data.index_access.array);
            count += toad_node_count(node->data.index_access.index);
            break;
        case TOAD_NODE_SLICE:
            count += toad_node_count(node->data.slice.array);
            if (node->data.slice.start) count += toad_node_count(node->data.slice.start);
            if (node->data.slice.end) count += toad_node_count(node->data.slice.end);
            break;
        case TOAD_NODE_FUNCTION_EXPRESSION:
        case TOAD_NODE_FUNCTION_DEFINITION:
            for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
                count += toad_node_count(node->data.func_def.arg_params[i]);
            }
            count += toad_node_count(node->data.func_def.body);
            break;
        case TOAD_NODE_MEMBER_CALL:
            count += toad_node_count(node->data.member_call.object);
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i)
                count += toad_node_count(node->data.member_call.args[i]);
            break;
        case TOAD_NODE_ASSIGNMENT:
            count += toad_node_count(node->data.assignment.value);
            break;
        case TOAD_NODE_FLOW:
            if (node->data.flow.value) count += toad_node_count(node->data.flow.value);
            break;
        case TOAD_NODE_CONSTANT_DECL:
            count += toad_node_count(node->data.assignment.value);
            break;
        case TOAD_NODE_SWITCH:
            count += toad_node_count(node->data.switch_stmt.value);
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                count += toad_node_count(node->data.switch_stmt.cases[i]);
            }
            if (node->data.switch_stmt.default_case) count += toad_node_count(node->data.switch_stmt.default_case);
            break;
        case TOAD_NODE_DO_WHILE:
            count += toad_node_count(node->data.do_while.body);
            count += toad_node_count(node->data.do_while.condition);
            break;
        case TOAD_NODE_MAP_LITERAL:
            for (size_t i = 0; i < node->data.map_literal.count; ++i)
                count += toad_node_count(node->data.map_literal.values[i]);
            break;
        case TOAD_NODE_MEMBER_ACCESS:
            count += toad_node_count(node->data.member_access.object);
            break;
        case TOAD_NODE_MEMBER_SET:
            count += toad_node_count(node->data.member_set.object);
            count += toad_node_count(node->data.member_set.value);
            break;
        case TOAD_NODE_FOR_IN:
            count += toad_node_count(node->data.for_in.collection);
            count += toad_node_count(node->data.for_in.body);
            break;
        case TOAD_NODE_SPREAD:
            count += toad_node_count(node->data.spread.child);
            break;

        case TOAD_NODE_DESTRUCTURING_ASSIGNMENT:
            count += toad_node_count(node->data.destructuring.targets);
            count += toad_node_count(node->data.destructuring.value);
            break;
        case TOAD_NODE_REST_PARAMETER:
            break;

        case TOAD_NODE_TRY_CATCH:
            count += toad_node_count(node->data.try_catch.try_body);
            count += toad_node_count(node->data.try_catch.catch_body);
            break;
        case TOAD_NODE_THROW:
            if (node->data.throw_stmt.value) count += toad_node_count(node->data.throw_stmt.value);
            break;

        case TOAD_NODE_NULL:
            break;
        default: break;
    }
    return count;
}

size_t toad_node_depth(const toad_node_t *node) {
    if (!node) return 0;
    size_t max_d = 0;
    size_t d;
    switch (node->type) {
        case TOAD_NODE_BINARY_OP:
            if (node->data.binary.left) { d = toad_node_depth(node->data.binary.left); if (d > max_d) max_d = d; }
            d = toad_node_depth(node->data.binary.right); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_IF:
            d = toad_node_depth(node->data.if_stmt.condition); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.if_stmt.if_branch); if (d > max_d) max_d = d;
            if (node->data.if_stmt.else_branch) {
                d = toad_node_depth(node->data.if_stmt.else_branch); if (d > max_d) max_d = d;
            }
            break;
        case TOAD_NODE_WHILE:
            d = toad_node_depth(node->data.while_loop.condition); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.while_loop.body); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_FOR:
            if (node->data.for_loop.init) { d = toad_node_depth(node->data.for_loop.init); if (d > max_d) max_d = d; }
            if (node->data.for_loop.condition) { d = toad_node_depth(node->data.for_loop.condition); if (d > max_d) max_d = d; }
            if (node->data.for_loop.post) { d = toad_node_depth(node->data.for_loop.post); if (d > max_d) max_d = d; }
            d = toad_node_depth(node->data.for_loop.body); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_BLOCK:
            for (size_t i = 0; i < node->data.block.count; ++i) {
                d = toad_node_depth(node->data.block.statements[i]); if (d > max_d) max_d = d;
            }
            break;
        case TOAD_NODE_FUNCTION_CALL:
            for (size_t i = 0; i < node->data.function.arg_count; ++i) {
                d = toad_node_depth(node->data.function.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case TOAD_NODE_VECTOR:
            for (size_t i = 0; i < node->data.vector.count; ++i) {
                d = toad_node_depth(node->data.vector.elements[i]); if (d > max_d) max_d = d;
            }
            break;
        case TOAD_NODE_INDEX:
            d = toad_node_depth(node->data.index_access.array); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.index_access.index); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_SLICE:
            d = toad_node_depth(node->data.slice.array); if (d > max_d) max_d = d;
            if (node->data.slice.start) { d = toad_node_depth(node->data.slice.start); if (d > max_d) max_d = d; }
            if (node->data.slice.end) { d = toad_node_depth(node->data.slice.end); if (d > max_d) max_d = d; }
            break;
        case TOAD_NODE_FUNCTION_EXPRESSION:
        case TOAD_NODE_FUNCTION_DEFINITION:
            for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
                d = toad_node_depth(node->data.func_def.arg_params[i]); if (d > max_d) max_d = d;
            }
            d = toad_node_depth(node->data.func_def.body); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_MEMBER_CALL:
            d = toad_node_depth(node->data.member_call.object); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
                d = toad_node_depth(node->data.member_call.args[i]); if (d > max_d) max_d = d;
            }
            break;
        case TOAD_NODE_INSTANCEOF:
            d = toad_node_depth(node->data.instanceof_expr.object); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.instanceof_expr.class_expr); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_ASSIGNMENT:
            d = toad_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_FLOW:
            if (node->data.flow.value) { d = toad_node_depth(node->data.flow.value); if (d > max_d) max_d = d; }
            break;
        case TOAD_NODE_CONSTANT_DECL:
            d = toad_node_depth(node->data.assignment.value); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_SWITCH:
            d = toad_node_depth(node->data.switch_stmt.value); if (d > max_d) max_d = d;
            for (size_t i = 0; i < node->data.switch_stmt.case_count * 2; ++i) {
                d = toad_node_depth(node->data.switch_stmt.cases[i]); if (d > max_d) max_d = d;
            }
            if (node->data.switch_stmt.default_case) { d = toad_node_depth(node->data.switch_stmt.default_case); if (d > max_d) max_d = d; }
            break;
        case TOAD_NODE_DO_WHILE:
            d = toad_node_depth(node->data.do_while.body); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.do_while.condition); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_MAP_LITERAL:
            for (size_t i = 0; i < node->data.map_literal.count; ++i) {
                d = toad_node_depth(node->data.map_literal.values[i]); if (d > max_d) max_d = d;
            }
            break;
        case TOAD_NODE_MEMBER_ACCESS:
            d = toad_node_depth(node->data.member_access.object); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_MEMBER_SET:
            d = toad_node_depth(node->data.member_set.object); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.member_set.value); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_FOR_IN:
            d = toad_node_depth(node->data.for_in.collection); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.for_in.body); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_SPREAD:
            d = toad_node_depth(node->data.spread.child); if (d > max_d) max_d = d;
            break;

        case TOAD_NODE_DESTRUCTURING_ASSIGNMENT:
            d = toad_node_depth(node->data.destructuring.targets); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.destructuring.value); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_REST_PARAMETER:
            break;

        case TOAD_NODE_TRY_CATCH:
            d = toad_node_depth(node->data.try_catch.try_body); if (d > max_d) max_d = d;
            d = toad_node_depth(node->data.try_catch.catch_body); if (d > max_d) max_d = d;
            break;
        case TOAD_NODE_THROW:
            if (node->data.throw_stmt.value) { d = toad_node_depth(node->data.throw_stmt.value); if (d > max_d) max_d = d; }
            break;

        case TOAD_NODE_NULL:
            break;
        default: break;
    }
    return 1 + max_d;
}
