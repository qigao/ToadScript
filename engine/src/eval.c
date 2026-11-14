/**
 * @file eval.c
 * @brief core evaluation engine for engine AST
 */

#include <turboscript/engine.h>
#include "internal.h"
#include "../parser/lexer.h"
#include <turboscript/detail/grammar_gen.h>
#include <turboscript/module.h>
#include <turboscript/class.h>
#include "core/tstr.h"
#include <cstl.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

static int eval_qualified_receiver_name(
    const tbs_node_t *node, char *out, size_t out_size,
    size_t *used, const char **root_name) {
    const char *segment = NULL;
    size_t segment_len;

    if (!node || !out || out_size == 0 || !used || *used >= out_size)
        return 0;

    if (node->type == TBS_NODE_VARIABLE) {
        segment = node->data.variable.name;
        if (!segment || !*segment) return 0;
        if (root_name && !*root_name) *root_name = segment;
    } else if (node->type == TBS_NODE_MEMBER_ACCESS) {
        if (!eval_qualified_receiver_name(
                node->data.member_access.object, out, out_size,
                used, root_name) ||
            !node->data.member_access.member ||
            !*node->data.member_access.member)
            return 0;
        if (*used + 1 >= out_size) return 0;
        out[(*used)++] = '.';
        segment = node->data.member_access.member;
    } else {
        return 0;
    }

    segment_len = strlen(segment);
    if (segment_len >= out_size - *used) return 0;
    memcpy(out + *used, segment, segment_len);
    *used += segment_len;
    out[*used] = '\0';
    return 1;
}

static int eval_qualified_member_call_name(
    const tbs_node_t *node, char *out, size_t out_size,
    const char **root_name) {
    size_t used = 0;
    size_t method_len;

    if (root_name) *root_name = NULL;
    if (!node || node->type != TBS_NODE_MEMBER_CALL ||
        !node->data.member_call.object ||
        !node->data.member_call.method ||
        !*node->data.member_call.method ||
        !out || out_size == 0)
        return 0;

    out[0] = '\0';
    if (!eval_qualified_receiver_name(
            node->data.member_call.object, out, out_size,
            &used, root_name))
        return 0;

    method_len = strlen(node->data.member_call.method);
    if (used + 1 >= out_size ||
        method_len >= out_size - used - 1)
        return 0;

    out[used++] = '.';
    memcpy(out + used, node->data.member_call.method, method_len);
    used += method_len;
    out[used] = '\0';
    return 1;
}

static int eval_value_truthy(tbs_value_t value) {
    if (value.type == TBS_VAL_BOOL) return value.data.boolean != 0;
    if (value.type == TBS_VAL_INTEGER) return value.data.integer != 0;
    if (value.type == TBS_VAL_NUMBER) return fabs(value.data.number) > 1e-9;
    if (value.type == TBS_VAL_STRING) return value.data.string.len > 0;
    if (value.type == TBS_VAL_BYTES) return value.data.bytes.len > 0;
    if (value.type == TBS_VAL_UUID) return 1;
    if (value.type == TBS_VAL_DATETIME) return 1;
    if (value.type == TBS_VAL_DATE || value.type == TBS_VAL_TIME ||
        value.type == TBS_VAL_DURATION || value.type == TBS_VAL_DECIMAL) return 1;
    if (value.type == TBS_VAL_BIGINT) return value.data.bigint.text.len > 0;
    if (value.type == TBS_VAL_MONEY || value.type == TBS_VAL_ENUM ||
        value.type == TBS_VAL_FLAGS || value.type == TBS_VAL_OFFSET_DATETIME)
        return 1;
    if (value.type == TBS_VAL_TYPED_ARRAY) return value.data.typed_array.count > 0;
    if (value.type == TBS_VAL_VECTOR) return value.data.vector.size > 0;
    if (value.type == TBS_VAL_LIST) return value.data.list.count > 0;
    if (value.type == TBS_VAL_SET) return value.data.list.count > 0;
    if (tbs_value_is_object_like(&value)) return tbs_map_count(&value) > 0;
    if (value.type == TBS_VAL_FUNCTION || value.type == TBS_VAL_CLASS ||
        value.type == TBS_VAL_INSTANCE || value.type == TBS_VAL_BOUND_METHOD)
        return 1;
    return 0;
}

static int eval_value_is_owned_container(tbs_value_t value) {
    return value.ownership == TBS_VALUE_OWNED &&
           (value.type == TBS_VAL_MAP || value.type == TBS_VAL_OBJECT ||
            value.type == TBS_VAL_LIST || value.type == TBS_VAL_SET);
}

static tbs_value_t eval_promote_string(tstr text, const tbs_node_t *node,
                                          tbs_env_t *env) {
    tbs_value_t result = { .type = TBS_VAL_NULL };
    size_t len;

    if (!text) return throw_error(env, node, "failed to allocate string value");
    len = tstr_len(text);
    if (env) {
        if (tbs_value_copy_to_env(
                tbs_val_str(vstr_from_buf(text, len)), env, &result) != 0) {
            tstr_free(text);
            return result;
        }
    } else {
        char *copy = (char *)mem_alloc(node->arena, len + 1U);
        if (!copy) {
            tstr_free(text);
            return result;
        }
        memcpy(copy, text, len + 1U);
        result = tbs_val_str(vstr_from_buf(copy, len));
    }
    tstr_free(text);
    return result;
}

static int eval_value_text(tbs_value_t value, char *buf, size_t buf_size,
                           const char **out_data, size_t *out_len) {
    int n;
    if (!out_data || !out_len) return 0;
    if (value.type == TBS_VAL_STRING) {
        *out_data = value.data.string.data;
        *out_len = value.data.string.len;
        return 1;
    }
    if (value.type == TBS_VAL_INTEGER) {
        n = snprintf(buf, buf_size, "%lld", (long long)value.data.integer);
    } else if (value.type == TBS_VAL_NUMBER) {
        n = snprintf(buf, buf_size, "%g", value.data.number);
    } else if (value.type == TBS_VAL_BOOL) {
        *out_data = value.data.boolean ? "true" : "false";
        *out_len = value.data.boolean ? 4 : 5;
        return 1;
    } else if (value.type == TBS_VAL_BYTES) {
        n = snprintf(buf, buf_size, "bytes(%zu)", value.data.bytes.len);
    } else if (value.type == TBS_VAL_UUID) {
        if (uuid_format(value.data.uuid, buf, buf_size) != SALTS_OK) return 0;
        *out_data = buf;
        *out_len = strlen(buf);
        return 1;
    } else if (value.type == TBS_VAL_DATETIME) {
        time_t ts = datetime_to_time(&value.data.datetime);
        if (ts == (time_t)-1 || datetime_format_rfc822(ts, buf, buf_size) < 0) return 0;
        *out_data = buf;
        *out_len = strlen(buf);
        return 1;
    } else if (value.type == TBS_VAL_OFFSET_DATETIME) {
        int offset = value.data.offset_datetime.offset_minutes;
        char sign = '+';
        if (offset < 0) {
            sign = '-';
            offset = -offset;
        }
        n = snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                     value.data.offset_datetime.datetime.year,
                     value.data.offset_datetime.datetime.month,
                     value.data.offset_datetime.datetime.day,
                     value.data.offset_datetime.datetime.hour,
                     value.data.offset_datetime.datetime.minute,
                     value.data.offset_datetime.datetime.second,
                     sign, offset / 60, offset % 60);
    } else if (value.type == TBS_VAL_DATE) {
        n = snprintf(buf, buf_size, "%04d-%02d-%02d", value.data.date.year,
                     value.data.date.month, value.data.date.day);
    } else if (value.type == TBS_VAL_TIME) {
        if (value.data.time.millisecond > 0)
            n = snprintf(buf, buf_size, "%02d:%02d:%02d.%03d", value.data.time.hour,
                         value.data.time.minute, value.data.time.second,
                         value.data.time.millisecond);
        else
            n = snprintf(buf, buf_size, "%02d:%02d:%02d", value.data.time.hour,
                         value.data.time.minute, value.data.time.second);
    } else if (value.type == TBS_VAL_DURATION) {
        int64_t rem = value.data.duration_ms < 0 ? -value.data.duration_ms
                                                 : value.data.duration_ms;
        int64_t h = rem / 3600000;
        int64_t m;
        int64_t s;
        rem %= 3600000;
        m = rem / 60000;
        rem %= 60000;
        s = rem / 1000;
        rem %= 1000;
        n = snprintf(buf, buf_size, "%s%lld:%02lld:%02lld.%03lld",
                     value.data.duration_ms < 0 ? "-" : "", (long long)h,
                     (long long)m, (long long)s, (long long)rem);
    } else if (value.type == TBS_VAL_DECIMAL) {
        tbs_decimal_t dec = value.data.decimal;
        char digits[32];
        char *p = digits + sizeof(digits);
        uint64_t mag;
        size_t digit_count;
        size_t pos = 0;
        int negative;
        while (dec.scale > 0 && dec.mantissa % 10 == 0) {
            dec.mantissa /= 10;
            dec.scale--;
        }
        if (dec.mantissa == 0) dec.scale = 0;
        negative = dec.mantissa < 0;
        mag = negative ? (uint64_t)(-(dec.mantissa + 1)) + 1ULL : (uint64_t)dec.mantissa;
        *--p = '\0';
        do {
            *--p = (char)('0' + (mag % 10ULL));
            mag /= 10ULL;
        } while (mag != 0);
        digit_count = strlen(p);
        if (negative) buf[pos++] = '-';
        if (dec.scale == 0) {
            if (pos + digit_count >= buf_size) return 0;
            memcpy(buf + pos, p, digit_count + 1);
        } else if ((size_t)dec.scale >= digit_count) {
            size_t zeros = (size_t)dec.scale - digit_count;
            if (pos + 2 + zeros + digit_count >= buf_size) return 0;
            buf[pos++] = '0';
            buf[pos++] = '.';
            while (zeros-- > 0) buf[pos++] = '0';
            memcpy(buf + pos, p, digit_count);
            pos += digit_count;
            buf[pos] = '\0';
        } else {
            size_t whole = digit_count - (size_t)dec.scale;
            if (pos + digit_count + 1 >= buf_size) return 0;
            memcpy(buf + pos, p, whole);
            pos += whole;
            buf[pos++] = '.';
            memcpy(buf + pos, p + whole, (size_t)dec.scale);
            pos += (size_t)dec.scale;
            buf[pos] = '\0';
        }
        *out_data = buf;
        *out_len = strlen(buf);
        return 1;
    } else if (value.type == TBS_VAL_BIGINT) {
        *out_data = value.data.bigint.text.data ? value.data.bigint.text.data : "";
        *out_len = value.data.bigint.text.len;
        return 1;
    } else if (value.type == TBS_VAL_MONEY) {
        tbs_decimal_t dec = value.data.money.amount;
        char digits[32];
        char *p = digits + sizeof(digits);
        uint64_t mag;
        size_t digit_count;
        size_t pos = 0;
        int negative;
        while (dec.scale > 0 && dec.mantissa % 10 == 0) {
            dec.mantissa /= 10;
            dec.scale--;
        }
        if (dec.mantissa == 0) dec.scale = 0;
        negative = dec.mantissa < 0;
        mag = negative ? (uint64_t)(-(dec.mantissa + 1)) + 1ULL : (uint64_t)dec.mantissa;
        *--p = '\0';
        do {
            *--p = (char)('0' + (mag % 10ULL));
            mag /= 10ULL;
        } while (mag != 0);
        digit_count = strlen(p);
        if (pos + 4 >= buf_size) return 0;
        memcpy(buf + pos, value.data.money.currency, 3);
        pos += 3;
        buf[pos++] = ' ';
        if (negative) buf[pos++] = '-';
        if (dec.scale == 0) {
            if (pos + digit_count >= buf_size) return 0;
            memcpy(buf + pos, p, digit_count + 1);
        } else if ((size_t)dec.scale >= digit_count) {
            size_t zeros = (size_t)dec.scale - digit_count;
            if (pos + 2 + zeros + digit_count >= buf_size) return 0;
            buf[pos++] = '0';
            buf[pos++] = '.';
            while (zeros-- > 0) buf[pos++] = '0';
            memcpy(buf + pos, p, digit_count);
            pos += digit_count;
            buf[pos] = '\0';
        } else {
            size_t whole = digit_count - (size_t)dec.scale;
            if (pos + digit_count + 1 >= buf_size) return 0;
            memcpy(buf + pos, p, whole);
            pos += whole;
            buf[pos++] = '.';
            memcpy(buf + pos, p + whole, (size_t)dec.scale);
            pos += (size_t)dec.scale;
            buf[pos] = '\0';
        }
        *out_data = buf;
        *out_len = strlen(buf);
        return 1;
    } else if (value.type == TBS_VAL_ENUM || value.type == TBS_VAL_FLAGS) {
        if (value.data.enum_val.symbol.data && value.data.enum_val.symbol.len > 0) {
            *out_data = value.data.enum_val.symbol.data;
            *out_len = value.data.enum_val.symbol.len;
            return 1;
        }
        n = snprintf(buf, buf_size, "%lld", (long long)value.data.enum_val.value);
    } else if (value.type == TBS_VAL_NULL) {
        *out_data = "null";
        *out_len = 4;
        return 1;
    } else {
        return 0;
    }
    if (n < 0) return 0;
    *out_data = buf;
    *out_len = (size_t)n;
    return 1;
}

