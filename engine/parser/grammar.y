/**
 * @file grammar.y
 * @brief engine-like Expression Parser Grammar (Lemon)
 */

%name tbsParse
%token_prefix tbs_TOKEN_
%token_type {tbs_token_t}
%default_type {tbs_node_t*}
%stack_size 256

%extra_argument {tbs_parse_ctx_t *ctx}

%include {
#include "lexer.h"
#include <turboscript/engine.h>
#include <turboscript/types.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

static tbs_node_t *tbs_node_new(tbs_parse_ctx_t *ctx, tbs_node_type_t type) {
    return tbs_node_create(ctx->arena, type);
}

static void tbs_node_set_pos(tbs_node_t *node, const tbs_token_t *token) {
    if (node && token) {
        node->line = token->line;
        node->column = token->column;
    }
}

static void tbs_node_copy_pos(tbs_node_t *dst, const tbs_node_t *src) {
    if (dst && src) {
        dst->line = src->line;
        dst->column = src->column;
    }
}

static char *tbs_strdup(tbs_parse_ctx_t *ctx, const char *s, size_t n) {
    char *d = (char*)mem_alloc(ctx->arena, n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static char *tbs_prefixed_name(tbs_parse_ctx_t *ctx, const char *prefix,
                                  size_t prefix_len, const tbs_token_t *name) {
    size_t len;
    char *d;
    if (!ctx || !prefix || !name) return NULL;
    len = prefix_len + 1 + name->length;
    d = (char*)mem_alloc(ctx->arena, len + 1);
    if (!d) return NULL;
    memcpy(d, prefix, prefix_len);
    d[prefix_len] = '.';
    memcpy(d + prefix_len + 1, name->start, name->length);
    d[len] = '\0';
    return d;
}

static vstr tbs_unescape_to_arena(tbs_parse_ctx_t *ctx, const char *s, size_t n) {
    char *d = (char*)mem_alloc(ctx->arena, n + 1);
    size_t len = 0;
    if (!d) return vstr_from_buf(NULL, 0);
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            switch (s[i]) {
                case 'n': d[len++] = '\n'; break;
                case 'r': d[len++] = '\r'; break;
                case 't': d[len++] = '\t'; break;
                case '\\': d[len++] = '\\'; break;
                case '"': d[len++] = '"'; break;
                case '0': d[len++] = '\0'; break;
                default: d[len++] = s[i]; break;
            }
        } else {
            d[len++] = s[i];
        }
    }
    d[len] = '\0';
    return vstr_from_buf(d, len);
}

static void tbs_record_parse_error(tbs_parse_ctx_t *ctx, int fatal, const char *fmt, ...) {
    va_list args;

    if (!ctx) return;

    ctx->error = 1;
    if (fatal) ctx->fatal_error = 1;
    if (ctx->error_msg[0] != '\0') return;

    va_start(args, fmt);
    vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, args);
    va_end(args);
}

static tbs_node_t *tbs_make_empty_block(tbs_parse_ctx_t *ctx, const tbs_token_t *token) {
    tbs_node_t *block = tbs_node_new(ctx, TBS_NODE_BLOCK);
    if (!block) return NULL;
    block->data.block.count = 0;
    block->data.block.statements = NULL;
    tbs_node_set_pos(block, token);
    return block;
}

tbs_node_t *tbs_fold_binary(tbs_parse_ctx_t *ctx, int op, tbs_node_t *left, tbs_node_t *right);
tbs_node_t *tbs_fold_unary(tbs_parse_ctx_t *ctx, int op, tbs_node_t *child);
tbs_node_t *tbs_fold_if(tbs_parse_ctx_t *ctx, tbs_node_t *cond, tbs_node_t *if_branch, tbs_node_t *else_branch);

static int tbs_is_valid_param_node(const tbs_node_t *node) {
    if (!node) return 0;
    return node->type == TBS_NODE_VARIABLE ||
           node->type == TBS_NODE_VECTOR ||
           node->type == TBS_NODE_MAP_LITERAL ||
           node->type == TBS_NODE_SPREAD ||
           node->type == TBS_NODE_ASSIGNMENT;
}

static int tbs_validate_param_list(tbs_parse_ctx_t *ctx, tbs_node_t **params,
                                      size_t count, const char *error_msg) {
    for (size_t i = 0; i < count; ++i) {
        tbs_node_t *param = params[i];
        if (!tbs_is_valid_param_node(param)) {
            tbs_record_parse_error(ctx, 1, "%s", error_msg);
            return 0;
        }
        if (param->type == TBS_NODE_SPREAD && i + 1 != count) {
            tbs_record_parse_error(ctx, 1, "%s", error_msg);
            return 0;
        }
    }
    return 1;
}

static tbs_node_t *tbs_wrap_implicit_return(tbs_parse_ctx_t *ctx, tbs_node_t *body,
                                                  int flow_type) {
    if (!body || body->type == TBS_NODE_BLOCK) return body;

    tbs_node_t *ret = tbs_node_new(ctx, TBS_NODE_FLOW);
    tbs_node_t *blk = tbs_node_new(ctx, TBS_NODE_BLOCK);
    if (!ret || !blk) return NULL;

    ret->data.flow.type = flow_type;
    ret->data.flow.value = body;
    tbs_node_copy_pos(ret, body);

    blk->data.block.count = 1;
    blk->data.block.statements = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
    if (!blk->data.block.statements) return NULL;
    blk->data.block.statements[0] = ret;
    tbs_node_copy_pos(blk, body);
    return blk;
}

static tbs_node_t *tbs_make_function_node(tbs_parse_ctx_t *ctx, tbs_node_type_t type,
                                                char *name, tbs_node_t **params,
                                                size_t param_count, tbs_node_t *body,
                                                const tbs_token_t *token,
                                                int wrap_implicit_return,
                                                int implicit_return_flow_type) {
    tbs_node_t *node = tbs_node_new(ctx, type);
    if (!node) return NULL;

    node->data.func_def.name = name;
    node->data.func_def.arg_count = param_count;
    node->data.func_def.arg_params = NULL;
    if (param_count > 0) {
        node->data.func_def.arg_params =
            (tbs_node_t**)mem_alloc(ctx->arena, param_count * sizeof(tbs_node_t*));
        if (!node->data.func_def.arg_params) return NULL;
        for (size_t i = 0; i < param_count; ++i) {
            node->data.func_def.arg_params[i] = params[i];
        }
    }

    node->data.func_def.body =
        wrap_implicit_return ? tbs_wrap_implicit_return(ctx, body, implicit_return_flow_type)
                             : body;
    if (!node->data.func_def.body) return NULL;
    tbs_node_set_pos(node, token);
    return node;
}

static tbs_node_t *tbs_make_unary_call(tbs_parse_ctx_t *ctx, const char *name,
                                             tbs_node_t *arg,
                                             const tbs_token_t *token) {
    tbs_node_t *node = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (!node || !name || !arg) return NULL;

    node->data.function.name = tbs_strdup(ctx, name, strlen(name));
    node->data.function.arg_count = 1;
    node->data.function.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
    if (!node->data.function.args) return NULL;
    node->data.function.args[0] = arg;
    tbs_node_set_pos(node, token);
    return node;
}

static tbs_node_t *tbs_make_raw_string_node(tbs_parse_ctx_t *ctx, const char *data,
                                                  size_t len, const tbs_token_t *token) {
    tbs_node_t *node = tbs_node_new(ctx, TBS_NODE_STRING);
    char *buf;
    if (!node) return NULL;
    buf = (char*)mem_alloc(ctx->arena, len + 1);
    if (!buf) return NULL;
    if (len > 0 && data) memcpy(buf, data, len);
    buf[len] = '\0';
    node->data.string.value = vstr_from_buf(buf, len);
    tbs_node_set_pos(node, token);
    return node;
}

static tbs_node_t *tbs_make_regex_literal(tbs_parse_ctx_t *ctx,
                                                const tbs_token_t *token) {
    const char *p;
    const char *body_start;
    const char *body_end = NULL;
    char *pattern = NULL;
    size_t pattern_len = 0;
    size_t pattern_cap;
    tbs_node_t *call;
    tbs_node_t *pattern_arg;
    tbs_node_t *flags_arg;
    size_t argc = 1;
    int in_class = 0;

    if (!ctx || !token || !token->start || token->length < 2) return NULL;

    body_start = token->start + 1;
    p = body_start;
    pattern_cap = token->length + 1;
    pattern = (char*)mem_alloc(ctx->arena, pattern_cap);
    if (!pattern) return NULL;

    while (p < token->start + token->length) {
        char c = *p;
        if (c == '\\' && p + 1 < token->start + token->length) {
            if (p[1] == '/') {
                pattern[pattern_len++] = '/';
                p += 2;
                continue;
            }
            pattern[pattern_len++] = *p++;
            pattern[pattern_len++] = *p++;
            continue;
        }
        if (c == '[') {
            in_class = 1;
            pattern[pattern_len++] = *p++;
            continue;
        }
        if (c == ']' && in_class) {
            in_class = 0;
            pattern[pattern_len++] = *p++;
            continue;
        }
        if (c == '/' && !in_class) {
            body_end = p;
            ++p;
            break;
        }
        pattern[pattern_len++] = *p++;
    }
    if (!body_end) {
        tbs_record_parse_error(ctx, 1, "Invalid regex literal");
        return NULL;
    }
    pattern[pattern_len] = '\0';

    pattern_arg = tbs_make_raw_string_node(ctx, pattern, pattern_len, token);
    if (!pattern_arg) return NULL;

    call = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (!call) return NULL;
    call->data.function.name = tbs_strdup(ctx, "RegExp", 6);

    if ((size_t)(token->start + token->length - p) > 0) argc = 2;
    call->data.function.arg_count = argc;
    call->data.function.args =
        (tbs_node_t**)mem_alloc(ctx->arena, argc * sizeof(tbs_node_t*));
    if (!call->data.function.args) return NULL;
    call->data.function.args[0] = pattern_arg;

    if (argc == 2) {
        flags_arg = tbs_make_raw_string_node(ctx, p,
            (size_t)(token->start + token->length - p), token);
        if (!flags_arg) return NULL;
        call->data.function.args[1] = flags_arg;
    }

    tbs_node_set_pos(call, token);
    return call;
}

static tbs_node_t *tbs_copy_variable(tbs_parse_ctx_t *ctx, const tbs_node_t *node) {
    tbs_node_t *copy = tbs_node_new(ctx, TBS_NODE_VARIABLE);
    if (!copy || !node || !node->data.variable.name) return NULL;
    copy->data.variable.name =
        tbs_strdup(ctx, node->data.variable.name, strlen(node->data.variable.name));
    tbs_node_copy_pos(copy, node);
    return copy;
}

static tbs_node_t *tbs_make_type_name(tbs_parse_ctx_t *ctx, const char *name,
                                            size_t len, const tbs_token_t *token) {
    tbs_node_t *node = tbs_node_new(ctx, TBS_NODE_VARIABLE);
    if (!node || !name) return NULL;
    node->data.variable.name = tbs_strdup(ctx, name, len);
    tbs_node_set_pos(node, token);
    return node;
}

static tbs_node_t *tbs_make_compound_assign(tbs_parse_ctx_t *ctx, tbs_node_t *target,
                                                  int op, tbs_node_t *value,
                                                  const tbs_token_t *token) {
    tbs_node_t *expr = NULL;
    tbs_node_t *assign = NULL;

    if (!target || target->type != TBS_NODE_VARIABLE) {
        tbs_record_parse_error(ctx, 1, "Invalid assignment target");
        return NULL;
    }

    expr = tbs_node_new(ctx, TBS_NODE_BINARY_OP);
    if (!expr) return NULL;
    expr->data.binary.op = op;
    expr->data.binary.left = tbs_copy_variable(ctx, target);
    expr->data.binary.right = value;
    tbs_node_set_pos(expr, token);

    assign = tbs_node_new(ctx, TBS_NODE_ASSIGNMENT);
    if (!assign) return NULL;
    assign->data.assignment.name =
        tbs_strdup(ctx, target->data.variable.name, strlen(target->data.variable.name));
    assign->data.assignment.value = expr;
    tbs_node_set_pos(assign, token);
    return assign;
}

static void tbs_init_class_def(tbs_node_t *node) {
    if (!node) return;
    node->data.class_def.name = NULL;
    node->data.class_def.parent_name = NULL;
    node->data.class_def.interface_names = NULL;
    node->data.class_def.interface_count = 0;
    node->data.class_def.is_abstract = 0;
    node->data.class_def.is_interface = 0;
    node->data.class_def.is_final = 0;
}

static void tbs_set_class_interfaces(tbs_parse_ctx_t *ctx,
                                        tbs_node_t *node,
                                        tbs_node_t *interfaces) {
    if (!ctx || !node || !interfaces || interfaces->type != TBS_NODE_VECTOR) return;
    size_t count = interfaces->data.vector.count;
    node->data.class_def.interface_names = NULL;
    node->data.class_def.interface_count = 0;
    if (count == 0) return;

    char **names = (char **)mem_alloc(ctx->arena, count * sizeof(char *));
    if (!names) return;
    for (size_t i = 0; i < count; ++i) {
        tbs_node_t *entry = interfaces->data.vector.elements[i];
        names[i] = (entry && entry->type == TBS_NODE_VARIABLE)
            ? entry->data.variable.name
            : NULL;
    }
    node->data.class_def.interface_names = names;
    node->data.class_def.interface_count = count;
}

}

%token SPREAD.
%token ASYNC.
%token AWAIT.
%token REGEX.
// TODO: 协程语法暂时禁用，存在 16 个解析冲突
// %token YIELD.
// %token FUNC_GENERATOR.

// OOP tokens
%token CLASS.
%token CONSTRUCTOR.
%token THIS.
%token STATIC.
%token EXTENDS.
%token IMPLEMENTS.
%token INTERFACE.
%token SUPER.
%token NEW.
%token INSTANCEOF.
%token ABSTRACT.
%token OVERRIDE.
%token FINAL.
%token PUBLIC.
%token PROTECTED.
%token PRIVATE.

// Precedence (lowest to highest)
%left SEMICOLON.
%left RETURN BREAK CONTINUE FUNC MAP TRY CATCH THROW. // Low precedence for flow control
%right EQUAL ASSIGN_ADD ASSIGN_SUB ASSIGN_MUL ASSIGN_DIV.
%right ARROW.
%right QUESTION. // Ternary operator
%nonassoc LOWER_THAN_ELSE.
%nonassoc ELSE ELIF.
%left OR.
%left PIPE. // Pipe operator |>
%left AND.
%left EQ NE.
%left INSTANCEOF. // instanceof operator (same level as equality)
%left LT LE GT GE.
%left PLUS MINUS.
%left MULTIPLY DIVIDE MOD.
%right POWER.
%right NOT AWAIT.
%left MEMBER_PREC. // Between NOT and LPAREN: lets MEMBER_ACCESS reduce for operators but shift for LPAREN
%left LPAREN LBRACKET DOT QUESTION_DOT. // High precedence for function calls, indexing, and member access

%start_symbol start

// Start Symbol
start ::= block_content(E). {
    ctx->root = E;
}

// Block Content (Handles optional trailing semicolon and empty blocks)
block_content(A) ::= . {
    A = tbs_node_new(ctx, TBS_NODE_BLOCK);
    A->data.block.count = 0;
    A->data.block.statements = NULL;
}

block_content(A) ::= stmts(S). {
    A = S;
}

// Statements and Blocks
stmt(A) ::= expr(E) SEMICOLON. { A = E; }
stmt(A) ::= expr(E). [LOWER_THAN_ELSE] { A = E; }
stmt(A) ::= SEMICOLON(OP). {
    A = tbs_make_empty_block(ctx, &OP);
}
stmt(A) ::= error SEMICOLON(OP). {
    A = tbs_make_empty_block(ctx, &OP);
}

block(A) ::= LBRACE block_content(B) RBRACE. {
    A = B;
}

block(A) ::= LBRACE error RBRACE(OP). {
    A = tbs_make_empty_block(ctx, &OP);
}

stmts(A) ::= stmt(S). {
    A = tbs_node_new(ctx, TBS_NODE_BLOCK);
    if (A) {
        if (S->type == TBS_NODE_BLOCK && S->data.block.count == 0) {
            A->data.block.count = 0;
            A->data.block.statements = NULL;
        } else {
            A->data.block.count = 1;
            A->data.block.statements = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
            A->data.block.statements[0] = S;
        }
        tbs_node_copy_pos(A, S);
    }
}

stmts(A) ::= stmts(L) stmt(R). {
    A = L;
    if (R->type == TBS_NODE_BLOCK && R->data.block.count == 0) {
        // Skip empty statement
    } else {
        A->data.block.count++;
        tbs_node_t **new_stmts = (tbs_node_t**)mem_alloc(ctx->arena, A->data.block.count * sizeof(tbs_node_t*));
        memcpy(new_stmts, L->data.block.statements, (A->data.block.count-1) * sizeof(tbs_node_t*));
        new_stmts[A->data.block.count-1] = R;
        A->data.block.statements = new_stmts;
    }
}

// Control Flow
expr(A) ::= block(B). {
    A = B;
}