static int eval_safe_point(tbs_env_t *env, tbs_safe_point_kind_t kind,
                           size_t cost) {
    if (!env || !env->safe_point) return 1;
    if (env->safe_point(env->safe_point_user_data, kind, cost) == 0) return 1;
    env->aborted = 1;
    return 0;
}

static int eval_loop_tick(tbs_env_t *env) {
    if (!env) return 1;
    if (env->safe_point) return eval_safe_point(env, TBS_SAFE_POINT_LOOP, 1);
    env->curr_loop_iterations++;
    if (env->curr_loop_iterations > env->max_loop_iterations) {
        env->aborted = 1;
        return 0;
    }
    return 1;
}

tbs_value_t tbs_eval(const tbs_node_t *node, tbs_env_t *env) {
    tbs_value_t zero = { .type = TBS_VAL_NUMBER, .data.number = 0.0 };
    if (!node || (env && env->aborted)) return zero;

    if (env) {
        // Track last evaluated node position for error reporting
        if (node->line > 0) {
            env->last_line = node->line;
            env->last_column = node->column;
        }

        if (env->safe_point) {
            if (!eval_safe_point(env, TBS_SAFE_POINT_STEP, 1)) return zero;
        } else {
            // Node Count Limit
            env->curr_nodes++;
            if (env->curr_nodes > env->max_nodes) {
                env->aborted = 1;
                return zero;
            }
        }
    }

    switch (node->type) {
        case TBS_NODE_NUMBER:
            return tbs_val_num(node->data.number);
        case TBS_NODE_INTEGER:
            return tbs_val_int(node->data.integer);
        case TBS_NODE_STRING:
            return tbs_val_str(node->data.string.value);
        case TBS_NODE_TEMPLATE_STRING: {
            const char *str = node->data.template_string.template_str;
            size_t len = node->data.template_string.len;
            tstr result_buf = tstr_new();
            size_t result_len = 0;
            if (!result_buf)
                return throw_error(env, node, "failed to allocate template string");

#define APPEND_STR(s, slen) do { \
    tstr next_buf = tstr_cat_len(result_buf, (s), (slen)); \
    if (!next_buf) { \
        tstr_free(result_buf); \
        return throw_error(env, node, "failed to grow template string"); \
    } \
    result_buf = next_buf; \
    result_len = tstr_len(result_buf); \
} while(0)

            size_t i = 0;
            while (i < len) {
                if (str[i] == '$' && i + 1 < len && str[i+1] == '{') {
                    // Start of expression
                    i += 2;
                    size_t expr_start = i;
                    int depth = 1;
                    while (i < len && depth > 0) {
                        if (str[i] == '{') depth++;
                        else if (str[i] == '}') depth--;
                        i++;
                    }
                    if (depth == 0) {
                        size_t expr_len = i - 1 - expr_start;
                        if (expr_len > 0) {
                            char *expr_str = (char*)malloc(expr_len + 1);
                            memcpy(expr_str, str + expr_start, expr_len);
                            expr_str[expr_len] = '\0';

                            // Parse and evaluate the inner expression
                            tbs_node_t *expr_node = tbs_parse(expr_str, expr_len);
                            if (expr_node) {
                                tbs_value_t expr_val = tbs_eval(expr_node, env);
                                if (expr_val.type == TBS_VAL_INTEGER) {
                                    char num_buf[64];
                                    snprintf(num_buf, sizeof(num_buf), "%lld", (long long)expr_val.data.integer);
                                    APPEND_STR(num_buf, strlen(num_buf));
                                } else if (expr_val.type == TBS_VAL_BOOL) {
                                    if (expr_val.data.boolean) APPEND_STR("true", 4);
                                    else APPEND_STR("false", 5);
                                } else if (expr_val.type == TBS_VAL_NUMBER) {
                                    char num_buf[64];
                                    snprintf(num_buf, sizeof(num_buf), "%g", expr_val.data.number);
                                    APPEND_STR(num_buf, strlen(num_buf));
                                } else if (expr_val.type == TBS_VAL_STRING) {
                                    APPEND_STR(expr_val.data.string.data, expr_val.data.string.len);
                                } else if (expr_val.type == TBS_VAL_BYTES) {
                                    char bytes_buf[64];
                                    int bytes_len = snprintf(bytes_buf, sizeof(bytes_buf), "bytes(%zu)", expr_val.data.bytes.len);
                                    if (bytes_len > 0) APPEND_STR(bytes_buf, (size_t)bytes_len);
                                } else if (expr_val.type == TBS_VAL_UUID) {
                                    char uuid_buf[SALTS_UUID_STRING_SIZE];
                                    if (uuid_format(expr_val.data.uuid, uuid_buf,
                                                          sizeof(uuid_buf)) == SALTS_OK)
                                        APPEND_STR(uuid_buf, strlen(uuid_buf));
                                } else if (expr_val.type == TBS_VAL_DATE ||
                                           expr_val.type == TBS_VAL_TIME ||
                                           expr_val.type == TBS_VAL_DURATION ||
                                           expr_val.type == TBS_VAL_DECIMAL) {
                                    char value_buf[64];
                                    const char *value_text = NULL;
                                    size_t value_len = 0;
                                    if (eval_value_text(expr_val, value_buf, sizeof(value_buf),
                                                        &value_text, &value_len))
                                        APPEND_STR(value_text, value_len);
                                } else if (expr_val.type == TBS_VAL_NULL) {
                                    APPEND_STR("null", 4);
                                } else if (expr_val.type == TBS_VAL_VECTOR) {
                                    APPEND_STR("[vector]", 8);
                                } else if (expr_val.type == TBS_VAL_MAP) {
                                    APPEND_STR("[map]", 5);
                                } else if (expr_val.type == TBS_VAL_OBJECT) {
                                    APPEND_STR("[object]", 8);
                                } else if (expr_val.type == TBS_VAL_LIST) {
                                    APPEND_STR("[list]", 6);
                                }
                                tbs_value_destroy(&expr_val);
                                tbs_free(expr_node);
                            }
                            free(expr_str);
                        }
                    }
                } else {
                    // Normal character
                    APPEND_STR(&str[i], 1);
                    i++;
                }
            }
#undef APPEND_STR
            (void)result_len;
            return eval_promote_string(result_buf, node, env);
        }
        case TBS_NODE_VARIABLE: {
            const char *name = node->data.variable.name;
            return tbs_env_get(env, name);
        }
        case TBS_NODE_SPREAD:
            return tbs_eval(node->data.spread.child, env);
        case TBS_NODE_DESTRUCTURING_ASSIGNMENT: {
            tbs_value_t rhs = tbs_eval(node->data.destructuring.value, env);
            eval_destructure(node->data.destructuring.targets, rhs, env, node->data.destructuring.is_constant);
            return rhs;
        }
        case TBS_NODE_ASSIGNMENT: {
            tbs_value_t val = tbs_eval(node->data.assignment.value, env);
            tbs_env_set(env, node->data.assignment.name, val);
            if (eval_value_is_owned_container(val)) tbs_value_destroy(&val);
            return tbs_env_get(env, node->data.assignment.name);
        }
        case TBS_NODE_CONSTANT_DECL: {
            tbs_value_t val = tbs_eval(node->data.assignment.value, env);
            tbs_env_set_constant(env, node->data.assignment.name, val);
            if (eval_value_is_owned_container(val)) tbs_value_destroy(&val);
            return tbs_env_get(env, node->data.assignment.name);
        }
        case TBS_NODE_IF: {
            tbs_value_t cond_val = tbs_eval(node->data.if_stmt.condition, env);
            if (env && env->flow != tbs_FLOW_NORMAL) return zero;

            if (eval_value_truthy(cond_val)) {
                if (node->data.if_stmt.if_branch) return tbs_eval(node->data.if_stmt.if_branch, env);
            } else {
                if (node->data.if_stmt.else_branch) return tbs_eval(node->data.if_stmt.else_branch, env);
            }
            return zero;
        }
        case TBS_NODE_WHILE: {
            tbs_value_t last_val = zero;
            while (1) {
                if (env && env->flow != tbs_FLOW_NORMAL && env->flow != tbs_FLOW_CONTINUE) break;
                if (env && env->flow == tbs_FLOW_CONTINUE) env->flow = tbs_FLOW_NORMAL;

                tbs_value_t cond_val = tbs_eval(node->data.while_loop.condition, env);
                if (env && (env->flow != tbs_FLOW_NORMAL || env->aborted)) break;

                if (!eval_value_truthy(cond_val)) break;

                if (env && !eval_loop_tick(env)) break;

                last_val = tbs_eval(node->data.while_loop.body, env);
                if (env && env->flow == tbs_FLOW_BREAK) {
                    env->flow = tbs_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == tbs_FLOW_RETURN) break;
            }
            return last_val;
        }
        case TBS_NODE_FOR: {
            if (node->data.for_loop.init) tbs_eval(node->data.for_loop.init, env);
            if (env && (env->flow != tbs_FLOW_NORMAL || env->aborted)) return zero;

            tbs_value_t last_val = zero;
            while (1) {
                if (node->data.for_loop.condition) {
                    tbs_value_t cond_val = tbs_eval(node->data.for_loop.condition, env);
                    if (env && (env->flow != tbs_FLOW_NORMAL || env->aborted)) break;
                    if (!eval_value_truthy(cond_val)) break;
                }

                if (env && !eval_loop_tick(env)) break;

                last_val = tbs_eval(node->data.for_loop.body, env);
                if (env && env->flow == tbs_FLOW_BREAK) {
                    env->flow = tbs_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == tbs_FLOW_RETURN) break;

                if (node->data.for_loop.post) {
                    tbs_eval(node->data.for_loop.post, env);
                    if (env && (env->flow != tbs_FLOW_NORMAL && env->flow != tbs_FLOW_CONTINUE)) break;
                    if (env && env->flow == tbs_FLOW_CONTINUE) env->flow = tbs_FLOW_NORMAL;
                }
            }
            return last_val;
        }
        case TBS_NODE_BLOCK: {
            tbs_value_t last_val = zero;
            for (size_t i = 0; i < node->data.block.count; ++i) {
                last_val = tbs_eval(node->data.block.statements[i], env);
                if (env && env->flow != tbs_FLOW_NORMAL) break;
                if (env && env->aborted) break;
            }
            return last_val;
        }
        case TBS_NODE_FLOW: {
            if (env) {
                if (node->data.flow.type == tbs_TOKEN_RETURN) {
                    if (node->data.flow.value) {
                        env->return_value = tbs_eval(node->data.flow.value, env);
                        if (env->flow != tbs_FLOW_NORMAL || env->aborted) {
                            return env->return_value;
                        }
                    } else {
                        env->return_value = zero;
                    }
                    env->flow = tbs_FLOW_RETURN;
                    return env->return_value;
                } else if (node->data.flow.type == tbs_TOKEN_BREAK) {
                    env->flow = tbs_FLOW_BREAK;
                } else if (node->data.flow.type == tbs_TOKEN_CONTINUE) {
                    env->flow = tbs_FLOW_CONTINUE;
                }
            }
            return zero;
        }
        case TBS_NODE_BINARY_OP: {
            tbs_value_t l_val = { .type = TBS_VAL_NUMBER, .data.number = 0.0 };
            if (node->data.binary.left) {
                l_val = tbs_eval(node->data.binary.left, env);
                if (env && env->flow != tbs_FLOW_NORMAL) return zero;
            }

            tbs_value_t r_val = tbs_eval(node->data.binary.right, env);
            if (env && env->flow != tbs_FLOW_NORMAL) return zero;

            // INTEGER + INTEGER → INTEGER
            if (l_val.type == TBS_VAL_INTEGER && r_val.type == TBS_VAL_INTEGER) {
                int64_t l = l_val.data.integer;
                int64_t r = r_val.data.integer;
                switch (node->data.binary.op) {
                    case tbs_TOKEN_PLUS:     return tbs_val_int(l + r);
                    case tbs_TOKEN_MINUS:    return tbs_val_int(l - r);
                    case tbs_TOKEN_MULTIPLY: return tbs_val_int(l * r);
                    case tbs_TOKEN_DIVIDE:
                        if (r == 0) return throw_error(env, node, "Division by zero");
                        if (l % r == 0) return tbs_val_int(l / r);
                        return tbs_val_num((double)l / (double)r);
                    case tbs_TOKEN_MOD:
                        if (r == 0) return throw_error(env, node, "Modulo by zero");
                        return tbs_val_int(l % r);
                    case tbs_TOKEN_POWER:    return tbs_val_num(pow((double)l, (double)r));
                    case tbs_TOKEN_EQ:       return tbs_val_int(l == r);
                    case tbs_TOKEN_NE:       return tbs_val_int(l != r);
                    case tbs_TOKEN_LT:       return tbs_val_int(l < r);
                    case tbs_TOKEN_LE:       return tbs_val_int(l <= r);
                    case tbs_TOKEN_GT:       return tbs_val_int(l > r);
                    case tbs_TOKEN_GE:       return tbs_val_int(l >= r);
                    case tbs_TOKEN_AND:      return tbs_val_int(l && r);
                    case tbs_TOKEN_OR:       return tbs_val_int(l || r);
                }
            }

            // (INTEGER | NUMBER | BOOL) + (INTEGER | NUMBER | BOOL) -> NUMBER
            if ((l_val.type == TBS_VAL_INTEGER || l_val.type == TBS_VAL_NUMBER ||
                 l_val.type == TBS_VAL_BOOL) &&
                (r_val.type == TBS_VAL_INTEGER || r_val.type == TBS_VAL_NUMBER ||
                 r_val.type == TBS_VAL_BOOL)) {
                double l = val_to_double(l_val);
                double r = val_to_double(r_val);
                switch (node->data.binary.op) {
                    case tbs_TOKEN_PLUS:     return tbs_val_num(l + r);
                    case tbs_TOKEN_MINUS:    return tbs_val_num(l - r);
                    case tbs_TOKEN_MULTIPLY: return tbs_val_num(l * r);
                    case tbs_TOKEN_DIVIDE:
                        if (fabs(r) < 1e-15) return throw_error(env, node, "Division by zero");
                        return tbs_val_num(l / r);
                    case tbs_TOKEN_MOD:
                        if (fabs(r) < 1e-15) return throw_error(env, node, "Modulo by zero");
                        return tbs_val_num(fmod(l, r));
                    case tbs_TOKEN_POWER:    return tbs_val_num(pow(l, r));
                    case tbs_TOKEN_EQ:       return tbs_val_num(fabs(l - r) < 1e-9);
                    case tbs_TOKEN_NE:       return tbs_val_num(fabs(l - r) >= 1e-9);
                    case tbs_TOKEN_LT:       return tbs_val_num(l < r);
                    case tbs_TOKEN_LE:       return tbs_val_num(l <= r);
                    case tbs_TOKEN_GT:       return tbs_val_num(l > r);
                    case tbs_TOKEN_GE:       return tbs_val_num(l >= r);
                    case tbs_TOKEN_AND:      return tbs_val_num(fabs(l) > 1e-9 && fabs(r) > 1e-9);
                    case tbs_TOKEN_OR:       return tbs_val_num(fabs(l) > 1e-9 || fabs(r) > 1e-9);
                }
            }

            // STRING + ANY → STRING (concatenation)
            if (node->data.binary.op == tbs_TOKEN_PLUS &&
                (l_val.type == TBS_VAL_STRING || r_val.type == TBS_VAL_STRING)) {
                char n_buf[64], r_buf[64];
                const char *l_data, *r_data;
                size_t l_len, r_len;

                if (!eval_value_text(l_val, n_buf, sizeof(n_buf), &l_data, &l_len) ||
                    !eval_value_text(r_val, r_buf, sizeof(r_buf), &r_data, &r_len))
                    return throw_error(env, node, "Type error: cannot stringify %s and %s",
                                       type_name(l_val.type), type_name(r_val.type));

                if (l_len > SIZE_MAX - r_len)
                    return throw_error(env, node, "String concatenation is too large");
                size_t new_len = l_len + r_len;
                tstr joined = tstr_new_len(NULL, new_len);
                if (!joined)
                    return throw_error(env, node, "failed to allocate concatenated string");
                memcpy(joined, l_data, l_len);
                memcpy(joined + l_len, r_data, r_len);
                joined[new_len] = '\0';
                tbs_value_t result = eval_promote_string(joined, node, env);
                tbs_value_destroy(&l_val);
                tbs_value_destroy(&r_val);
                return result;
            }

            // STRING == STRING
            if (l_val.type == TBS_VAL_STRING && r_val.type == TBS_VAL_STRING) {
                if (node->data.binary.op == tbs_TOKEN_EQ) {
                    return tbs_val_num(vstr_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                }
                if (node->data.binary.op == tbs_TOKEN_NE) {
                    return tbs_val_num(!vstr_eq(l_val.data.string, r_val.data.string) ? 1.0 : 0.0);
                }
            }

            if (l_val.type == TBS_VAL_BOOL && r_val.type == TBS_VAL_BOOL) {
                if (node->data.binary.op == tbs_TOKEN_EQ)
                    return tbs_val_num(l_val.data.boolean == r_val.data.boolean ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE)
                    return tbs_val_num(l_val.data.boolean != r_val.data.boolean ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_BYTES && r_val.type == TBS_VAL_BYTES) {
                int equal = l_val.data.bytes.len == r_val.data.bytes.len &&
                            (l_val.data.bytes.len == 0 ||
                             memcmp(l_val.data.bytes.data, r_val.data.bytes.data,
                                    l_val.data.bytes.len) == 0);
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_UUID && r_val.type == TBS_VAL_UUID) {
                int equal = memcmp(l_val.data.uuid, r_val.data.uuid,
                                   sizeof(l_val.data.uuid)) == 0;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_DATETIME && r_val.type == TBS_VAL_DATETIME) {
                int equal = memcmp(&l_val.data.datetime, &r_val.data.datetime,
                                   sizeof(l_val.data.datetime)) == 0;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_OFFSET_DATETIME &&
                r_val.type == TBS_VAL_OFFSET_DATETIME) {
                int equal = l_val.data.offset_datetime.offset_minutes ==
                                r_val.data.offset_datetime.offset_minutes &&
                            memcmp(&l_val.data.offset_datetime.datetime,
                                   &r_val.data.offset_datetime.datetime,
                                   sizeof(l_val.data.offset_datetime.datetime)) == 0;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_DATE && r_val.type == TBS_VAL_DATE) {
                int equal = l_val.data.date.year == r_val.data.date.year &&
                            l_val.data.date.month == r_val.data.date.month &&
                            l_val.data.date.day == r_val.data.date.day;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_TIME && r_val.type == TBS_VAL_TIME) {
                int equal = l_val.data.time.hour == r_val.data.time.hour &&
                            l_val.data.time.minute == r_val.data.time.minute &&
                            l_val.data.time.second == r_val.data.time.second &&
                            l_val.data.time.millisecond == r_val.data.time.millisecond;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_DURATION && r_val.type == TBS_VAL_DURATION) {
                int equal = l_val.data.duration_ms == r_val.data.duration_ms;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_DECIMAL && r_val.type == TBS_VAL_DECIMAL) {
                tbs_decimal_t l = l_val.data.decimal;
                tbs_decimal_t r = r_val.data.decimal;
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
                int equal = l.mantissa == r.mantissa && l.scale == r.scale;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_BIGINT && r_val.type == TBS_VAL_BIGINT) {
                int equal = vstr_eq(l_val.data.bigint.text, r_val.data.bigint.text);
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if (l_val.type == TBS_VAL_MONEY && r_val.type == TBS_VAL_MONEY) {
                int equal = l_val.data.money.amount.mantissa == r_val.data.money.amount.mantissa &&
                            l_val.data.money.amount.scale == r_val.data.money.amount.scale &&
                            memcmp(l_val.data.money.currency, r_val.data.money.currency, 4) == 0;
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            if ((l_val.type == TBS_VAL_ENUM || l_val.type == TBS_VAL_FLAGS) &&
                l_val.type == r_val.type) {
                int equal = l_val.data.enum_val.value == r_val.data.enum_val.value &&
                            vstr_eq(l_val.data.enum_val.type_name, r_val.data.enum_val.type_name);
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(equal ? 1.0 : 0.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(!equal ? 1.0 : 0.0);
            }

            // NULL == NULL
            if (l_val.type == TBS_VAL_NULL && r_val.type == TBS_VAL_NULL) {
                if (node->data.binary.op == tbs_TOKEN_EQ) return tbs_val_num(1.0);
                if (node->data.binary.op == tbs_TOKEN_NE) return tbs_val_num(0.0);
            }

            // NULL == ANY (not NULL) → false
            if ((l_val.type == TBS_VAL_NULL || r_val.type == TBS_VAL_NULL) &&
                (node->data.binary.op == tbs_TOKEN_EQ || node->data.binary.op == tbs_TOKEN_NE)) {
                return tbs_val_num(node->data.binary.op == tbs_TOKEN_NE ? 1.0 : 0.0);
            }

            // Type error: unsupported operation
            const char *op_name = "unknown";
            switch (node->data.binary.op) {
                case tbs_TOKEN_PLUS: op_name = "+"; break;
                case tbs_TOKEN_MINUS: op_name = "-"; break;
                case tbs_TOKEN_MULTIPLY: op_name = "*"; break;
                case tbs_TOKEN_DIVIDE: op_name = "/"; break;
                case tbs_TOKEN_MOD: op_name = "%"; break;
                case tbs_TOKEN_POWER: op_name = "**"; break;
                case tbs_TOKEN_LT: op_name = "<"; break;
                case tbs_TOKEN_LE: op_name = "<="; break;
                case tbs_TOKEN_GT: op_name = ">"; break;
                case tbs_TOKEN_GE: op_name = ">="; break;
                case tbs_TOKEN_AND: op_name = "&&"; break;
                case tbs_TOKEN_OR: op_name = "||"; break;
                default: break;
            }
            return throw_error(env, node, "Type error: cannot apply '%s' to %s and %s",
                             op_name, type_name(l_val.type), type_name(r_val.type));
        }
        case TBS_NODE_FUNCTION_CALL: {
            if (!node->data.function.name) return zero;

            tbs_value_t callee = tbs_env_get(env, node->data.function.name);
            if (callee.type == TBS_VAL_CLASS) {
                return eval_class_value_instantiation(callee.data.class_val.klass,
                                                      node->data.function.name,
                                                      node->data.function.args,
                                                      node->data.function.arg_count,
                                                      env);
            }

            // Regular function call
            size_t actual_count = 0;
            tbs_value_t *args = eval_expand_args(node->data.function.args, node->data.function.arg_count, env, &actual_count);
            if (!args && actual_count == 0) return zero;
            if (callee.type == TBS_VAL_BOUND_METHOD) {
                tbs_value_t result = eval_instance_script_method(
                    callee.data.bound_method_val.instance,
                    callee.data.bound_method_val.method,
                    actual_count, args, env);
                tbs_values_destroy(args, actual_count);
                free(args);
                return result;
            }
            if (callee.type == TBS_VAL_FUNCTION && callee.data.function.body) {
                tbs_value_t result =
                    tbs_call_function_value(callee, actual_count, args, env);
                tbs_values_destroy(args, actual_count);
                free(args);
                return result;
            }

            tbs_value_t result = tbs_call_internal(node->data.function.name, actual_count, args, env);
            tbs_values_destroy(args, actual_count);
            free(args);
            return result;
        }
        case TBS_NODE_NEW:
            return eval_class_instantiation(node->data.new_expr.class_name,
                                            node->data.new_expr.args,
                                            node->data.new_expr.arg_count,
                                            env);
        case TBS_NODE_FUNCTION_DEFINITION: {
            const char *name = node->data.func_def.name;
            if (!name) return zero;

            tbs_func_t *curr = env->funcs;
            while (curr) {
                if (curr->name && strcmp(curr->name, name) == 0) break;
                curr = curr->next;
            }
            if (!curr) {
                curr = (tbs_func_t*)calloc(1, sizeof(tbs_func_t));
                curr->name = strdup(name);
                curr->next = env->funcs;
                env->funcs = curr;
            }
            curr->is_script = 1;
            curr->access_level = TBS_ACCESS_PUBLIC;
            curr->data.script.arg_count = node->data.func_def.arg_count;
            if (curr->data.script.arg_count > 0) {
                curr->data.script.arg_params = (tbs_node_t**)calloc(curr->data.script.arg_count, sizeof(tbs_node_t*));
                for (size_t i = 0; i < curr->data.script.arg_count; ++i) {
                    curr->data.script.arg_params[i] = tbs_node_copy(node->data.func_def.arg_params[i], &env->arena);
                }
            } else {
                curr->data.script.arg_params = NULL;
            }
            curr->data.script.body = tbs_node_copy(node->data.func_def.body, &env->arena);
            return zero;
        }
        case TBS_NODE_MEMBER_CALL: {
            size_t mc_argc = 0;
            char qualified_name[256];
            const char *qualified_root = NULL;
            tbs_value_t *mc_args = eval_expand_args(
                node->data.member_call.args, node->data.member_call.arg_count, env, &mc_argc);
            if (!mc_args && mc_argc == 0) return zero;

            /*
             * Native/plugin functions may use multi-segment qualified IDs
             * such as Image.Codec.Decode. Resolve the pure syntax namespace
             * chain directly only when its root is not a runtime value; real
             * object/map/class receivers keep the normal member semantics.
             */
            if (eval_qualified_member_call_name(
                    node, qualified_name, sizeof(qualified_name),
                    &qualified_root) &&
                qualified_root &&
                !tbs_env_has(env, qualified_root) &&
                tbs_env_has_func(env, qualified_name)) {
                tbs_value_t result =
                    tbs_call_internal(qualified_name, mc_argc, mc_args, env);
                tbs_values_destroy(mc_args, mc_argc);
                free(mc_args);
                return result;
            }

            mc_ctx_t mc = {
                .method   = node->data.member_call.method,
                .obj      = tbs_eval(node->data.member_call.object, env),
                .args     = mc_args,
                .argc     = mc_argc,
                .obj_node = node->data.member_call.object,
                .env      = env,
                .arena    = node->arena,
            };

            tbs_value_t mc_result;
            switch (mc.obj.type) {
                case TBS_VAL_LIST:
                case TBS_VAL_SET:    mc_result = eval_list_method(&mc);   break;
                case TBS_VAL_MAP:
                case TBS_VAL_OBJECT: mc_result = eval_map_method(&mc);    break;
                case TBS_VAL_BYTES:  mc_result = eval_bytes_method(&mc);  break;
                case TBS_VAL_UUID:   mc_result = eval_uuid_method(&mc);   break;
                case TBS_VAL_DATETIME: mc_result = eval_datetime_method(&mc); break;
                case TBS_VAL_DATE:   mc_result = eval_date_method(&mc);   break;
                case TBS_VAL_TIME:   mc_result = eval_time_method(&mc);   break;
                case TBS_VAL_DURATION: mc_result = eval_duration_method(&mc); break;
                case TBS_VAL_DECIMAL: mc_result = eval_decimal_method(&mc); break;
                case TBS_VAL_OFFSET_DATETIME: mc_result = eval_offset_datetime_method(&mc); break;
                case TBS_VAL_TYPED_ARRAY: mc_result = eval_typed_array_method(&mc); break;
                case TBS_VAL_STRING: mc_result = eval_string_method(&mc); break;
                case TBS_VAL_VECTOR: mc_result = eval_vector_method(&mc); break;
                // OOP: Instance method call
                case TBS_VAL_INSTANCE: {
                    tbs_instance_t *instance = mc.obj.data.instance_val.instance;
                    tbs_func_t *method =
                        tbs_class_lookup_method_typed(instance->klass, mc.method, 0,
                                                         mc_argc, mc_args);
                    if (!method) {
                        mc_result = throw_error(env, node, "Instance has no method '%s'", mc.method);
                    } else if (!can_access_method(env, method, mc.obj_node)) {
                        mc_result = throw_method_access_error(env, node, mc.method, method);
                    } else {
                        mc_result = eval_instance_script_method(instance, method,
                                                                mc_argc, mc_args, env);
                    }
                    break;
                }
                // OOP: Bound method call (when method was stored in a variable)
                case TBS_VAL_BOUND_METHOD: {
                    tbs_instance_t *instance = mc.obj.data.bound_method_val.instance;
                    tbs_func_t *method = mc.obj.data.bound_method_val.method;

                    mc_result = eval_instance_script_method(instance, method,
                                                            mc_argc, mc_args, env);
                    break;
                }
                // OOP: Static class method call
                case TBS_VAL_CLASS: {
                    tbs_class_t *klass = mc.obj.data.class_val.klass;
                    tbs_func_t *method =
                        tbs_class_lookup_method_typed(klass, mc.method, 1, mc_argc, mc_args);
                    if (!method) {
                        mc_result = throw_error(env, node, "Class has no static method '%s'", mc.method);
                    } else if (!can_access_method(env, method, mc.obj_node)) {
                        mc_result = throw_method_access_error(env, node, mc.method, method);
                    } else {
                        mc_result = eval_script_function(method, mc_argc, mc_args, env, env);
                    }
                    break;
                }
                default:
                    if (mc.obj_node->type == TBS_NODE_VARIABLE) {
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s.%s",
                                 mc.obj_node->data.variable.name, mc.method);
                        mc_result = tbs_call_internal(full_name, mc_argc, mc_args, env);
                    } else {
                        mc_result = throw_error(env, node, "Method call '%s' is invalid for %s",
                                                mc.method ? mc.method : "<null>",
                                                type_name(mc.obj.type));
                    }
                    break;
            }
            tbs_values_destroy(mc_args, mc_argc);
            free(mc_args);
            return mc_result;
        }
        case TBS_NODE_DO_WHILE: {
            tbs_value_t last_val = zero;
            while (1) {
                last_val = tbs_eval(node->data.do_while.body, env);
                if (env && env->flow == tbs_FLOW_BREAK) {
                    env->flow = tbs_FLOW_NORMAL;
                    break;
                }
                if (env && env->flow == tbs_FLOW_RETURN) break;
                if (env && env->flow == tbs_FLOW_CONTINUE) {
                    env->flow = tbs_FLOW_NORMAL;
                }

                tbs_value_t cond_val = tbs_eval(node->data.do_while.condition, env);
                if (env && (env->flow != tbs_FLOW_NORMAL || env->aborted)) break;

                if (!eval_value_truthy(cond_val)) break;

                if (env && !eval_loop_tick(env)) break;
            }
            return last_val;
        }
        case TBS_NODE_VECTOR: {
            size_t actual_count = 0;
            tbs_value_t *vals = eval_expand_args(node->data.vector.elements, node->data.vector.count, env, &actual_count);
            if (!vals && actual_count == 0) return zero;
            if (actual_count == 0) { free(vals); return zero; }

            vec_t vector_data = {0};
            if (vec_init_bytes(&vector_data, sizeof(double), _Alignof(double),
                               actual_count) != STL_OK) {
                tbs_values_destroy(vals, actual_count);
                free(vals);
                return throw_error(env, node, "failed to allocate vector value");
            }
            if (vec_reserve(&vector_data, actual_count) != STL_OK) {
                vec_destroy(&vector_data);
                tbs_values_destroy(vals, actual_count);
                free(vals);
                return throw_error(env, node, "failed to allocate vector value");
            }
            for (size_t i = 0; i < actual_count; ++i) {
                double element = val_to_double(vals[i]);
                if (vec_push(&vector_data, &element) != STL_OK) {
                    vec_destroy(&vector_data);
                    tbs_values_destroy(vals, actual_count);
                    free(vals);
                    return throw_error(env, node, "failed to build vector value");
                }
            }
            tbs_value_t result = zero;
            tbs_value_t borrowed = tbs_val_vec(
                (double *)vector_data.data, vector_data.size);
            if (tbs_value_copy_to_env(borrowed, env, &result) != 0) {
                vec_destroy(&vector_data);
                tbs_values_destroy(vals, actual_count);
                free(vals);
                return zero;
            }
            vec_destroy(&vector_data);
            tbs_values_destroy(vals, actual_count);
            free(vals);
            return result;
        }
        case TBS_NODE_INDEX: {
            tbs_value_t arr = tbs_eval(node->data.index_access.array, env);
            tbs_value_t idx_val = tbs_eval(node->data.index_access.index, env);

            if (arr.type == TBS_VAL_VECTOR && (idx_val.type == TBS_VAL_NUMBER || idx_val.type == TBS_VAL_INTEGER)) {
                int idx = (idx_val.type == TBS_VAL_INTEGER) ? (int)idx_val.data.integer : (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.vector.size) {
                    return throw_error(env, node, "Array index %d out of bounds [0, %zu)", idx, arr.data.vector.size);
                }
                return tbs_val_num(arr.data.vector.data[idx]);
            }
            /* List index: l[i] → any value */
            if ((arr.type == TBS_VAL_LIST || arr.type == TBS_VAL_SET) &&
                (idx_val.type == TBS_VAL_NUMBER || idx_val.type == TBS_VAL_INTEGER)) {
                int idx = (idx_val.type == TBS_VAL_INTEGER) ? (int)idx_val.data.integer : (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.list.count) {
                    return throw_error(env, node, "List index %d out of bounds [0, %zu)", idx, arr.data.list.count);
                }
                return tbs_value_borrow(arr.data.list.items[idx]);
            }
            if (arr.type == TBS_VAL_TYPED_ARRAY &&
                (idx_val.type == TBS_VAL_NUMBER || idx_val.type == TBS_VAL_INTEGER)) {
                int idx = (idx_val.type == TBS_VAL_INTEGER) ? (int)idx_val.data.integer : (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.typed_array.count) {
                    return throw_error(env, node, "Typed array index %d out of bounds [0, %zu)",
                                       idx, arr.data.typed_array.count);
                }
                return tbs_typed_array_get_value(arr, (size_t)idx);
            }
            /* Bytes index: b[i] -> byte value */
            if (arr.type == TBS_VAL_BYTES && (idx_val.type == TBS_VAL_NUMBER || idx_val.type == TBS_VAL_INTEGER)) {
                int idx = (idx_val.type == TBS_VAL_INTEGER) ? (int)idx_val.data.integer : (int)idx_val.data.number;
                if (idx < 0 || idx >= (int)arr.data.bytes.len) {
                    return throw_error(env, node, "Bytes index %d out of bounds [0, %zu)", idx, arr.data.bytes.len);
                }
                return tbs_val_int((unsigned char)arr.data.bytes.data[idx]);
            }
            /* Dynamic map index: m["key"] */
            if (tbs_value_is_object_like(&arr) && idx_val.type == TBS_VAL_STRING) {
                return tbs_map_get(&arr, idx_val.data.string.data);
            }

            // Type error
            return throw_error(env, node, "Invalid indexing: expected vector[number], list[number], typed_array[number], or map[string], got %s[%s]",
                             type_name(arr.type), type_name(idx_val.type));
        }
        case TBS_NODE_SLICE: {
            tbs_value_t arr = tbs_eval(node->data.slice.array, env);
            tbs_value_t start_val = tbs_eval(node->data.slice.start, env);
            tbs_value_t end_val = tbs_eval(node->data.slice.end, env);
            if (arr.type != TBS_VAL_VECTOR ||
                (start_val.type != TBS_VAL_NUMBER && start_val.type != TBS_VAL_INTEGER) ||
                (end_val.type != TBS_VAL_NUMBER && end_val.type != TBS_VAL_INTEGER)) {
                return throw_error(env, node,
                                   "Invalid slice: expected vector[number:number], got %s[%s:%s]",
                                   type_name(arr.type), type_name(start_val.type), type_name(end_val.type));
            }
            int start = (start_val.type == TBS_VAL_INTEGER) ? (int)start_val.data.integer : (int)start_val.data.number;
            int end = (end_val.type == TBS_VAL_INTEGER) ? (int)end_val.data.integer : (int)end_val.data.number;
            if (start < 0) start = 0;
            if (end > (int)arr.data.vector.size) end = (int)arr.data.vector.size;
            if (start > end) return throw_error(env, node, "Invalid slice range [%d:%d]", start, end);
            size_t count = (size_t)(end - start);
            tbs_value_t slice = tbs_val_vec(
                count ? arr.data.vector.data + start : NULL, count);
            if (env) {
                tbs_value_t owned = { .type = TBS_VAL_NULL };
                if (tbs_value_copy_to_env(slice, env, &owned) != 0)
                    return throw_error(env, node, "Out of memory creating slice");
                tbs_value_destroy(&arr);
                return owned;
            }
            double *data = (double*)mem_alloc(node->arena, count * sizeof(double));
            if (!data) return throw_error(env, node, "Out of memory creating slice");
            memcpy(data, slice.data.vector.data, count * sizeof(double));
            return tbs_val_vec(data, count);
        }
        case TBS_NODE_MAP_LITERAL: {
            tbs_value_t map = tbs_val_map();
            for (size_t i = 0; i < node->data.map_literal.count; ++i) {
                if (node->data.map_literal.keys[i] == NULL) {
                    /* Spread operator */
                    tbs_value_t other = tbs_eval(node->data.map_literal.values[i], env);
                    if (tbs_value_is_object_like(&other)) {
                        tbs_map_iter_t it = tbs_map_iter_begin(&other);
                        const char *k;
                        tbs_value_t v;
                        while (tbs_map_iter_next(&it, &k, &v)) {
                            if (tbs_map_set(&map, k, v) != 0) {
                                tbs_value_destroy(&other);
                                tbs_value_destroy(&map);
                                return throw_error(env, node, "failed to spread map value");
                            }
                        }
                    }
                    tbs_value_destroy(&other);
                } else {
                    tbs_value_t val = tbs_eval(node->data.map_literal.values[i], env);
                    if (env && env->flow != tbs_FLOW_NORMAL) return map;
                    if (tbs_map_set(&map, node->data.map_literal.keys[i], val) != 0) {
                        tbs_value_destroy(&val);
                        tbs_value_destroy(&map);
                        return throw_error(env, node, "failed to store map value");
                    }
                    tbs_value_destroy(&val);
                }
            }
            return map;
        }
        case TBS_NODE_MEMBER_ACCESS: {
            tbs_value_t obj = tbs_eval(node->data.member_access.object, env);
            const char *member = node->data.member_access.member;

            // OOP: Instance field/method access
            if (obj.type == TBS_VAL_INSTANCE) {
                tbs_instance_t *instance = obj.data.instance_val.instance;

                // Try field first
                tbs_value_t field_val;
                if (tbs_instance_get_field(instance, member, &field_val)) {
                    tbs_class_t *field_owner = NULL;
                    int access = tbs_class_get_instance_field_access(instance->klass, member,
                                                                        &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_access.object, member)) {
                        return throw_field_access_error(env, node, member, access);
                    }
                    return field_val;
                }

                // Try method
                tbs_func_t *method = tbs_instance_get_method(instance, member);
                if (method) {
                    if (!can_access_method(env, method, node->data.member_access.object)) {
                        return throw_method_access_error(env, node, member, method);
                    }
                    return tbs_val_bound_method(instance, method);
                }

                return throw_error(env, node, "Instance has no field or method '%s'", member);
            }

            // OOP: Class static field/method access
            if (obj.type == TBS_VAL_CLASS) {
                tbs_class_t *klass = obj.data.class_val.klass;

                tbs_value_t static_field;
                if (tbs_class_get_static_field(klass, member, &static_field)) {
                    tbs_class_t *field_owner = NULL;
                    int access = tbs_class_get_static_field_access(klass, member,
                                                                      &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_access.object, member)) {
                        return throw_field_access_error(env, node, member, access);
                    }
                    return static_field;
                }

                tbs_func_t *static_method = tbs_class_lookup_method(klass, member, 1);
                if (static_method) {
                    if (!can_access_method(env, static_method, node->data.member_access.object)) {
                        return throw_method_access_error(env, node, member, static_method);
                    }
                    // For static methods, we don't bind 'this'
                    tbs_value_t func_val = {0};
                    func_val.type = TBS_VAL_FUNCTION;
                    func_val.data.function.arg_params = static_method->data.script.arg_params;
                    func_val.data.function.arg_count = static_method->data.script.arg_count;
                    func_val.data.function.body = static_method->data.script.body;
                    func_val.data.function.closure_env = static_method->closure_env;
                    tbs_env_retain(static_method->closure_env);
                    func_val.data.function.owner_class = static_method->owner_class;
                    func_val.data.function.is_static_method = static_method->is_static_method;
                    func_val.data.function.access_level = static_method->access_level;
                    return func_val;
                }
                return throw_error(env, node, "Class has no static field or method '%s'", member);
            }

            if (tbs_value_is_object_like(&obj)) {
                return tbs_map_get(&obj, member);
            }
            /* String property access */
            if (obj.type == TBS_VAL_STRING) {
                if (strcmp(member, "length") == 0) return tbs_val_num((double)obj.data.string.len);
            }
            if (obj.type == TBS_VAL_BYTES) {
                if (strcmp(member, "length") == 0) return tbs_val_num((double)obj.data.bytes.len);
            }
            if (obj.type == TBS_VAL_DATETIME) {
                tbs_value_t datetime_member;
                if (tbs_datetime_member_get(obj, member, &datetime_member)) return datetime_member;
            }
            if (obj.type == TBS_VAL_OFFSET_DATETIME) {
                tbs_value_t offset_datetime_member;
                if (tbs_offset_datetime_member_get(obj, member, &offset_datetime_member))
                    return offset_datetime_member;
            }
            if (obj.type == TBS_VAL_DATE) {
                tbs_value_t date_member;
                if (tbs_date_member_get(obj, member, &date_member)) return date_member;
            }
            if (obj.type == TBS_VAL_TIME) {
                tbs_value_t time_member;
                if (tbs_time_member_get(obj, member, &time_member)) return time_member;
            }
            if (obj.type == TBS_VAL_DURATION) {
                tbs_value_t duration_member;
                if (tbs_duration_member_get(obj, member, &duration_member)) return duration_member;
            }
            if (obj.type == TBS_VAL_DECIMAL) {
                tbs_value_t decimal_member;
                if (tbs_decimal_member_get(obj, member, &decimal_member)) return decimal_member;
            }
            if (obj.type == TBS_VAL_MONEY) {
                tbs_value_t money_member;
                if (tbs_money_member_get(obj, member, &money_member)) return money_member;
            }
            if (obj.type == TBS_VAL_ENUM || obj.type == TBS_VAL_FLAGS) {
                tbs_value_t enum_member;
                if (tbs_enum_member_get(obj, member, &enum_member)) return enum_member;
            }
            if (obj.type == TBS_VAL_TYPED_ARRAY) {
                tbs_value_t typed_array_member;
                if (tbs_typed_array_member_get(obj, member, &typed_array_member))
                    return typed_array_member;
            }
            /* Vector property access */
            if (obj.type == TBS_VAL_VECTOR) {
                if (strcmp(member, "length") == 0) return tbs_val_num((double)obj.data.vector.size);
            }
            return throw_error(env, node, "Member access '%s' is invalid for %s",
                               member ? member : "<null>", type_name(obj.type));
        }
        case TBS_NODE_MEMBER_SET: {
            tbs_node_t *object_node = node->data.member_set.object;
            if (!object_node ||
                (object_node->type != TBS_NODE_VARIABLE &&
                 object_node->type != TBS_NODE_THIS &&
                 object_node->type != TBS_NODE_SUPER)) {
                return throw_error(env, node,
                                   "Member assignment '%s' requires a variable receiver, this, or static super",
                                   node->data.member_set.member ? node->data.member_set.member : "<null>");
            }

            if (object_node->type == TBS_NODE_SUPER) {
                if (!object_node->data.super_expr.member ||
                    object_node->data.super_expr.is_call ||
                    !env) {
                    return throw_error(env, node,
                                       "super field assignment is only valid inside class methods");
                }

                tbs_class_t *owner_class = eval_current_class(env);
                if (!owner_class) {
                    return throw_error(env, node, "super is only valid inside class methods");
                }

                tbs_class_t *parent = owner_class->prototype;
                if (!parent) {
                    return throw_error(env, node, "Class '%s' has no parent class",
                                       owner_class->name ? owner_class->name : "<unknown>");
                }

                if (env->current_method_is_static) {
                    tbs_class_t *field_owner = NULL;
                    int access = tbs_class_get_static_field_access(
                        parent, node->data.member_set.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access, object_node,
                                                   node->data.member_set.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.member_set.member,
                                                        access);
                    }
                    tbs_value_t val = tbs_eval(node->data.member_set.value, env);
                    if (env && env->flow != tbs_FLOW_NORMAL) return zero;
                    {
                        char field_error[256];
                        if (!tbs_class_set_static_field_checked(
                                parent, node->data.member_set.member, val,
                                field_error, sizeof(field_error)))
                            return throw_error(env, node, "%s", field_error);
                    }
                    return val;
                }

                tbs_value_t this_val = tbs_env_get(env, "this");
                if (this_val.type != TBS_VAL_INSTANCE) {
                    return throw_error(env, node, "super is only valid inside instance methods or constructors");
                }

                tbs_value_t val = tbs_eval(node->data.member_set.value, env);
                if (env && env->flow != tbs_FLOW_NORMAL) return zero;
                tbs_class_t *field_owner = NULL;
                int access = tbs_class_get_instance_field_access(
                    parent, node->data.member_set.member, &field_owner);
                if (!can_access_declared_field(env, field_owner, access, object_node,
                                               node->data.member_set.member)) {
                    return throw_field_access_error(env, node,
                                                    node->data.member_set.member,
                                                    access);
                }
                {
                    char field_error[256];
                    if (!tbs_instance_set_field_checked(
                            this_val.data.instance_val.instance,
                            node->data.member_set.member, val,
                            field_error, sizeof(field_error)))
                        return throw_error(env, node, "%s", field_error);
                }
                return val;
            }

            /* Evaluate the new value only after the receiver shape is accepted. */
            tbs_value_t val = tbs_eval(node->data.member_set.value, env);
            if (env && env->flow != tbs_FLOW_NORMAL) return zero;
            if (object_node->type == TBS_NODE_VARIABLE) {
                const char *var_name = object_node->data.variable.name;
                /* Get the variable from hash table */
                tbs_value_t var_val = tbs_env_get(env, var_name);
                if (tbs_value_is_object_like(&var_val)) {
                    tbs_map_set(&var_val, node->data.member_set.member, val);
                    /* Update the variable in hash table */
                    tbs_env_set(env, var_name, var_val);
                    return val;
                }
                // OOP: Instance field assignment
                if (var_val.type == TBS_VAL_INSTANCE) {
                    tbs_instance_t *instance = var_val.data.instance_val.instance;
                    tbs_class_t *field_owner = NULL;
                    int access = tbs_class_get_instance_field_access(
                        instance->klass, node->data.member_set.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_set.object,
                                                   node->data.member_set.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.member_set.member,
                                                        access);
                    }

                    {
                        char field_error[256];
                        if (!tbs_instance_set_field_checked(
                                instance, node->data.member_set.member, val,
                                field_error, sizeof(field_error)))
                            return throw_error(env, node, "%s", field_error);
                    }
                    return val;
                }
                if (var_val.type == TBS_VAL_CLASS) {
                    tbs_class_t *klass = var_val.data.class_val.klass;
                    tbs_class_t *field_owner = NULL;
                    int access = tbs_class_get_static_field_access(
                        klass, node->data.member_set.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access,
                                                   node->data.member_set.object,
                                                   node->data.member_set.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.member_set.member,
                                                        access);
                    }

                    {
                        char field_error[256];
                        if (!tbs_class_set_static_field_checked(
                                klass, node->data.member_set.member, val,
                                field_error, sizeof(field_error)))
                            return throw_error(env, node, "%s", field_error);
                    }
                    return val;
                }
            }

            if (object_node->type == TBS_NODE_THIS) {
                tbs_value_t this_val = tbs_eval(object_node, env);
                if (env && env->flow != tbs_FLOW_NORMAL) return zero;
                if (this_val.type != TBS_VAL_INSTANCE) {
                    return throw_error(env, node, "this is only valid inside instance methods or constructors");
                }

                tbs_instance_t *instance = this_val.data.instance_val.instance;
                tbs_class_t *field_owner = NULL;
                int access = tbs_class_get_instance_field_access(
                    instance->klass, node->data.member_set.member, &field_owner);
                if (!can_access_declared_field(env, field_owner, access, object_node,
                                               node->data.member_set.member)) {
                    return throw_field_access_error(env, node,
                                                    node->data.member_set.member,
                                                    access);
                }

                {
                    char field_error[256];
                    if (!tbs_instance_set_field_checked(
                            instance, node->data.member_set.member, val,
                            field_error, sizeof(field_error)))
                        return throw_error(env, node, "%s", field_error);
                }
                return val;
            }

            return throw_error(env, node, "Member assignment '%s' requires a map, instance, or class",
                               node->data.member_set.member ? node->data.member_set.member : "<null>");
        }
        case TBS_NODE_NULL: {
            tbs_value_t null_val;
            memset(&null_val, 0, sizeof(null_val));
            null_val.type = TBS_VAL_NULL;
            return null_val;
        }
        case TBS_NODE_FOR_IN: {
            tbs_value_t collection = tbs_eval(node->data.for_in.collection, env);
            if (env && (env->flow != tbs_FLOW_NORMAL || env->aborted)) return zero;
            tbs_value_t last_val = zero;

            if (collection.type == TBS_VAL_VECTOR) {
                for (size_t i = 0; i < collection.data.vector.size; ++i) {
                    if (env && !eval_loop_tick(env)) break;
                    tbs_env_set(env, node->data.for_in.var_name, tbs_val_num(collection.data.vector.data[i]));
                    last_val = tbs_eval(node->data.for_in.body, env);
                    if (env && env->flow == tbs_FLOW_BREAK) { env->flow = tbs_FLOW_NORMAL; break; }
                    if (env && env->flow == tbs_FLOW_CONTINUE) env->flow = tbs_FLOW_NORMAL;
                    if (env && env->flow == tbs_FLOW_RETURN) break;
                }
            } else if (tbs_value_is_object_like(&collection)) {
                tbs_map_iter_t it = tbs_map_iter_begin(&collection);
                const char *key;
                while (tbs_map_iter_next(&it, &key, NULL)) {
                    if (env && !eval_loop_tick(env)) break;
                    vstr sv;
                    sv.data = (char*)key;
                    sv.len = strlen(key);
                    tbs_env_set(env, node->data.for_in.var_name, tbs_val_str(sv));
                    last_val = tbs_eval(node->data.for_in.body, env);
                    if (env && env->flow == tbs_FLOW_BREAK) { env->flow = tbs_FLOW_NORMAL; break; }
                    if (env && env->flow == tbs_FLOW_CONTINUE) env->flow = tbs_FLOW_NORMAL;
                    if (env && env->flow == tbs_FLOW_RETURN) break;
                }
            } else if (collection.type == TBS_VAL_LIST || collection.type == TBS_VAL_SET) {
                for (size_t i = 0; i < collection.data.list.count; ++i) {
                    if (env && !eval_loop_tick(env)) break;
                    tbs_env_set(env, node->data.for_in.var_name, collection.data.list.items[i]);
                    last_val = tbs_eval(node->data.for_in.body, env);
                    if (env && env->flow == tbs_FLOW_BREAK) { env->flow = tbs_FLOW_NORMAL; break; }
                    if (env && env->flow == tbs_FLOW_CONTINUE) env->flow = tbs_FLOW_NORMAL;
                    if (env && env->flow == tbs_FLOW_RETURN) break;
                }
            } else if (collection.type == TBS_VAL_TYPED_ARRAY) {
                for (size_t i = 0; i < collection.data.typed_array.count; ++i) {
                    if (env && !eval_loop_tick(env)) break;
                    tbs_env_set(env, node->data.for_in.var_name,
                                   tbs_typed_array_get_value(collection, i));
                    last_val = tbs_eval(node->data.for_in.body, env);
                    if (env && env->flow == tbs_FLOW_BREAK) { env->flow = tbs_FLOW_NORMAL; break; }
                    if (env && env->flow == tbs_FLOW_CONTINUE) env->flow = tbs_FLOW_NORMAL;
                    if (env && env->flow == tbs_FLOW_RETURN) break;
                }
            }
            return last_val;
        }

        case TBS_NODE_THROW: {
            tbs_value_t thrown = zero;
            if (node->data.throw_stmt.value) {
                thrown = tbs_eval(node->data.throw_stmt.value, env);
            }
            if (env) {
                env->flow = tbs_FLOW_THROW;
                env->error_value = thrown;
            }
            return thrown;
        }

        case TBS_NODE_TRY_CATCH: {
            /* Evaluate try body */
            tbs_value_t result = tbs_eval(node->data.try_catch.try_body, env);

            /* If a throw occurred, handle catch */
            if (env && env->flow == tbs_FLOW_THROW) {
                env->flow = tbs_FLOW_NORMAL;

                tbs_env_t catch_env;
                tbs_env_init_child(&catch_env, env);
                catch_env.eval_node = env->eval_node;
                catch_env.exec_script_body = env->exec_script_body;
                catch_env.max_recursion = env->max_recursion;
                catch_env.curr_recursion = env->curr_recursion;
                catch_env.max_loop_iterations = env->max_loop_iterations;
                catch_env.curr_loop_iterations = env->curr_loop_iterations;
                catch_env.max_nodes = env->max_nodes;
                catch_env.curr_nodes = env->curr_nodes;

                /* Bind error value to catch variable if one was declared */
                if (node->data.try_catch.catch_var) {
                    tbs_env_set_local(&catch_env, node->data.try_catch.catch_var, env->error_value);
                }

                /* Evaluate catch body */
                result = tbs_eval(node->data.try_catch.catch_body, &catch_env);
                result = tbs_value_clone_to_env(result, env);

                env->curr_nodes = catch_env.curr_nodes;
                env->curr_loop_iterations = catch_env.curr_loop_iterations;
                env->aborted = catch_env.aborted;
                if (catch_env.flow != tbs_FLOW_NORMAL) {
                    env->flow = catch_env.flow;
                    env->return_value = tbs_value_clone_to_env(catch_env.return_value, env);
                    env->error_value = tbs_value_clone_to_env(catch_env.error_value, env);
                }

                tbs_env_free(&catch_env);
            }

            return result;
        }

        case TBS_NODE_FUNCTION_EXPRESSION: {
            /* Create a function value that captures the current environment */
            tbs_value_t val = {0};
            val.type = TBS_VAL_FUNCTION;
            val.data.function.arg_params = node->data.func_def.arg_params;
            val.data.function.arg_count  = node->data.func_def.arg_count;
            val.data.function.body       = node->data.func_def.body;
            {
                /* Capture only the free variables of the closure body so
                 * repeated closure assignment does not chain-capture the
                 * previous closure value (which would pin it forever). */
                char **free_vars = tbs_collect_closure_free_vars(
                    node->data.func_def.body, node->data.func_def.arg_params,
                    node->data.func_def.arg_count, &env->arena);
                size_t fv_count = 0;
                while (free_vars && free_vars[fv_count]) fv_count++;
                val.data.function.closure_env = free_vars
                    ? tbs_env_snapshot_names(env, (const char *const *)free_vars, fv_count)
                    : tbs_env_snapshot(env);
            }
            val.data.function.owner_class = NULL;
            val.data.function.is_static_method = 0;
            val.data.function.access_level = TBS_ACCESS_PUBLIC;
            return val;
        }

        case TBS_NODE_SWITCH: {
            tbs_value_t switch_val = tbs_eval(node->data.switch_stmt.value, env);
            if (env && env->flow != tbs_FLOW_NORMAL) return zero;

            for (size_t i = 0; i < node->data.switch_stmt.case_count; ++i) {
                tbs_value_t case_val = tbs_eval(node->data.switch_stmt.cases[i * 2], env);
                if (env && env->flow != tbs_FLOW_NORMAL) return zero;

                int match = 0;
                if ((switch_val.type == TBS_VAL_NUMBER || switch_val.type == TBS_VAL_INTEGER ||
                     switch_val.type == TBS_VAL_BOOL || switch_val.type == TBS_VAL_STRING ||
                     switch_val.type == TBS_VAL_BYTES || switch_val.type == TBS_VAL_UUID ||
                     switch_val.type == TBS_VAL_NULL) &&
                    (case_val.type == TBS_VAL_NUMBER || case_val.type == TBS_VAL_INTEGER ||
                     case_val.type == TBS_VAL_BOOL || case_val.type == TBS_VAL_STRING ||
                     case_val.type == TBS_VAL_BYTES || case_val.type == TBS_VAL_UUID ||
                     case_val.type == TBS_VAL_NULL))
                    match = values_match(switch_val, case_val);

                if (match)
                    return tbs_eval(node->data.switch_stmt.cases[i * 2 + 1], env);
            }

            if (node->data.switch_stmt.default_case)
                return tbs_eval(node->data.switch_stmt.default_case, env);

            return zero;
        }

        // ====================================================================
        // OOP (Object-Oriented Programming) Node Evaluation
        // ====================================================================

        case TBS_NODE_CLASS_DEF: {
            return eval_class_def_node(node, env);
        }

        case TBS_NODE_THIS: {
            // 'this' keyword - lookup in environment
            if (env && env->current_method_is_static) {
                return throw_error(env, node, "this is only valid inside instance methods or constructors");
            }

            tbs_value_t this_val = tbs_env_get(env, "this");
            if (this_val.type != TBS_VAL_INSTANCE) {
                return throw_error(env, node, "this is only valid inside instance methods or constructors");
            }
            return this_val;
        }

        case TBS_NODE_SUPER: {
            // super() or super.method
            tbs_class_t *owner_class = eval_current_class(env);
            if (env && env->current_method_is_static) {
                if (!owner_class) {
                    return throw_error(env, node, "super is only valid inside class methods");
                }

                tbs_class_t *parent = owner_class->prototype;
                if (!parent) {
                    return throw_error(env, node, "Class '%s' has no parent class",
                                       owner_class->name ? owner_class->name : "<unknown>");
                }

                if (node->data.super_expr.member == NULL) {
                    return throw_error(env, node, "super() is only valid inside instance constructors");
                }

                if (node->data.super_expr.is_call) {
                    size_t actual_count = 0;
                    tbs_value_t *args = eval_expand_args(node->data.super_expr.args,
                                                            node->data.super_expr.arg_count,
                                                            env, &actual_count);
                    if (env->flow != tbs_FLOW_NORMAL || env->aborted) {
                        tbs_values_destroy(args, actual_count);
                        free(args);
                        return zero;
                    }

                    tbs_func_t *method = tbs_class_lookup_method_typed(
                        parent, node->data.super_expr.member, 1, actual_count, args);
                    if (!method) {
                        tbs_values_destroy(args, actual_count);
                        free(args);
                        return throw_error(env, node, "Parent class has no static method '%s'",
                                           node->data.super_expr.member);
                    }
                    if (!can_access_method(env, method, node)) {
                        tbs_values_destroy(args, actual_count);
                        free(args);
                        return throw_method_access_error(env, node,
                                                         node->data.super_expr.member,
                                                         method);
                    }

                    tbs_value_t result = eval_script_function(method, actual_count, args, env, env);
                    tbs_values_destroy(args, actual_count);
                    free(args);
                    return result;
                }

                tbs_value_t static_field;
                if (tbs_class_get_static_field(parent, node->data.super_expr.member,
                                                  &static_field)) {
                    tbs_class_t *field_owner = NULL;
                    int access = tbs_class_get_static_field_access(
                        parent, node->data.super_expr.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access, node,
                                                   node->data.super_expr.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.super_expr.member,
                                                        access);
                    }
                    return static_field;
                }

                tbs_func_t *method =
                    tbs_class_lookup_method(parent, node->data.super_expr.member, 1);
                if (!method) {
                    return throw_error(env, node, "Parent class has no static field or method '%s'",
                                       node->data.super_expr.member);
                }
                if (!can_access_method(env, method, node)) {
                    return throw_method_access_error(env, node, node->data.super_expr.member,
                                                     method);
                }

                tbs_value_t func_val = {0};
                func_val.type = TBS_VAL_FUNCTION;
                func_val.data.function.arg_params = method->data.script.arg_params;
                func_val.data.function.arg_count = method->data.script.arg_count;
                func_val.data.function.body = method->data.script.body;
                func_val.data.function.closure_env = method->closure_env;
                tbs_env_retain(method->closure_env);
                func_val.data.function.owner_class = method->owner_class;
                func_val.data.function.is_static_method = method->is_static_method;
                func_val.data.function.access_level = method->access_level;
                return func_val;
            }

            // Get 'this' from environment
            tbs_value_t this_val = tbs_env_get(env, "this");
            if (this_val.type != TBS_VAL_INSTANCE) {
                return throw_error(env, node, "super is only valid inside instance methods or constructors");
            }

            tbs_instance_t *instance = this_val.data.instance_val.instance;
            tbs_class_t *parent = owner_class ? owner_class->prototype : instance->klass->prototype;
            if (!parent) {
                const char *owner_name = owner_class ? owner_class->name : instance->klass->name;
                return throw_error(env, node, "Class '%s' has no parent class",
                                   owner_name ? owner_name : "<unknown>");
            }

            if (node->data.super_expr.member == NULL) {
                if (!env || !env->current_method_is_constructor) {
                    return throw_error(env, node, "super() is only valid inside instance constructors");
                }

                size_t actual_count = 0;
                tbs_value_t *args = eval_expand_args(node->data.super_expr.args,
                                                        node->data.super_expr.arg_count,
                                                        env, &actual_count);
                if (env->flow != tbs_FLOW_NORMAL || env->aborted) {
                    tbs_values_destroy(args, actual_count);
                    free(args);
                    return zero;
                }

                tbs_func_t *constructor =
                    eval_find_constructor_typed(parent, actual_count, args);
                if (!constructor) {
                    tbs_values_destroy(args, actual_count);
                    free(args);
                    return zero;
                }

                tbs_value_t result =
                    eval_script_function(constructor, actual_count, args, env, env);
                tbs_values_destroy(args, actual_count);
                free(args);
                return result;
            } else {
                if (node->data.super_expr.is_call) {
                    size_t actual_count = 0;
                    tbs_value_t *args = eval_expand_args(node->data.super_expr.args,
                                                            node->data.super_expr.arg_count,
                                                            env, &actual_count);
                    if (env->flow != tbs_FLOW_NORMAL || env->aborted) {
                        tbs_values_destroy(args, actual_count);
                        free(args);
                        return zero;
                    }

                    // super.method(...) - lookup method in parent class
                    tbs_func_t *method = tbs_class_lookup_method_typed(
                        parent, node->data.super_expr.member, 0, actual_count, args);
                    if (!method) {
                        tbs_values_destroy(args, actual_count);
                        free(args);
                        return throw_error(env, node, "Parent class has no method '%s'",
                                           node->data.super_expr.member);
                    }
                    if (!can_access_method(env, method, node)) {
                        tbs_values_destroy(args, actual_count);
                        free(args);
                        return throw_method_access_error(env, node,
                                                         node->data.super_expr.member,
                                                         method);
                    }

                    tbs_value_t result = eval_script_function(method, actual_count, args, env, env);
                    tbs_values_destroy(args, actual_count);
                    free(args);
                    return result;
                }

                // super.method - lookup method in parent class
                tbs_func_t *method = tbs_class_lookup_method(parent, node->data.super_expr.member, 0);
                if (method) {
                    if (!can_access_method(env, method, node)) {
                        return throw_method_access_error(env, node,
                                                         node->data.super_expr.member,
                                                         method);
                    }
                    // Return bound method
                    return tbs_val_bound_method(instance, method);
                }

                tbs_value_t field_value;
                if (tbs_instance_get_field(instance, node->data.super_expr.member, &field_value)) {
                    tbs_class_t *field_owner = NULL;
                    int access = tbs_class_get_instance_field_access(
                        parent, node->data.super_expr.member, &field_owner);
                    if (!can_access_declared_field(env, field_owner, access, node,
                                                   node->data.super_expr.member)) {
                        return throw_field_access_error(env, node,
                                                        node->data.super_expr.member,
                                                        access);
                    }
                    return field_value;
                }

                return throw_error(env, node, "Parent class has no method or instance field '%s'",
                                   node->data.super_expr.member);
            }
        }

        case TBS_NODE_INSTANCEOF: {
            // obj instanceof ClassName
            tbs_value_t obj = tbs_eval(node->data.instanceof_expr.object, env);
            if (env->flow != tbs_FLOW_NORMAL) return zero;

            if (obj.type != TBS_VAL_INSTANCE) {
                return tbs_val_num(0.0); // Not an instance
            }

            tbs_class_t *klass = NULL;
            tbs_value_t class_value;
            if (node->data.instanceof_expr.class_expr) {
                class_value = tbs_eval(node->data.instanceof_expr.class_expr, env);
                if (env->flow != tbs_FLOW_NORMAL) return zero;
            } else {
                class_value = tbs_env_get(env, node->data.instanceof_expr.class_name);
            }
            if (class_value.type == TBS_VAL_CLASS)
                klass = class_value.data.class_val.klass;
            if (!klass) return tbs_val_num(0.0);

            int is_instance = tbs_instance_of(obj.data.instance_val.instance, klass);
            return tbs_val_num(is_instance ? 1.0 : 0.0);
        }

        default:
            return zero;
    }
}