expr(A) ::= IF(OP) LPAREN expr(C) RPAREN expr(T) ELSE expr(E). {
    A = tbs_fold_if(ctx, C, T, E);
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= IF(OP) LPAREN expr(C) RPAREN expr(T). [LOWER_THAN_ELSE] {
    A = tbs_fold_if(ctx, C, T, NULL);
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= IF(OP) LPAREN expr(C) RPAREN expr(T) elif_clause(E). {
    A = tbs_fold_if(ctx, C, T, E);
    tbs_node_set_pos(A, &OP);
}

elif_clause(A) ::= ELIF(OP) LPAREN expr(C) RPAREN expr(T). {
    A = tbs_fold_if(ctx, C, T, NULL);
    tbs_node_set_pos(A, &OP);
}
elif_clause(A) ::= ELIF(OP) LPAREN expr(C) RPAREN expr(T) ELSE expr(E). {
    A = tbs_fold_if(ctx, C, T, E);
    tbs_node_set_pos(A, &OP);
}
elif_clause(A) ::= ELIF(OP) LPAREN expr(C) RPAREN expr(T) elif_clause(E). {
    A = tbs_fold_if(ctx, C, T, E);
    tbs_node_set_pos(A, &OP);
}

// Ternary operator: cond ? then : else
expr(A) ::= expr(C) QUESTION(OP) expr(T) COLON expr(E). [QUESTION] {
    A = tbs_fold_if(ctx, C, T, E);
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= WHILE(OP) LPAREN expr(C) RPAREN expr(B). {
    A = tbs_node_new(ctx, TBS_NODE_WHILE);
    if (A) {
        A->data.while_loop.condition = C;
        A->data.while_loop.body = B;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= FOR(OP) LPAREN expr(I) SEMICOLON expr(C) SEMICOLON expr(P) RPAREN expr(B). {
    A = tbs_node_new(ctx, TBS_NODE_FOR);
    if (A) {
        A->data.for_loop.init = I;
        A->data.for_loop.condition = C;
        A->data.for_loop.post = P;
        A->data.for_loop.body = B;
        tbs_node_set_pos(A, &OP);
    }
}

// For-in loop: for (x in collection) { ... }
expr(A) ::= FOR(OP) LPAREN VARIABLE(V) IN expr(C) RPAREN expr(B). {
    A = tbs_node_new(ctx, TBS_NODE_FOR_IN);
    if (A) {
        A->data.for_in.var_name = tbs_strdup(ctx, V.start, V.length);
        A->data.for_in.collection = C;
        A->data.for_in.body = B;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= DO(OP) expr(B) WHILE LPAREN expr(C) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_DO_WHILE);
    if (A) {
        A->data.do_while.body = B;
        A->data.do_while.condition = C;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= SWITCH(OP) LPAREN expr(V) RPAREN LBRACE switch_cases(C) RBRACE. {
    A = tbs_node_new(ctx, TBS_NODE_SWITCH);
    if (A) {
        A->data.switch_stmt.value = V;
        A->data.switch_stmt.cases = C->data.switch_stmt.cases;
        A->data.switch_stmt.case_count = C->data.switch_stmt.case_count;
        A->data.switch_stmt.default_case = C->data.switch_stmt.default_case;
        tbs_node_set_pos(A, &OP);
    }
}

switch_cases(A) ::= . {
    A = tbs_node_new(ctx, TBS_NODE_SWITCH);
    A->data.switch_stmt.cases = NULL;
    A->data.switch_stmt.case_count = 0;
    A->data.switch_stmt.default_case = NULL;
}

switch_cases(A) ::= switch_cases(L) CASE expr(V) COLON block_content(B). {
    A = L;
    A->data.switch_stmt.case_count++;
    tbs_node_t **new_cases = (tbs_node_t**)mem_alloc(ctx->arena, A->data.switch_stmt.case_count * 2 * sizeof(tbs_node_t*));
    if (A->data.switch_stmt.case_count > 1) {
        memcpy(new_cases, A->data.switch_stmt.cases, (A->data.switch_stmt.case_count - 1) * 2 * sizeof(tbs_node_t*));
    }
    new_cases[(A->data.switch_stmt.case_count - 1) * 2] = V;
    new_cases[(A->data.switch_stmt.case_count - 1) * 2 + 1] = B;
    A->data.switch_stmt.cases = new_cases;
}

switch_cases(A) ::= switch_cases(L) DEFAULT COLON block_content(B). {
    A = L;
    A->data.switch_stmt.default_case = B;
}

expr(A) ::= BREAK(OP). {
    A = tbs_node_new(ctx, TBS_NODE_FLOW);
    if (A) {
        A->data.flow.type = tbs_TOKEN_BREAK;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= CONTINUE(OP). {
    A = tbs_node_new(ctx, TBS_NODE_FLOW);
    if (A) {
        A->data.flow.type = tbs_TOKEN_CONTINUE;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= RETURN(OP) expr(V). {
    A = tbs_node_new(ctx, TBS_NODE_FLOW);
    if (A) {
        A->data.flow.type = tbs_TOKEN_RETURN;
        A->data.flow.value = V;
        tbs_node_set_pos(A, &OP);
    }
}

// throw expression
expr(A) ::= THROW(OP) expr(V). {
    A = tbs_node_new(ctx, TBS_NODE_THROW);
    if (A) {
        A->data.throw_stmt.value = V;
        tbs_node_set_pos(A, &OP);
    }
}

// try { ... } catch (e) { ... }
expr(A) ::= TRY(OP) block(T) CATCH LPAREN VARIABLE(V) RPAREN block(C). {
    A = tbs_node_new(ctx, TBS_NODE_TRY_CATCH);
    if (A) {
        A->data.try_catch.try_body = T;
        A->data.try_catch.catch_var = tbs_strdup(ctx, V.start, V.length);
        A->data.try_catch.catch_body = C;
        tbs_node_set_pos(A, &OP);
    }
}

// try { ... } catch { ... } (no variable binding)
expr(A) ::= TRY(OP) block(T) CATCH block(C). {
    A = tbs_node_new(ctx, TBS_NODE_TRY_CATCH);
    if (A) {
        A->data.try_catch.try_body = T;
        A->data.try_catch.catch_var = NULL;
        A->data.try_catch.catch_body = C;
        tbs_node_set_pos(A, &OP);
    }
}

// Function Definition (Go-like)
async_opt ::= .
async_opt ::= ASYNC.

func_def(A) ::= async_opt FUNC(OP) VARIABLE(Name) LPAREN expr(Arg) RPAREN block(Body). {
    tbs_node_t *params[1] = {Arg};
    if (!tbs_validate_param_list(ctx, params, 1, "Invalid function parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_DEFINITION,
                                      tbs_strdup(ctx, Name.start, Name.length),
                                      params, 1, Body, &OP, 0, 0);
    }
}

func_def(A) ::= async_opt FUNC(OP) VARIABLE(Name) LPAREN expr_list_2plus(Args) RPAREN block(Body). {
    if (!tbs_validate_param_list(ctx, Args->data.function.args, Args->data.function.arg_count,
                                    "Invalid function parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_DEFINITION,
                                      tbs_strdup(ctx, Name.start, Name.length),
                                      Args->data.function.args, Args->data.function.arg_count,
                                      Body, &OP, 0, 0);
    }
}

func_def(A) ::= async_opt FUNC(OP) VARIABLE(Name) LPAREN RPAREN block(Body). {
    A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_DEFINITION,
                                  tbs_strdup(ctx, Name.start, Name.length),
                                  NULL, 0, Body, &OP, 0, 0);
}

// Anonymous function expressions: func(params) { body }  (no name → first-class value)
expr(A) ::= async_opt FUNC(OP) LPAREN RPAREN block(Body). {
    A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_EXPRESSION,
                                  NULL, NULL, 0, Body, &OP, 0, 0);
}

expr(A) ::= async_opt FUNC(OP) LPAREN expr(Arg) RPAREN block(Body). {
    tbs_node_t *params[1] = {Arg};
    if (!tbs_validate_param_list(ctx, params, 1, "Invalid function parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_EXPRESSION,
                                      NULL, params, 1, Body, &OP, 0, 0);
    }
}

expr(A) ::= async_opt FUNC(OP) LPAREN expr_list_2plus(Args) RPAREN block(Body). {
    if (!tbs_validate_param_list(ctx, Args->data.function.args, Args->data.function.arg_count,
                                    "Invalid function parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_EXPRESSION,
                                      NULL, Args->data.function.args,
                                      Args->data.function.arg_count, Body, &OP, 0, 0);
    }
}

// Arrow functions
expr(A) ::= expr(E) ARROW(OP) expr(B). {
    tbs_node_t *params[1] = {E};
    if (!B || !tbs_validate_param_list(ctx, params, 1, "Invalid arrow function parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_EXPRESSION,
                                      NULL, params, 1, B, &OP, 1, tbs_TOKEN_RETURN);
    }
}

expr(A) ::= LPAREN expr_list_2plus(Args) RPAREN ARROW(OP) expr(B). [ARROW] {
    if (!Args || !B) {
        A = NULL;
    } else if (!tbs_validate_param_list(ctx, Args->data.function.args,
                                           Args->data.function.arg_count,
                                           "Invalid arrow function parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_EXPRESSION,
                                      NULL, Args->data.function.args,
                                      Args->data.function.arg_count, B, &OP, 1, tbs_TOKEN_RETURN);
    }
}

expr(A) ::= LPAREN RPAREN ARROW(OP) expr(B). [ARROW] {
    if (!B) {
        A = NULL;
    } else {
        A = tbs_make_function_node(ctx, TBS_NODE_FUNCTION_EXPRESSION,
                                      NULL, NULL, 0, B, &OP, 1, tbs_TOKEN_RETURN);
    }
}

expr(A) ::= func_def(F). { A = F; }

// TODO: 协程语法规则暂时禁用，存在解析冲突需要解决
/*
// Generator Function Definition: func* name(params) { ... }
func_def(A) ::= FUNC_GENERATOR(OP) VARIABLE(Name) LPAREN expr(Arg) RPAREN block(Body). {
    tbs_node_t *params[1] = {Arg};
    if (!tbs_validate_param_list(ctx, params, 1, "Invalid generator parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_node_new(ctx, TBS_NODE_GENERATOR_FUNCTION);
        if (A) {
            A->data.generator_def.name = tbs_strdup(ctx, Name.start, Name.length);
            A->data.generator_def.arg_params = mem_alloc(ctx->arena, sizeof(tbs_node_t*));
            if (A->data.generator_def.arg_params) {
                A->data.generator_def.arg_params[0] = params[0];
            }
            A->data.generator_def.arg_count = 1;
            A->data.generator_def.body = Body;
            tbs_node_set_pos(A, &OP);
        }
    }
}

func_def(A) ::= FUNC_GENERATOR(OP) VARIABLE(Name) LPAREN expr_list_2plus(Args) RPAREN block(Body). {
    if (!tbs_validate_param_list(ctx, Args->data.function.args, Args->data.function.arg_count,
                                    "Invalid generator parameter pattern")) {
        A = NULL;
    } else {
        A = tbs_node_new(ctx, TBS_NODE_GENERATOR_FUNCTION);
        if (A) {
            A->data.generator_def.name = tbs_strdup(ctx, Name.start, Name.length);
            A->data.generator_def.arg_params = Args->data.function.args;
            A->data.generator_def.arg_count = Args->data.function.arg_count;
            A->data.generator_def.body = Body;
            tbs_node_set_pos(A, &OP);
        }
    }
}

func_def(A) ::= FUNC_GENERATOR(OP) VARIABLE(Name) LPAREN RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_GENERATOR_FUNCTION);
    if (A) {
        A->data.generator_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.generator_def.arg_params = NULL;
        A->data.generator_def.arg_count = 0;
        A->data.generator_def.body = Body;
        tbs_node_set_pos(A, &OP);
    }
}

// yield expression: yield value
expr(A) ::= YIELD(OP) expr(V). {
    A = tbs_node_new(ctx, TBS_NODE_YIELD);
    if (A) {
        A->data.yield_expr.value = V;
        tbs_node_set_pos(A, &OP);
    }
}

// yield without value: yield
expr(A) ::= YIELD(OP). {
    A = tbs_node_new(ctx, TBS_NODE_YIELD);
    if (A) {
        A->data.yield_expr.value = NULL;
        tbs_node_set_pos(A, &OP);
    }
}
*/



// Support JS-style assignment: x = 10 or f(x) = ...
expr(A) ::= expr(B) EQUAL(OP) expr(C). {
    if (B->type == TBS_NODE_VARIABLE) {
        if (C && C->type == TBS_NODE_FUNCTION_DEFINITION && C->data.func_def.name == NULL) {
            // Assigning anonymous function to a variable natively names the function
            C->data.func_def.name = tbs_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
            A = C; // Just emit it directly as a named function definition!
            tbs_node_set_pos(A, &OP);
        } else {
            A = tbs_node_new(ctx, TBS_NODE_ASSIGNMENT);
            if (A) {
                A->data.assignment.name = tbs_strdup(ctx, B->data.variable.name, strlen(B->data.variable.name));
                A->data.assignment.value = C;
                tbs_node_set_pos(A, &OP);
            }
        }
    } else if (B->type == TBS_NODE_MEMBER_ACCESS) {
        // Member assignment: obj.key = val
        A = tbs_node_new(ctx, TBS_NODE_MEMBER_SET);
        if (A) {
            A->data.member_set.object = B->data.member_access.object;
            A->data.member_set.member = tbs_strdup(ctx, B->data.member_access.member, strlen(B->data.member_access.member));
            A->data.member_set.value = C;
            tbs_node_set_pos(A, &OP);
        }
    } else if (B->type == TBS_NODE_SUPER &&
               B->data.super_expr.member != NULL &&
               !B->data.super_expr.is_call) {
        // Static super field assignment: super.key = val
        A = tbs_node_new(ctx, TBS_NODE_MEMBER_SET);
        if (A) {
            A->data.member_set.object = B;
            A->data.member_set.member = tbs_strdup(ctx, B->data.super_expr.member, strlen(B->data.super_expr.member));
            A->data.member_set.value = C;
            tbs_node_set_pos(A, &OP);
        }
    } else if (B->type == TBS_NODE_VECTOR || B->type == TBS_NODE_MAP_LITERAL) {
        // Destructuring assignment: [a, b] = [1, 2] or map{a, b} = m
        A = tbs_node_new(ctx, TBS_NODE_DESTRUCTURING_ASSIGNMENT);
        if (A) {
            A->data.destructuring.targets = B;
            A->data.destructuring.value = C;
            A->data.destructuring.is_constant = 0;
            tbs_node_set_pos(A, &OP);
        }
    } else if (B->type == TBS_NODE_FUNCTION_CALL) {
        // Function Definition: f(x, y) = ...
        int valid = 1;
        for (size_t i = 0; i < B->data.function.arg_count; ++i) {
            tbs_node_t *arg = B->data.function.args[i];
            if (arg->type != TBS_NODE_VARIABLE && arg->type != TBS_NODE_VECTOR && arg->type != TBS_NODE_MAP_LITERAL) {
                valid = 0;
                break;
            }
        }
        if (!valid) {
            tbs_record_parse_error(ctx, 1, "Invalid function parameter pattern");
            A = NULL;
        } else {
            A = tbs_node_new(ctx, TBS_NODE_FUNCTION_DEFINITION);
            if (A) {
                A->data.func_def.name = tbs_strdup(ctx, B->data.function.name, strlen(B->data.function.name));
                A->data.func_def.arg_count = B->data.function.arg_count;
                A->data.func_def.arg_params = (tbs_node_t**)mem_alloc(ctx->arena, A->data.func_def.arg_count * sizeof(tbs_node_t*));
                for (size_t i = 0; i < A->data.func_def.arg_count; ++i) {
                    A->data.func_def.arg_params[i] = B->data.function.args[i];
                }
                A->data.func_def.body = C;
                tbs_node_set_pos(A, &OP);
            }
        }
    } else {
        tbs_record_parse_error(ctx, 1, "Invalid l-value for assignment");
        A = NULL;
    }
}

expr(A) ::= VAR(OP) VARIABLE(V) EQUAL expr(C). {
    if (C && C->type == TBS_NODE_FUNCTION_DEFINITION && C->data.func_def.name == NULL) {
        C->data.func_def.name = tbs_strdup(ctx, V.start, V.length);
        A = C;
        tbs_node_set_pos(A, &OP);
    } else {
        A = tbs_node_new(ctx, TBS_NODE_ASSIGNMENT);
        if (A) {
            A->data.assignment.name = tbs_strdup(ctx, V.start, V.length);
            A->data.assignment.value = C;
            tbs_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= VAR(OP) LBRACKET vector_elements(V) RBRACKET EQUAL expr(C). {
    tbs_node_t *lhs = tbs_node_new(ctx, TBS_NODE_VECTOR);
    lhs->data.vector.elements = V->data.vector.elements;
    lhs->data.vector.count = V->data.vector.count;

    A = tbs_node_new(ctx, TBS_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = lhs;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 0;
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= VAR(OP) MAP LBRACE map_entries(E) RBRACE EQUAL expr(C). {
    A = tbs_node_new(ctx, TBS_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = E;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 0;
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= CONST(OP) VARIABLE(V) EQUAL expr(C). {
    if (C && C->type == TBS_NODE_FUNCTION_DEFINITION && C->data.func_def.name == NULL) {
        C->data.func_def.name = tbs_strdup(ctx, V.start, V.length);
        // We can't easily mark function definitions as "constant" in the same way as variables
        // but since we don't have function re-assignment checks yet, this is fine.
        A = C;
        tbs_node_set_pos(A, &OP);
    } else {
        A = tbs_node_new(ctx, TBS_NODE_CONSTANT_DECL);
        if (A) {
            A->data.assignment.name = tbs_strdup(ctx, V.start, V.length);
            A->data.assignment.value = C;
            tbs_node_set_pos(A, &OP);
        }
    }
}

expr(A) ::= CONST(OP) LBRACKET vector_elements(V) RBRACKET EQUAL expr(C). {
    tbs_node_t *lhs = tbs_node_new(ctx, TBS_NODE_VECTOR);
    lhs->data.vector.elements = V->data.vector.elements;
    lhs->data.vector.count = V->data.vector.count;

    A = tbs_node_new(ctx, TBS_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = lhs;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 1;
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= CONST(OP) MAP LBRACE map_entries(E) RBRACE EQUAL expr(C). {
    A = tbs_node_new(ctx, TBS_NODE_DESTRUCTURING_ASSIGNMENT);
    A->data.destructuring.targets = E;
    A->data.destructuring.value = C;
    A->data.destructuring.is_constant = 1;
    tbs_node_set_pos(A, &OP);
}

// Assignment - Compound (e.g. +=) maps to x := x + c
// For simplicity, let's treat them as regular assignments in the AST or expand them.
// Expanding them in AST is better for the evaluator.
// x += 1  =>  x := x + 1
// But we need to duplicate B (variable).
// Or we can add specific AST node types for compound assignment.
// For Phase 1, let's just implement basic ASSIGN := and map += to it if possible or just parse them and error/ignore for now to fix build?
// User asked for +=.
// Let's create `TBS_NODE_ASSIGNMENT` with an extra field `op`?
// Or just handle in evaluator. Evaluator currently has `TBS_NODE_ASSIGNMENT` with `value`.
// If I change evaluator I need to update struct.
// Let's just implement `ASSIGN` first to fix conflicts/warnings, and add `ASSIGN_ADD` etc later or now.
// I will implement `ASSIGN_ADD` by constructing `x = x + c` semantic tree.
expr(A) ::= expr(B) ASSIGN_ADD(OP) expr(C). {
     A = tbs_make_compound_assign(ctx, B, tbs_TOKEN_PLUS, C, &OP);
}
// Repeat for other compound assignments or leave them for now to pass build?
// I'll leave others for now to minimize code size in this turn, focus on fixing build.
// Actually I should at least define the rules so they parse (even if I error/nop).
// But I defined precedence for them, so I should use them.
// I'll just do ASSIGN_SUB for completeness.
expr(A) ::= expr(B) ASSIGN_SUB(OP) expr(C). {
     A = tbs_make_compound_assign(ctx, B, tbs_TOKEN_MINUS, C, &OP);
}

expr(A) ::= expr(B) ASSIGN_MUL(OP) expr(C). {
     A = tbs_make_compound_assign(ctx, B, tbs_TOKEN_MULTIPLY, C, &OP);
}

expr(A) ::= expr(B) ASSIGN_DIV(OP) expr(C). {
     A = tbs_make_compound_assign(ctx, B, tbs_TOKEN_DIVIDE, C, &OP);
}
// Skip MUL/DIV for now to keep it short.

// Binary Operations
// Comparison Operations
expr(A) ::= expr(B) EQ(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) NE(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) LT(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) LE(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) GT(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) GE(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }

// Logical Operations
expr(A) ::= expr(B) AND(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) OR(OP) expr(C).  { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }

// Arithmetic Operations
expr(A) ::= expr(B) PLUS(OP) expr(C).     { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) MINUS(OP) expr(C).    { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) MULTIPLY(OP) expr(C). { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) DIVIDE(OP) expr(C).   { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) MOD(OP) expr(C).      { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }
expr(A) ::= expr(B) POWER(OP) expr(C).    { A = tbs_fold_binary(ctx, OP.type, B, C); tbs_node_set_pos(A, &OP); }

expr(A) ::= MINUS(OP) expr(B). [NOT] {
    A = tbs_fold_unary(ctx, tbs_TOKEN_MINUS, B);
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= PLUS(OP) expr(B). [NOT] {
    A = tbs_fold_unary(ctx, tbs_TOKEN_PLUS, B);
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= NOT(OP) expr(B). {
    A = tbs_fold_unary(ctx, tbs_TOKEN_NOT, B);
    tbs_node_set_pos(A, &OP);
}

expr(A) ::= AWAIT(OP) expr(B). [AWAIT] {
    A = tbs_make_unary_call(ctx, "await", B, &OP);
}

expr(A) ::= SPREAD(OP) expr(B). [NOT] {
    A = tbs_node_new(ctx, TBS_NODE_SPREAD);
    if (A) {
        A->data.spread.child = B;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= LPAREN expr(B) RPAREN. {
    A = B;
}

expr(A) ::= NUMBER(N). {
    A = tbs_node_new(ctx, TBS_NODE_NUMBER);
    A->data.number = N.num_value;
    tbs_node_set_pos(A, &N);
}

expr(A) ::= INTEGER(N). {
    A = tbs_node_new(ctx, TBS_NODE_INTEGER);
    A->data.integer = (int64_t)N.num_value;
    tbs_node_set_pos(A, &N);
}

expr(A) ::= STRING(S). {
    A = tbs_node_new(ctx, TBS_NODE_STRING);
    if (A) {
        A->data.string.value = tbs_unescape_to_arena(ctx, S.start + 1, S.length - 2);
        tbs_node_set_pos(A, &S);
    }
}

expr(A) ::= REGEX(R). {
    A = tbs_make_regex_literal(ctx, &R);
}

expr(A) ::= TEMPLATE(S). {
    A = tbs_node_new(ctx, TBS_NODE_TEMPLATE_STRING);
    if (A) {
        A->data.template_string.template_str = tbs_strdup(ctx, S.start + 1, S.length - 2);
        A->data.template_string.len = S.length - 2;
        tbs_node_set_pos(A, &S);
    }
}

expr(A) ::= NULL(N). {
    A = tbs_node_new(ctx, TBS_NODE_NULL);
    tbs_node_set_pos(A, &N);
}

expr(A) ::= VARIABLE(V). {
    A = tbs_node_new(ctx, TBS_NODE_VARIABLE);
    if (A) {
        A->data.variable.name = tbs_strdup(ctx, V.start, V.length);
        tbs_node_set_pos(A, &V);
    }
}

expr(A) ::= expr(F) LPAREN expr(E) RPAREN. {
    if (F->type != TBS_NODE_VARIABLE) {
        tbs_record_parse_error(ctx, 1, "Invalid function call");
        A = NULL;
    } else {
        A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
        if (A) {
            A->data.function.name = tbs_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
            A->data.function.arg_count = 1;
            A->data.function.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
            A->data.function.args[0] = E;
            tbs_node_copy_pos(A, F);
        }
    }
}

expr(A) ::= expr(F) LPAREN expr_list_2plus(L) RPAREN. {
    if (F->type != TBS_NODE_VARIABLE) {
        tbs_record_parse_error(ctx, 1, "Invalid function call");
        A = NULL;
    } else {
        A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
        if (A) {
            A->data.function.name = tbs_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
            A->data.function.args = L->data.function.args;
            A->data.function.arg_count = L->data.function.arg_count;
            tbs_node_copy_pos(A, F);
        }
    }
}

expr(A) ::= expr(F) LPAREN RPAREN. {
    if (F->type != TBS_NODE_VARIABLE) {
        tbs_record_parse_error(ctx, 1, "Invalid function call");
        A = NULL;
    } else {
        A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
        if (A) {
            A->data.function.name = tbs_strdup(ctx, F->data.variable.name, strlen(F->data.variable.name));
            A->data.function.args = NULL;
            A->data.function.arg_count = 0;
            tbs_node_copy_pos(A, F);
        }
    }
}

expr(A) ::= MAP(OP) LPAREN expr(E) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.name = tbs_strdup(ctx, "map", 3);
        A->data.function.arg_count = 1;
        A->data.function.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.function.args[0] = E;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= MAP(OP) LPAREN expr_list_2plus(L) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.name = tbs_strdup(ctx, "map", 3);
        A->data.function.args = L->data.function.args;
        A->data.function.arg_count = L->data.function.arg_count;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= MAP(OP) LPAREN RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.name = tbs_strdup(ctx, "map", 3);
        A->data.function.args = NULL;
        A->data.function.arg_count = 0;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= MAP(OP) DOT VARIABLE(V) LPAREN expr(E) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.name = tbs_prefixed_name(ctx, "map", 3, &V);
        A->data.function.arg_count = 1;
        A->data.function.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.function.args[0] = E;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= MAP(OP) DOT VARIABLE(V) LPAREN expr_list_2plus(L) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.name = tbs_prefixed_name(ctx, "map", 3, &V);
        A->data.function.args = L->data.function.args;
        A->data.function.arg_count = L->data.function.arg_count;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= MAP(OP) DOT VARIABLE(V) LPAREN RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.name = tbs_prefixed_name(ctx, "map", 3, &V);
        A->data.function.args = NULL;
        A->data.function.arg_count = 0;
        tbs_node_set_pos(A, &OP);
    }
}

// Explicit instantiation: new ClassName(args)
expr(A) ::= NEW(OP) VARIABLE(Name) LPAREN RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_NEW);
    if (A) {
        A->data.new_expr.class_name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.new_expr.args = NULL;
        A->data.new_expr.arg_count = 0;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= NEW(OP) VARIABLE(Name) LPAREN expr(E) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_NEW);
    if (A) {
        A->data.new_expr.class_name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.new_expr.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.new_expr.args[0] = E;
        A->data.new_expr.arg_count = 1;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= NEW(OP) VARIABLE(Name) LPAREN expr_list_2plus(L) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_NEW);
    if (A) {
        A->data.new_expr.class_name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.new_expr.args = L->data.function.args;
        A->data.new_expr.arg_count = L->data.function.arg_count;
        tbs_node_set_pos(A, &OP);
    }
}

// Member call: obj.method(args) and obj.method()
expr(A) ::= expr(B) DOT(OP) VARIABLE(V) LPAREN expr(E) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = tbs_strdup(ctx, V.start, V.length);
        A->data.member_call.arg_count = 1;
        A->data.member_call.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.member_call.args[0] = E;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) MAP LPAREN expr(E) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = tbs_strdup(ctx, "map", 3);
        A->data.member_call.arg_count = 1;
        A->data.member_call.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.member_call.args[0] = E;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) MAP LPAREN expr_list_2plus(L) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = tbs_strdup(ctx, "map", 3);
        A->data.member_call.args = L->data.function.args;
        A->data.member_call.arg_count = L->data.function.arg_count;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) MAP LPAREN RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = tbs_strdup(ctx, "map", 3);
        A->data.member_call.args = NULL;
        A->data.member_call.arg_count = 0;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) VARIABLE(V) LPAREN expr_list_2plus(L) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = tbs_strdup(ctx, V.start, V.length);
        A->data.member_call.args = L->data.function.args;
        A->data.member_call.arg_count = L->data.function.arg_count;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) VARIABLE(V) LPAREN RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_CALL);
    if (A) {
        A->data.member_call.object = B;
        A->data.member_call.method = tbs_strdup(ctx, V.start, V.length);
        A->data.member_call.args = NULL;
        A->data.member_call.arg_count = 0;
        tbs_node_set_pos(A, &OP);
    }
}

// Member access: obj.key (property read). Uses MEMBER_PREC so LPAREN can shift for member calls.
expr(A) ::= expr(B) DOT(OP) VARIABLE(V). [MEMBER_PREC] {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_ACCESS);
    if (A) {
        A->data.member_access.object = B;
        A->data.member_access.member = tbs_strdup(ctx, V.start, V.length);
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) DOT(OP) MAP. [MEMBER_PREC] {
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_ACCESS);
    if (A) {
        A->data.member_access.object = B;
        A->data.member_access.member = tbs_strdup(ctx, "map", 3);
        tbs_node_set_pos(A, &OP);
    }
}

// Optional chaining: obj?.key → if (obj == null) null else obj.key
// Desugars to an IF node at the AST level for zero evaluator changes
expr(A) ::= expr(B) QUESTION_DOT(OP) VARIABLE(V). [MEMBER_PREC] {
    // Build: if (B == null) { null } else { B.member }
    // Note: B is evaluated twice in this desugaring, but for typical use (variable access)
    // this is acceptable.
    tbs_node_t *null_node = tbs_node_new(ctx, TBS_NODE_NULL);
    tbs_node_set_pos(null_node, &OP);

    // condition: B == null
    tbs_node_t *cond = tbs_node_new(ctx, TBS_NODE_BINARY_OP);
    cond->data.binary.op = tbs_TOKEN_EQ;
    cond->data.binary.left = B;
    cond->data.binary.right = tbs_node_new(ctx, TBS_NODE_NULL);
    tbs_node_set_pos(cond, &OP);

    // else branch: B.member (we reuse B, which is fine for simple expressions)
    tbs_node_t *access = tbs_node_new(ctx, TBS_NODE_MEMBER_ACCESS);
    access->data.member_access.object = B;
    access->data.member_access.member = tbs_strdup(ctx, V.start, V.length);
    tbs_node_set_pos(access, &OP);

    // null result for if-true
    tbs_node_t *null_result = tbs_node_new(ctx, TBS_NODE_NULL);
    tbs_node_set_pos(null_result, &OP);

    A = tbs_node_new(ctx, TBS_NODE_IF);
    A->data.if_stmt.condition = cond;
    A->data.if_stmt.if_branch = null_result;
    A->data.if_stmt.else_branch = access;
    tbs_node_set_pos(A, &OP);
}

// Pipe operator: expr |> f(args) → f(expr, args)
// This rewrites the RHS function call to prepend the LHS as the first argument.
expr(A) ::= expr(B) PIPE(OP) expr(C). {
    if (!C || C->type != TBS_NODE_FUNCTION_CALL) {
        tbs_record_parse_error(ctx, 1, "Pipe operator requires a function call on the right side");
        A = NULL;
    } else {
        // Prepend B as first argument to C's function call
        size_t new_count = C->data.function.arg_count + 1;
        tbs_node_t **new_args = (tbs_node_t**)mem_alloc(ctx->arena, new_count * sizeof(tbs_node_t*));
        new_args[0] = B;
        for (size_t i = 0; i < C->data.function.arg_count; ++i) {
            new_args[i + 1] = C->data.function.args[i];
        }
        C->data.function.args = new_args;
        C->data.function.arg_count = new_count;
        A = C;
        tbs_node_set_pos(A, &OP);
    }
}

// Map literal: map{key: val, key2: val2}
expr(A) ::= MAP LBRACE map_entries(E) RBRACE. {
    A = E;
}

expr(A) ::= MAP(OP) LBRACE RBRACE. {
    A = tbs_node_new(ctx, TBS_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.keys = NULL;
        A->data.map_literal.values = NULL;
        A->data.map_literal.count = 0;
        tbs_node_set_pos(A, &OP);
    }
}

map_entries(A) ::= VARIABLE(K). {
    A = tbs_node_new(ctx, TBS_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.count = 1;
        A->data.map_literal.keys = (char**)mem_alloc(ctx->arena, sizeof(char*));
        A->data.map_literal.keys[0] = tbs_strdup(ctx, K.start, K.length);
        A->data.map_literal.values = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));

        tbs_node_t *v = tbs_node_new(ctx, TBS_NODE_VARIABLE);
        v->data.variable.name = tbs_strdup(ctx, K.start, K.length);
        A->data.map_literal.values[0] = v;

        tbs_node_set_pos(A, &K);
    }
}

map_entries(A) ::= VARIABLE(K) COLON expr(V). {
    A = tbs_node_new(ctx, TBS_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.count = 1;
        A->data.map_literal.keys = (char**)mem_alloc(ctx->arena, sizeof(char*));
        A->data.map_literal.keys[0] = tbs_strdup(ctx, K.start, K.length);
        A->data.map_literal.values = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.map_literal.values[0] = V;
        tbs_node_set_pos(A, &K);
    }
}

map_entries(A) ::= SPREAD(OP) expr(E). {
    A = tbs_node_new(ctx, TBS_NODE_MAP_LITERAL);
    if (A) {
        A->data.map_literal.count = 1;
        A->data.map_literal.keys = (char**)mem_alloc(ctx->arena, sizeof(char*));
        A->data.map_literal.keys[0] = NULL; // Spread marker
        A->data.map_literal.values = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.map_literal.values[0] = E;
        tbs_node_set_pos(A, &OP);
    }
}

map_entries(A) ::= map_entries(L) COMMA VARIABLE(K). {
    A = L;
    A->data.map_literal.count++;
    size_t c = A->data.map_literal.count;
    char **new_keys = (char**)mem_alloc(ctx->arena, c * sizeof(char*));
    tbs_node_t **new_vals = (tbs_node_t**)mem_alloc(ctx->arena, c * sizeof(tbs_node_t*));
    if (L->data.map_literal.keys) memcpy(new_keys, L->data.map_literal.keys, (c-1) * sizeof(char*));
    if (L->data.map_literal.values) memcpy(new_vals, L->data.map_literal.values, (c-1) * sizeof(tbs_node_t*));

    new_keys[c-1] = tbs_strdup(ctx, K.start, K.length);
    tbs_node_t *v = tbs_node_new(ctx, TBS_NODE_VARIABLE);
    v->data.variable.name = tbs_strdup(ctx, K.start, K.length);
    new_vals[c-1] = v;

    A->data.map_literal.keys = new_keys;
    A->data.map_literal.values = new_vals;
}

map_entries(A) ::= map_entries(L) COMMA VARIABLE(K) COLON expr(V). {
    A = L;
    A->data.map_literal.count++;
    size_t c = A->data.map_literal.count;
    char **new_keys = (char**)mem_alloc(ctx->arena, c * sizeof(char*));
    tbs_node_t **new_vals = (tbs_node_t**)mem_alloc(ctx->arena, c * sizeof(tbs_node_t*));
    if (L->data.map_literal.keys) memcpy(new_keys, L->data.map_literal.keys, (c-1) * sizeof(char*));
    if (L->data.map_literal.values) memcpy(new_vals, L->data.map_literal.values, (c-1) * sizeof(tbs_node_t*));
    new_keys[c-1] = tbs_strdup(ctx, K.start, K.length);
    new_vals[c-1] = V;
    A->data.map_literal.keys = new_keys;
    A->data.map_literal.values = new_vals;
}

map_entries(A) ::= map_entries(L) COMMA SPREAD(OP) expr(E). {
    A = L;
    A->data.map_literal.count++;
    size_t c = A->data.map_literal.count;
    char **new_keys = (char**)mem_alloc(ctx->arena, c * sizeof(char*));
    tbs_node_t **new_vals = (tbs_node_t**)mem_alloc(ctx->arena, c * sizeof(tbs_node_t*));
    if (L->data.map_literal.keys) memcpy(new_keys, L->data.map_literal.keys, (c-1) * sizeof(char*));
    if (L->data.map_literal.values) memcpy(new_vals, L->data.map_literal.values, (c-1) * sizeof(tbs_node_t*));
    new_keys[c-1] = NULL; // Spread marker
    new_vals[c-1] = E;
    A->data.map_literal.keys = new_keys;
    A->data.map_literal.values = new_vals;
    (void)OP;
}

expr_list_2plus(A) ::= expr(E1) COMMA(OP) expr(E2). {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.arg_count = 2;
        A->data.function.args = (tbs_node_t**)mem_alloc(ctx->arena, 2 * sizeof(tbs_node_t*));
        A->data.function.args[0] = E1;
        A->data.function.args[1] = E2;
        tbs_node_set_pos(A, &OP);
    }
}

expr_list_2plus(A) ::= expr_list_2plus(L) COMMA expr(E). {
    A = L;
    if (A) {
        A->data.function.arg_count++;
        tbs_node_t **new_args = (tbs_node_t**)mem_alloc(ctx->arena, A->data.function.arg_count * sizeof(tbs_node_t*));
        if (new_args && A->data.function.args) {
            memcpy(new_args, A->data.function.args, (A->data.function.arg_count-1) * sizeof(tbs_node_t*));
            new_args[A->data.function.arg_count-1] = E;
            A->data.function.args = new_args;
        }
    }
}



expr(A) ::= LBRACKET vector_content(B) RBRACKET. {
    A = B;
}

expr(A) ::= expr(B) LBRACKET(OP) expr(C) RBRACKET. {
    A = tbs_node_new(ctx, TBS_NODE_INDEX);
    if (A) {
        A->data.index_access.array = B;
        A->data.index_access.index = C;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= expr(B) LBRACKET(OP) expr(S) DOTDOT expr(E) RBRACKET. {
    A = tbs_node_new(ctx, TBS_NODE_SLICE);
    if (A) {
        A->data.slice.array = B;
        A->data.slice.start = S;
        A->data.slice.end = E;
        tbs_node_set_pos(A, &OP);
    }
}

vector_content(A) ::= . {
    A = tbs_node_new(ctx, TBS_NODE_VECTOR);
    if (A) {
        A->data.vector.count = 0;
        A->data.vector.elements = NULL;
    }
}

vector_content(A) ::= vector_elements(E). {
    A = E;
}

vector_elements(A) ::= expr(E). {
    A = tbs_node_new(ctx, TBS_NODE_VECTOR);
    if (A) {
        A->data.vector.count = 1;
        A->data.vector.elements = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.vector.elements[0] = E;
        tbs_node_copy_pos(A, E);
    }
}

vector_elements(A) ::= vector_elements(L) COMMA expr(R). {
    A = L;
    A->data.vector.count++;
    tbs_node_t **new_elements = (tbs_node_t**)mem_alloc(ctx->arena, A->data.vector.count * sizeof(tbs_node_t*));
    memcpy(new_elements, A->data.vector.elements, (A->data.vector.count - 1) * sizeof(tbs_node_t*));
    new_elements[A->data.vector.count - 1] = R;
    A->data.vector.elements = new_elements;
}

%syntax_error {
    ctx->error = 1;
    if (TOKEN.start && TOKEN.length > 0) {
        tbs_record_parse_error(ctx, 0, "Syntax error at line %d, col %d near '%.*s'",
                                  TOKEN.line, TOKEN.column, (int)TOKEN.length, TOKEN.start);
    } else {
        tbs_record_parse_error(ctx, 0, "Syntax error at line %d, col %d",
                                  TOKEN.line, TOKEN.column);
    }
}

%parse_failure {
    ctx->error = 1;
    ctx->fatal_error = 1;
    if (ctx->error_msg[0] == '\0') {
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Parse failure");
    }
}



// ============================================================================
// OOP (Object-Oriented Programming) Grammar Rules
// ============================================================================

// Class definition: class ClassName { constructor(...) { ... } method1(...) { ... } }
expr(A) ::= CLASS(OP) VARIABLE(Name) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        tbs_node_set_pos(A, &OP);
    }
}

// Final class definition: final class ClassName { ... }
expr(A) ::= FINAL(Final) CLASS(OP) VARIABLE(Name) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        A->data.class_def.is_final = 1;
        tbs_node_set_pos(A, &Final);
        (void)OP;
    }
}

// Class with inheritance: class ClassName extends ParentName { ... }
expr(A) ::= CLASS(OP) VARIABLE(Name) EXTENDS VARIABLE(Parent) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = tbs_strdup(ctx, Parent.start, Parent.length);
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= FINAL(Final) CLASS(OP) VARIABLE(Name) EXTENDS VARIABLE(Parent) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = tbs_strdup(ctx, Parent.start, Parent.length);
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        A->data.class_def.is_final = 1;
        tbs_node_set_pos(A, &Final);
        (void)OP;
    }
}

// Class with interface requirements: class ClassName implements InterfaceName { ... }
expr(A) ::= CLASS(OP) VARIABLE(Name) IMPLEMENTS interface_name_list(Interfaces) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        tbs_set_class_interfaces(ctx, A, Interfaces);
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= FINAL(Final) CLASS(OP) VARIABLE(Name) IMPLEMENTS interface_name_list(Interfaces) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        A->data.class_def.is_final = 1;
        tbs_set_class_interfaces(ctx, A, Interfaces);
        tbs_node_set_pos(A, &Final);
        (void)OP;
    }
}

// Class with inheritance and interface requirements.
expr(A) ::= CLASS(OP) VARIABLE(Name) EXTENDS VARIABLE(Parent) IMPLEMENTS interface_name_list(Interfaces) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = tbs_strdup(ctx, Parent.start, Parent.length);
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        tbs_set_class_interfaces(ctx, A, Interfaces);
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= FINAL(Final) CLASS(OP) VARIABLE(Name) EXTENDS VARIABLE(Parent) IMPLEMENTS interface_name_list(Interfaces) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = tbs_strdup(ctx, Parent.start, Parent.length);
        A->data.class_def.is_abstract = 0;
        A->data.class_def.is_interface = 0;
        A->data.class_def.is_final = 1;
        tbs_set_class_interfaces(ctx, A, Interfaces);
        tbs_node_set_pos(A, &Final);
        (void)OP;
    }
}

// Abstract class definition: abstract class ClassName { ... }
expr(A) ::= ABSTRACT(OP) CLASS VARIABLE(Name) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 1;
        A->data.class_def.is_interface = 0;
        tbs_node_set_pos(A, &OP);
    }
}

// Abstract class with inheritance: abstract class ClassName extends ParentName { ... }
expr(A) ::= ABSTRACT(OP) CLASS VARIABLE(Name) EXTENDS VARIABLE(Parent) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = tbs_strdup(ctx, Parent.start, Parent.length);
        A->data.class_def.is_abstract = 1;
        A->data.class_def.is_interface = 0;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= ABSTRACT(OP) CLASS VARIABLE(Name) IMPLEMENTS interface_name_list(Interfaces) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 1;
        A->data.class_def.is_interface = 0;
        tbs_set_class_interfaces(ctx, A, Interfaces);
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= ABSTRACT(OP) CLASS VARIABLE(Name) EXTENDS VARIABLE(Parent) IMPLEMENTS interface_name_list(Interfaces) LBRACE class_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = tbs_strdup(ctx, Parent.start, Parent.length);
        A->data.class_def.is_abstract = 1;
        A->data.class_def.is_interface = 0;
        tbs_set_class_interfaces(ctx, A, Interfaces);
        tbs_node_set_pos(A, &OP);
    }
}

// Interface definition: interface InterfaceName { method(...); }
expr(A) ::= INTERFACE(OP) VARIABLE(Name) LBRACE interface_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 1;
        A->data.class_def.is_interface = 1;
        tbs_node_set_pos(A, &OP);
    }
}

// Interface inheritance: interface Child extends Parent { method(...); }
expr(A) ::= INTERFACE(OP) VARIABLE(Name) EXTENDS interface_name_list(Interfaces) LBRACE interface_body(Body) RBRACE. {
    A = Body;
    if (A) {
        A->data.class_def.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.class_def.parent_name = NULL;
        A->data.class_def.is_abstract = 1;
        A->data.class_def.is_interface = 1;
        tbs_set_class_interfaces(ctx, A, Interfaces);
        tbs_node_set_pos(A, &OP);
    }
}

// Class body. Constructors are represented as method nodes named "constructor",
// so overloads and modifiers use the same path as other methods.
class_body(A) ::= method_list(Methods). {
    A = tbs_node_new(ctx, TBS_NODE_CLASS_DEF);
    if (A) {
        tbs_init_class_def(A);
        A->data.class_def.constructor = NULL;
        A->data.class_def.methods = Methods->data.vector.elements;
        A->data.class_def.method_count = Methods->data.vector.count;
        A->data.class_def.static_methods = NULL;
        A->data.class_def.static_method_count = 0;
    }
}

interface_name_list(A) ::= VARIABLE(Name). {
    A = tbs_node_new(ctx, TBS_NODE_VECTOR);
    if (A) {
        tbs_node_t *entry = tbs_node_new(ctx, TBS_NODE_VARIABLE);
        if (entry) {
            entry->data.variable.name = tbs_strdup(ctx, Name.start, Name.length);
            tbs_node_set_pos(entry, &Name);
        }
        A->data.vector.count = 1;
        A->data.vector.elements = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        if (A->data.vector.elements) A->data.vector.elements[0] = entry;
        tbs_node_set_pos(A, &Name);
    }
}

interface_name_list(A) ::= interface_name_list(L) COMMA VARIABLE(Name). {
    A = L;
    if (A) {
        tbs_node_t *entry = tbs_node_new(ctx, TBS_NODE_VARIABLE);
        if (entry) {
            entry->data.variable.name = tbs_strdup(ctx, Name.start, Name.length);
            tbs_node_set_pos(entry, &Name);
        }
        A->data.vector.count++;
        tbs_node_t **new_names = (tbs_node_t**)mem_alloc(ctx->arena,
            A->data.vector.count * sizeof(tbs_node_t*));
        if (new_names) {
            memcpy(new_names, L->data.vector.elements,
                (A->data.vector.count - 1) * sizeof(tbs_node_t*));
            new_names[A->data.vector.count - 1] = entry;
            A->data.vector.elements = new_names;
        }
    }
}

interface_body(A) ::= interface_method_list(Methods). {
    A = tbs_node_new(ctx, TBS_NODE_CLASS_DEF);
    if (A) {
        tbs_init_class_def(A);
        A->data.class_def.constructor = NULL;
        A->data.class_def.methods = Methods->data.vector.elements;
        A->data.class_def.method_count = Methods->data.vector.count;
        A->data.class_def.static_methods = NULL;
        A->data.class_def.static_method_count = 0;
    }
}

interface_method_list(A) ::= . {
    A = tbs_node_new(ctx, TBS_NODE_VECTOR);
    if (A) {
        A->data.vector.count = 0;
        A->data.vector.elements = NULL;
    }
}

interface_method_list(A) ::= interface_method_list(L) interface_method_def(M). {
    A = L;
    if (A && M) {
        A->data.vector.count++;
        tbs_node_t **new_methods = (tbs_node_t**)mem_alloc(ctx->arena,
            A->data.vector.count * sizeof(tbs_node_t*));
        if (L->data.vector.elements) {
            memcpy(new_methods, L->data.vector.elements,
                (A->data.vector.count - 1) * sizeof(tbs_node_t*));
        }
        new_methods[A->data.vector.count - 1] = M;
        A->data.vector.elements = new_methods;
    }
}

interface_method_def(A) ::= PUBLIC(OP) interface_method_def(M). {
    A = M;
    if (A) tbs_node_set_pos(A, &OP);
}

interface_method_def(A) ::= STATIC(OP) interface_method_def(M). {
    A = M;
    if (A) {
        A->data.method.is_static = 1;
        tbs_node_set_pos(A, &OP);
    }
}

interface_method_def(A) ::= VARIABLE(Name) LPAREN RPAREN SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = NULL;
        A->data.method.arg_count = 0;
        A->data.method.body = NULL;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 1;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &Name);
    }
}

interface_method_def(A) ::= VARIABLE(Name) LPAREN method_param(Param) RPAREN SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.method.arg_params[0] = Param;
        A->data.method.arg_count = 1;
        A->data.method.body = NULL;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 1;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &Name);
    }
}

interface_method_def(A) ::= VARIABLE(Name) LPAREN method_param_list_2plus(Params) RPAREN SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = Params->data.function.args;
        A->data.method.arg_count = Params->data.function.arg_count;
        A->data.method.body = NULL;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 1;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &Name);
    }
}

// Method list
method_list(A) ::= . {
    A = tbs_node_new(ctx, TBS_NODE_VECTOR);
    if (A) {
        A->data.vector.count = 0;
        A->data.vector.elements = NULL;
    }
}

method_list(A) ::= method_list(L) method_def(M). {
    A = L;
    if (A && M) {
        A->data.vector.count++;
        tbs_node_t **new_methods = (tbs_node_t**)mem_alloc(ctx->arena,
            A->data.vector.count * sizeof(tbs_node_t*));
        if (L->data.vector.elements) {
            memcpy(new_methods, L->data.vector.elements,
                (A->data.vector.count - 1) * sizeof(tbs_node_t*));
        }
        new_methods[A->data.vector.count - 1] = M;
        A->data.vector.elements = new_methods;
    }
}

method_list(A) ::= method_list(L) field_def(F). {
    A = L;
    if (A && F) {
        A->data.vector.count++;
        tbs_node_t **new_members = (tbs_node_t**)mem_alloc(ctx->arena,
            A->data.vector.count * sizeof(tbs_node_t*));
        if (L->data.vector.elements) {
            memcpy(new_members, L->data.vector.elements,
                (A->data.vector.count - 1) * sizeof(tbs_node_t*));
        }
        new_members[A->data.vector.count - 1] = F;
        A->data.vector.elements = new_members;
    }
}

field_def(A) ::= PUBLIC(OP) field_def(F). {
    A = F;
    if (A) {
        A->data.field_decl.access_level = TBS_ACCESS_PUBLIC;
        tbs_node_set_pos(A, &OP);
    }
}

field_def(A) ::= PROTECTED(OP) field_def(F). {
    A = F;
    if (A) {
        A->data.field_decl.access_level = TBS_ACCESS_PROTECTED;
        tbs_node_set_pos(A, &OP);
    }
}

field_def(A) ::= PRIVATE(OP) field_def(F). {
    A = F;
    if (A) {
        A->data.field_decl.access_level = TBS_ACCESS_PRIVATE;
        tbs_node_set_pos(A, &OP);
    }
}

field_def(A) ::= STATIC(OP) field_def(F). {
    A = F;
    if (A) {
        A->data.field_decl.is_static = 1;
        tbs_node_set_pos(A, &OP);
    }
}

field_def(A) ::= VARIABLE(Name) SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_FIELD_DECL);
    if (A) {
        A->data.field_decl.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.field_decl.declared_type = NULL;
        A->data.field_decl.initializer = NULL;
        A->data.field_decl.is_static = 0;
        A->data.field_decl.access_level = TBS_ACCESS_PUBLIC;
        tbs_node_set_pos(A, &Name);
    }
}

field_def(A) ::= VARIABLE(Name) EQUAL expr(Init) SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_FIELD_DECL);
    if (A) {
        A->data.field_decl.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.field_decl.declared_type = NULL;
        A->data.field_decl.initializer = Init;
        A->data.field_decl.is_static = 0;
        A->data.field_decl.access_level = TBS_ACCESS_PUBLIC;
        tbs_node_set_pos(A, &Name);
    }
}

field_def(A) ::= VARIABLE(Name) COLON method_type_name(Type) SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_FIELD_DECL);
    if (A && Type) {
        A->data.field_decl.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.field_decl.declared_type = tbs_strdup(ctx,
            Type->data.variable.name, strlen(Type->data.variable.name));
        A->data.field_decl.initializer = NULL;
        A->data.field_decl.is_static = 0;
        A->data.field_decl.access_level = TBS_ACCESS_PUBLIC;
        tbs_node_set_pos(A, &Name);
    }
}

field_def(A) ::= VARIABLE(Name) COLON method_type_name(Type) EQUAL expr(Init) SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_FIELD_DECL);
    if (A && Type) {
        A->data.field_decl.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.field_decl.declared_type = tbs_strdup(ctx,
            Type->data.variable.name, strlen(Type->data.variable.name));
        A->data.field_decl.initializer = Init;
        A->data.field_decl.is_static = 0;
        A->data.field_decl.access_level = TBS_ACCESS_PUBLIC;
        tbs_node_set_pos(A, &Name);
    }
}

method_param(A) ::= VARIABLE(Name). {
    A = tbs_node_new(ctx, TBS_NODE_VARIABLE);
    if (A) {
        A->data.variable.name = tbs_strdup(ctx, Name.start, Name.length);
        tbs_node_set_pos(A, &Name);
    }
}

method_type_name(A) ::= VARIABLE(Type). {
    A = tbs_make_type_name(ctx, Type.start, Type.length, &Type);
}

method_type_name(A) ::= MAP(Type). {
    A = tbs_make_type_name(ctx, "map", 3, &Type);
}

method_type_name(A) ::= CLASS(Type). {
    A = tbs_make_type_name(ctx, "class", 5, &Type);
}

method_type_name(A) ::= FUNC(Type). {
    A = tbs_make_type_name(ctx, "function", 8, &Type);
}

method_type_name(A) ::= NULL(Type). {
    A = tbs_make_type_name(ctx, "null", 4, &Type);
}

method_param(A) ::= VARIABLE(Name) COLON method_type_name(Type). {
    tbs_node_t *name_node = tbs_node_new(ctx, TBS_NODE_VARIABLE);
    A = tbs_node_new(ctx, TBS_NODE_MEMBER_ACCESS);
    if (A && name_node) {
        name_node->data.variable.name = tbs_strdup(ctx, Name.start, Name.length);
        tbs_node_set_pos(name_node, &Name);
        A->data.member_access.object = name_node;
        A->data.member_access.member =
            tbs_strdup(ctx, Type->data.variable.name, strlen(Type->data.variable.name));
        tbs_node_set_pos(A, &Name);
    }
}

method_param_list_2plus(A) ::= method_param(P) COMMA method_param(Q). {
    A = tbs_node_new(ctx, TBS_NODE_FUNCTION_CALL);
    if (A) {
        A->data.function.name = NULL;
        A->data.function.arg_count = 2;
        A->data.function.args = (tbs_node_t**)mem_alloc(ctx->arena, 2 * sizeof(tbs_node_t*));
        A->data.function.args[0] = P;
        A->data.function.args[1] = Q;
    }
}

method_param_list_2plus(A) ::= method_param_list_2plus(L) COMMA method_param(P). {
    A = L;
    if (A) {
        A->data.function.arg_count++;
        tbs_node_t **new_args = (tbs_node_t**)mem_alloc(ctx->arena,
            A->data.function.arg_count * sizeof(tbs_node_t*));
        memcpy(new_args, A->data.function.args,
            (A->data.function.arg_count - 1) * sizeof(tbs_node_t*));
        new_args[A->data.function.arg_count - 1] = P;
        A->data.function.args = new_args;
    }
}

// Instance method definition
method_def(A) ::= PUBLIC(OP) method_def(M). {
    A = M;
    if (A) {
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= PROTECTED(OP) method_def(M). {
    A = M;
    if (A) {
        A->data.method.access_level = TBS_ACCESS_PROTECTED;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= PRIVATE(OP) method_def(M). {
    A = M;
    if (A) {
        A->data.method.access_level = TBS_ACCESS_PRIVATE;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= OVERRIDE(OP) method_def(M). {
    A = M;
    if (A) {
        A->data.method.is_override = 1;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= FINAL(OP) method_def(M). {
    A = M;
    if (A) {
        A->data.method.is_final = 1;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= ABSTRACT(OP) VARIABLE(Name) LPAREN RPAREN SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = NULL;
        A->data.method.arg_count = 0;
        A->data.method.body = NULL;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 1;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= ABSTRACT(OP) VARIABLE(Name) LPAREN method_param(Param) RPAREN SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.method.arg_params[0] = Param;
        A->data.method.arg_count = 1;
        A->data.method.body = NULL;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 1;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= ABSTRACT(OP) VARIABLE(Name) LPAREN method_param_list_2plus(Params) RPAREN SEMICOLON. {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = Params->data.function.args;
        A->data.method.arg_count = Params->data.function.arg_count;
        A->data.method.body = NULL;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 1;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= CONSTRUCTOR(OP) LPAREN RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, "constructor", 11);
        A->data.method.arg_params = NULL;
        A->data.method.arg_count = 0;
        A->data.method.body = Body;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= CONSTRUCTOR(OP) LPAREN method_param(Param) RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, "constructor", 11);
        A->data.method.arg_params = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.method.arg_params[0] = Param;
        A->data.method.arg_count = 1;
        A->data.method.body = Body;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= CONSTRUCTOR(OP) LPAREN method_param_list_2plus(Params) RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, "constructor", 11);
        A->data.method.arg_params = Params->data.function.args;
        A->data.method.arg_count = Params->data.function.arg_count;
        A->data.method.body = Body;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= VARIABLE(Name) LPAREN RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = NULL;
        A->data.method.arg_count = 0;
        A->data.method.body = Body;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &Name);
    }
}

method_def(A) ::= VARIABLE(Name) LPAREN method_param(Param) RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.method.arg_params[0] = Param;
        A->data.method.arg_count = 1;
        A->data.method.body = Body;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &Name);
    }
}

method_def(A) ::= VARIABLE(Name) LPAREN method_param_list_2plus(Params) RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = Params->data.function.args;
        A->data.method.arg_count = Params->data.function.arg_count;
        A->data.method.body = Body;
        A->data.method.is_static = 0;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &Name);
    }
}

// Static method definition
method_def(A) ::= STATIC(OP) VARIABLE(Name) LPAREN RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = NULL;
        A->data.method.arg_count = 0;
        A->data.method.body = Body;
        A->data.method.is_static = 1;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= STATIC(OP) VARIABLE(Name) LPAREN method_param(Param) RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.method.arg_params[0] = Param;
        A->data.method.arg_count = 1;
        A->data.method.body = Body;
        A->data.method.is_static = 1;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

method_def(A) ::= STATIC(OP) VARIABLE(Name) LPAREN method_param_list_2plus(Params) RPAREN block(Body). {
    A = tbs_node_new(ctx, TBS_NODE_METHOD);
    if (A) {
        A->data.method.name = tbs_strdup(ctx, Name.start, Name.length);
        A->data.method.arg_params = Params->data.function.args;
        A->data.method.arg_count = Params->data.function.arg_count;
        A->data.method.body = Body;
        A->data.method.is_static = 1;
        A->data.method.is_abstract = 0;
        A->data.method.access_level = TBS_ACCESS_PUBLIC;
        A->data.method.is_override = 0;
        A->data.method.is_final = 0;
        tbs_node_set_pos(A, &OP);
    }
}

// Instantiation: ClassName(args)
// Note: This uses the same syntax as function calls, disambiguated at eval time
// We handle this in the evaluator by checking if the identifier refers to a class

// 'this' keyword
expr(A) ::= THIS(OP). {
    A = tbs_node_new(ctx, TBS_NODE_THIS);
    tbs_node_set_pos(A, &OP);
}

// 'super' constructor call: super(args)
expr(A) ::= SUPER(OP) LPAREN RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_SUPER);
    if (A) {
        A->data.super_expr.member = NULL;  // NULL means super() constructor
        A->data.super_expr.args = NULL;
        A->data.super_expr.arg_count = 0;
        A->data.super_expr.is_call = 1;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= SUPER(OP) LPAREN expr(Arg) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_SUPER);
    if (A) {
        A->data.super_expr.member = NULL;
        A->data.super_expr.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.super_expr.args[0] = Arg;
        A->data.super_expr.arg_count = 1;
        A->data.super_expr.is_call = 1;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= SUPER(OP) LPAREN expr_list_2plus(Args) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_SUPER);
    if (A) {
        A->data.super_expr.member = NULL;
        A->data.super_expr.args = Args->data.function.args;
        A->data.super_expr.arg_count = Args->data.function.arg_count;
        A->data.super_expr.is_call = 1;
        tbs_node_set_pos(A, &OP);
    }
}

// 'super' member access: super.method
expr(A) ::= SUPER DOT VARIABLE(Member). [MEMBER_PREC] {
    A = tbs_node_new(ctx, TBS_NODE_SUPER);
    if (A) {
        A->data.super_expr.member = tbs_strdup(ctx, Member.start, Member.length);
        A->data.super_expr.args = NULL;
        A->data.super_expr.arg_count = 0;
        A->data.super_expr.is_call = 0;
    }
}

expr(A) ::= SUPER(OP) DOT VARIABLE(Member) LPAREN RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_SUPER);
    if (A) {
        A->data.super_expr.member = tbs_strdup(ctx, Member.start, Member.length);
        A->data.super_expr.args = NULL;
        A->data.super_expr.arg_count = 0;
        A->data.super_expr.is_call = 1;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= SUPER(OP) DOT VARIABLE(Member) LPAREN expr(Arg) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_SUPER);
    if (A) {
        A->data.super_expr.member = tbs_strdup(ctx, Member.start, Member.length);
        A->data.super_expr.args = (tbs_node_t**)mem_alloc(ctx->arena, sizeof(tbs_node_t*));
        A->data.super_expr.args[0] = Arg;
        A->data.super_expr.arg_count = 1;
        A->data.super_expr.is_call = 1;
        tbs_node_set_pos(A, &OP);
    }
}

expr(A) ::= SUPER(OP) DOT VARIABLE(Member) LPAREN expr_list_2plus(Args) RPAREN. {
    A = tbs_node_new(ctx, TBS_NODE_SUPER);
    if (A) {
        A->data.super_expr.member = tbs_strdup(ctx, Member.start, Member.length);
        A->data.super_expr.args = Args->data.function.args;
        A->data.super_expr.arg_count = Args->data.function.arg_count;
        A->data.super_expr.is_call = 1;
        tbs_node_set_pos(A, &OP);
    }
}

// 'instanceof' operator with class value expression: obj instanceof ClassName/bundle.Class
expr(A) ::= expr(Obj) INSTANCEOF(OP) expr(ClassExpr). [INSTANCEOF] {
    A = tbs_node_new(ctx, TBS_NODE_INSTANCEOF);
    if (A) {
        A->data.instanceof_expr.object = Obj;
        A->data.instanceof_expr.class_name =
            (ClassExpr && ClassExpr->type == TBS_NODE_VARIABLE)
                ? ClassExpr->data.variable.name
                : NULL;
        A->data.instanceof_expr.class_expr = ClassExpr;
        tbs_node_set_pos(A, &OP);
    }
}
