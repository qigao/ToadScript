#include "../src/internal.h"
#include "../src/graph/cflow_runtime.h"
#include <toad/engine.h>
#include "tinytest.h"

#include <math.h>
#include <string.h>

static toad_node_t *ts_stream_test_single_expr(toad_script_ctx_t *ctx,
                                                 const char *source,
                                                 toad_node_t **root_out) {
  toad_node_t *root = toad_script_parse_with_error(ctx, source);
  if (root_out) *root_out = root;
  if (!root) return NULL;
  if (root->type == TOAD_NODE_BLOCK && root->data.block.count == 1u)
    return root->data.block.statements[0];
  return root;
}

typedef struct ts_stream_test_string_source_s {
  size_t calls;
  const char *value;
} ts_stream_test_string_source_t;

static toad_value_t ts_stream_test_counted_string(
    size_t argc, toad_value_t *args, toad_env_t *env, void *user_data) {
  ts_stream_test_string_source_t *state =
      (ts_stream_test_string_source_t *)user_data;
  (void)args;
  (void)env;
  if (!state || argc != 0u) return toad_val_num(0.0);
  ++state->calls;
  return toad_val_str(vstr_from_cstr(state->value ? state->value : ""));
}

spec("Toad CFlow stream runtime") {
  describe("terminal admission") {
    it("executes a numeric stream reduce through CFlow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr = ts_stream_test_single_expr(
          ctx,
          "[1, 2, 3].stream().filter(x => x > 1).map(x => x * 2)"
          ".reduce(10, (acc, x) => acc + x);",
          &root);
      toad_value_t result = toad_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, TOAD_VAL_NUMBER);
      check(fabs(result.data.number - 20.0) <= 1e-9);
      check_equal(error[0], '\0');

      toad_value_destroy(&result);
      toad_free(root);
      toad_script_free(ctx);
    }

    it("executes stream count through the same CFlow plan path") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([-2, 0, 3]).filter(x => x > 0).map(x => x * 2).count();",
          &root);
      toad_value_t result = toad_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, TOAD_VAL_NUMBER);
      check(fabs(result.data.number - 1.0) <= 1e-9);
      check_equal(error[0], '\0');

      toad_value_destroy(&result);
      toad_free(root);
      toad_script_free(ctx);
    }


    it("projects a pure canonical Plugin Function directly into CFlow MAP") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr;
      toad_value_t result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_equal(toad_script_load_plugin(ctx, "loader_function"), 0);

      expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([1,2,3])"
          ".map(x => loader_function.double(x))"
          ".toVector();",
          &root);
      check_not_null(expr);
      check_equal(
          ts_cflow_runtime_try_scalar_terminal(
              ctx, expr, &result, error, sizeof(error)),
          TS_CFLOW_RUNTIME_HANDLED);
      check_equal(error[0], '\0');
      check_equal(result.type, TOAD_VAL_VECTOR);
      check_equal(result.data.vector.size, (size_t)3u);
      check(fabs(result.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(result.data.vector.data[1] - 4.0) <= 1e-9);
      check(fabs(result.data.vector.data[2] - 6.0) <= 1e-9);

      toad_value_destroy(&result);
      toad_free(root);
      toad_script_free(ctx);
    }

    it("keeps stateful canonical Plugin Functions outside CFlow admission") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr;
      toad_value_t result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_equal(toad_script_load_plugin(ctx, "loader_function"), 0);

      expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([1,2,3])"
          ".map(x => loader_function.stateful(x))"
          ".toVector();",
          &root);
      check_not_null(expr);
      check_equal(
          ts_cflow_runtime_try_scalar_terminal(
              ctx, expr, &result, error, sizeof(error)),
          TS_CFLOW_RUNTIME_NOT_APPLICABLE);

      toad_value_destroy(&result);
      toad_free(root);
      toad_script_free(ctx);
    }

    it("keeps canonical Plugin Function CFlow MAP aligned in interpreter and JIT") {
      const char *source =
          "mapped=stream.of([1,2,3])"
          ".map(x => loader_function.double(x))"
          ".toVector();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_result;
      toad_value_t jit_result;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_load_plugin(interp, "loader_function"), 0);
      check_equal(toad_script_load_plugin(jit, "loader_function"), 0);
      check_equal(toad_script_run_mir_interp(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);

      interp_result = toad_env_get(&interp->env, "mapped");
      jit_result = toad_env_get(&jit->env, "mapped");
      check_equal(interp_result.type, TOAD_VAL_VECTOR);
      check_equal(jit_result.type, TOAD_VAL_VECTOR);
      check_equal(interp_result.data.vector.size, (size_t)3u);
      check_equal(jit_result.data.vector.size, (size_t)3u);
      check(fabs(interp_result.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(interp_result.data.vector.data[2] - 6.0) <= 1e-9);
      check(fabs(jit_result.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(jit_result.data.vector.data[2] - 6.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }



    it("materializes an admitted intermediate numeric stream through CFlow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([1, 2, 3]).map(x => x * 2);",
          &root);
      toad_value_t stream = toad_val_num(0.0);
      toad_value_t marker;
      toad_value_t source;
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      ctx->env.max_nodes = 1u;

      check_equal(
          ts_cflow_runtime_try_materialized_stream(
              ctx, expr, &stream, error, sizeof(error)),
          TS_CFLOW_RUNTIME_HANDLED);
      check_equal(error[0], '\0');
      check_true(toad_value_is_object_like(&stream));
      check_true(toad_map_has(&stream, "__ts_cflow_materialized"));
      marker = toad_map_get(&stream, "__ts_cflow_materialized");
      check_equal(marker.type, TOAD_VAL_NUMBER);
      check(fabs(marker.data.number - 1.0) <= 1e-9);
      source = toad_map_get(&stream, "source");
      check_equal(source.type, TOAD_VAL_VECTOR);
      check_equal(source.data.vector.size, (size_t)3u);
      check(fabs(source.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(source.data.vector.data[1] - 4.0) <= 1e-9);
      check(fabs(source.data.vector.data[2] - 6.0) <= 1e-9);

      toad_value_destroy(&stream);
      toad_free(root);
      toad_script_free(ctx);
    }

    it("carries a CFlow materialized stream across statements") {
      const char *source =
          "s=stream.of([1,2,3]).map(x => x * 2);"
          "n=s.count();"
          "v=s.toVector();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_stream;
      toad_value_t jit_stream;
      toad_value_t interp_vector;
      toad_value_t jit_vector;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run_mir_interp(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);

      interp_stream = toad_env_get(&interp->env, "s");
      jit_stream = toad_env_get(&jit->env, "s");
      check_true(toad_value_is_object_like(&interp_stream));
      check_true(toad_value_is_object_like(&jit_stream));
      check_true(toad_map_has(&interp_stream, "__ts_cflow_materialized"));
      check_true(toad_map_has(&jit_stream, "__ts_cflow_materialized"));
      check(fabs(toad_script_get_num(interp, "n") - 3.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "n") - 3.0) <= 1e-9);

      interp_vector = toad_env_get(&interp->env, "v");
      jit_vector = toad_env_get(&jit->env, "v");
      check_equal(interp_vector.type, TOAD_VAL_VECTOR);
      check_equal(jit_vector.type, TOAD_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)3u);
      check_equal(jit_vector.data.vector.size, (size_t)3u);
      check(fabs(interp_vector.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(interp_vector.data.vector.data[2] - 6.0) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[2] - 6.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }


    it("re-admits pure map and filter from a materialized stream") {
      const char *source =
          "s=stream.of([1,2,3]).map(x => x * 2);"
          "t=s.filter(x => x > 2);"
          "u=t.map(x => x + 1);"
          "n=u.count();"
          "v=u.toVector();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_t;
      toad_value_t jit_t;
      toad_value_t interp_u;
      toad_value_t jit_u;
      toad_value_t interp_vector;
      toad_value_t jit_vector;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run_mir_interp(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);

      interp_t = toad_env_get(&interp->env, "t");
      jit_t = toad_env_get(&jit->env, "t");
      interp_u = toad_env_get(&interp->env, "u");
      jit_u = toad_env_get(&jit->env, "u");
      check_true(toad_value_is_object_like(&interp_t));
      check_true(toad_value_is_object_like(&jit_t));
      check_true(toad_value_is_object_like(&interp_u));
      check_true(toad_value_is_object_like(&jit_u));
      check_true(toad_map_has(&interp_t, "__ts_cflow_materialized"));
      check_true(toad_map_has(&jit_t, "__ts_cflow_materialized"));
      check_true(toad_map_has(&interp_u, "__ts_cflow_materialized"));
      check_true(toad_map_has(&jit_u, "__ts_cflow_materialized"));

      check(fabs(toad_script_get_num(interp, "n") - 2.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "n") - 2.0) <= 1e-9);

      interp_vector = toad_env_get(&interp->env, "v");
      jit_vector = toad_env_get(&jit->env, "v");
      check_equal(interp_vector.type, TOAD_VAL_VECTOR);
      check_equal(jit_vector.type, TOAD_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)2u);
      check_equal(jit_vector.data.vector.size, (size_t)2u);
      check(fabs(interp_vector.data.vector.data[0] - 5.0) <= 1e-9);
      check(fabs(interp_vector.data.vector.data[1] - 7.0) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[0] - 5.0) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[1] - 7.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps admitted numeric terminals out of the legacy stream evaluator") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t result;

      check_not_null(ctx);
      ctx->env.max_nodes = 1u;

      check_equal(
          toad_script_run_mir_interp(
              ctx,
              "result=stream.of([-2,0,3])"
              ".filter(x => x > 0)"
              ".map(x => x * 2)"
              ".count();"),
          0);
      check_false(ctx->env.aborted);
      result = toad_env_get(&ctx->env, "result");
      check_equal(result.type, TOAD_VAL_NUMBER);
      check(fabs(result.data.number - 1.0) <= 1e-9);

      toad_script_free(ctx);
    }

    it("keeps captured numeric callables on the explicit legacy barrier") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      toad_env_set(&ctx->env, "threshold", toad_val_num(0.0));
      ctx->env.max_nodes = 1u;

      check(
          toad_script_run_mir_interp(
              ctx,
              "result=stream.of([-2,0,3])"
              ".filter(x => x > threshold)"
              ".count();") != 0);
      check_true(ctx->env.aborted);

      toad_script_free(ctx);
    }

    it("executes text lines count through a typed CFlow plan") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *lines_root = NULL;
      toad_node_t *empty_root = NULL;
      toad_node_t *number_root = NULL;
      toad_node_t *lines_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"a\\r\\nb\\n\").lines().count();", &lines_root);
      toad_node_t *empty_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"\").lines().count();", &empty_root);
      toad_node_t *number_expr = ts_stream_test_single_expr(
          ctx, "stream.text(42).lines().count();", &number_root);
      toad_value_t lines_result = toad_val_num(-1.0);
      toad_value_t empty_result = toad_val_num(-1.0);
      toad_value_t number_result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(lines_expr);
      check_not_null(empty_expr);
      check_not_null(number_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, lines_expr, &lines_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(lines_result.type, TOAD_VAL_NUMBER);
      check(fabs(lines_result.data.number - 3.0) <= 1e-9);
      check_equal(error[0], '\0');

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, empty_expr, &empty_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check(fabs(empty_result.data.number - 1.0) <= 1e-9);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, number_expr, &number_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check(fabs(number_result.data.number - 1.0) <= 1e-9);

      toad_value_destroy(&number_result);
      toad_value_destroy(&empty_result);
      toad_value_destroy(&lines_result);
      toad_free(number_root);
      toad_free(empty_root);
      toad_free(lines_root);
      toad_script_free(ctx);
    }

    it("materializes text lines collect and toList through CFlow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *collect_root = NULL;
      toad_node_t *list_root = NULL;
      toad_node_t *collect_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"a\\r\\nb\\n\").lines().collect();",
          &collect_root);
      toad_node_t *list_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"x\\ny\").lines().toList();",
          &list_root);
      toad_value_t collect_result = toad_val_num(-1.0);
      toad_value_t list_result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(collect_expr);
      check_not_null(list_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, collect_expr, &collect_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(collect_result.type, TOAD_VAL_LIST);
      check_equal(collect_result.data.list.count, (size_t)3u);
      check_equal(collect_result.data.list.items[0].type, TOAD_VAL_STRING);
      check_equal(collect_result.data.list.items[0].data.string.len, (size_t)1u);
      check(memcmp(collect_result.data.list.items[0].data.string.data, "a", 1u) == 0);
      check_equal(collect_result.data.list.items[1].data.string.len, (size_t)1u);
      check(memcmp(collect_result.data.list.items[1].data.string.data, "b", 1u) == 0);
      check_equal(collect_result.data.list.items[2].data.string.len, (size_t)0u);
      check_equal(error[0], '\0');

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, list_expr, &list_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(list_result.type, TOAD_VAL_LIST);
      check_equal(list_result.data.list.count, (size_t)2u);
      check(memcmp(list_result.data.list.items[0].data.string.data, "x", 1u) == 0);
      check(memcmp(list_result.data.list.items[1].data.string.data, "y", 1u) == 0);

      toad_value_destroy(&list_result);
      toad_value_destroy(&collect_result);
      toad_free(list_root);
      toad_free(collect_root);
      toad_script_free(ctx);
    }

    it("executes text split terminals through CFlow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *count_root = NULL;
      toad_node_t *collect_root = NULL;
      toad_node_t *chars_root = NULL;
      toad_node_t *bad_sep_root = NULL;
      toad_node_t *count_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\",a,,b,\").split(\",\").count();",
          &count_root);
      toad_node_t *collect_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\",a,,b,\").split(\",\").collect();",
          &collect_root);
      toad_node_t *chars_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"ABC\").split(\"\").toList();",
          &chars_root);
      toad_node_t *bad_sep_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"abc\").split(42).count();",
          &bad_sep_root);
      toad_value_t count_result = toad_val_num(-1.0);
      toad_value_t collect_result = toad_val_num(-1.0);
      toad_value_t chars_result = toad_val_num(-1.0);
      toad_value_t bad_sep_result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(count_expr);
      check_not_null(collect_expr);
      check_not_null(chars_expr);
      check_not_null(bad_sep_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, count_expr, &count_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check(fabs(count_result.data.number - 5.0) <= 1e-9);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, collect_expr, &collect_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(collect_result.type, TOAD_VAL_LIST);
      check_equal(collect_result.data.list.count, (size_t)5u);
      check_equal(collect_result.data.list.items[0].data.string.len, (size_t)0u);
      check(memcmp(collect_result.data.list.items[1].data.string.data, "a", 1u) == 0);
      check_equal(collect_result.data.list.items[2].data.string.len, (size_t)0u);
      check(memcmp(collect_result.data.list.items[3].data.string.data, "b", 1u) == 0);
      check_equal(collect_result.data.list.items[4].data.string.len, (size_t)0u);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, chars_expr, &chars_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(chars_result.type, TOAD_VAL_LIST);
      check_equal(chars_result.data.list.count, (size_t)3u);
      check(memcmp(chars_result.data.list.items[0].data.string.data, "A", 1u) == 0);
      check(memcmp(chars_result.data.list.items[2].data.string.data, "C", 1u) == 0);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, bad_sep_expr, &bad_sep_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(bad_sep_result.type, TOAD_VAL_NUMBER);
      check(fabs(bad_sep_result.data.number) <= 1e-9);
      check_equal(error[0], '\0');

      toad_value_destroy(&bad_sep_result);
      toad_value_destroy(&chars_result);
      toad_value_destroy(&collect_result);
      toad_value_destroy(&count_result);
      toad_free(bad_sep_root);
      toad_free(chars_root);
      toad_free(collect_root);
      toad_free(count_root);
      toad_script_free(ctx);
    }

    it("executes typed text line length MAP terminals through CFlow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *vector_root = NULL;
      toad_node_t *list_root = NULL;
      toad_node_t *collect_root = NULL;
      toad_node_t *legacy_root = NULL;
      toad_node_t *vector_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\"a\\nbb\\n\").lines()"
          ".map(line => line.length()).toVector();",
          &vector_root);
      toad_node_t *list_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\"a\\nbb\\n\").lines()"
          ".map(line => line.length()).toList();",
          &list_root);
      toad_node_t *collect_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\"a\\nbb\\n\").lines()"
          ".map(line => line.length()).collect();",
          &collect_root);
      toad_node_t *legacy_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\" a \").lines()"
          ".map(line => line.trim()).toList();",
          &legacy_root);
      toad_value_t vector_result = toad_val_num(-1.0);
      toad_value_t list_result = toad_val_num(-1.0);
      toad_value_t collect_result = toad_val_num(-1.0);
      toad_value_t legacy_result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(vector_expr);
      check_not_null(list_expr);
      check_not_null(collect_expr);
      check_not_null(legacy_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, vector_expr, &vector_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(vector_result.type, TOAD_VAL_VECTOR);
      check_equal(vector_result.data.vector.size, (size_t)3u);
      check(fabs(vector_result.data.vector.data[0] - 1.0) <= 1e-9);
      check(fabs(vector_result.data.vector.data[1] - 2.0) <= 1e-9);
      check(fabs(vector_result.data.vector.data[2] - 0.0) <= 1e-9);
      check_equal(error[0], '\0');

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, list_expr, &list_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(list_result.type, TOAD_VAL_LIST);
      check_equal(list_result.data.list.count, (size_t)3u);
      check(fabs(list_result.data.list.items[0].data.number - 1.0) <= 1e-9);
      check(fabs(list_result.data.list.items[1].data.number - 2.0) <= 1e-9);
      check(fabs(list_result.data.list.items[2].data.number - 0.0) <= 1e-9);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, collect_expr, &collect_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(collect_result.type, TOAD_VAL_LIST);
      check_equal(collect_result.data.list.count, (size_t)3u);
      check(fabs(collect_result.data.list.items[1].data.number - 2.0) <= 1e-9);

      error[0] = '\0';
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, legacy_expr, &legacy_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_NOT_APPLICABLE);
      check_equal(error[0], '\0');

      toad_value_destroy(&legacy_result);
      toad_value_destroy(&collect_result);
      toad_value_destroy(&list_result);
      toad_value_destroy(&vector_result);
      toad_free(legacy_root);
      toad_free(collect_root);
      toad_free(list_root);
      toad_free(vector_root);
      toad_script_free(ctx);
    }

    it("executes typed text nonempty FILTER terminals through CFlow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *count_root = NULL;
      toad_node_t *list_root = NULL;
      toad_node_t *collect_root = NULL;
      toad_node_t *legacy_root = NULL;
      toad_node_t *count_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\"a\\n\\nbb\\n\").lines()"
          ".filter(line => line.length() > 0).count();",
          &count_root);
      toad_node_t *list_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\"a\\n\\nbb\\n\").lines()"
          ".filter(line => line.length() > 0).toList();",
          &list_root);
      toad_node_t *collect_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\"a\\n\\nbb\\n\").lines()"
          ".filter(line => line.length() > 0).collect();",
          &collect_root);
      toad_node_t *legacy_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(\"a\\n\").lines()"
          ".filter(line => line.length() >= 0).count();",
          &legacy_root);
      toad_value_t count_result = toad_val_num(-1.0);
      toad_value_t list_result = toad_val_num(-1.0);
      toad_value_t collect_result = toad_val_num(-1.0);
      toad_value_t legacy_result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(count_expr);
      check_not_null(list_expr);
      check_not_null(collect_expr);
      check_not_null(legacy_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, count_expr, &count_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(count_result.type, TOAD_VAL_NUMBER);
      check(fabs(count_result.data.number - 2.0) <= 1e-9);
      check_equal(error[0], '\0');

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, list_expr, &list_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(list_result.type, TOAD_VAL_LIST);
      check_equal(list_result.data.list.count, (size_t)2u);
      check_equal(list_result.data.list.items[0].type, TOAD_VAL_STRING);
      check_equal(list_result.data.list.items[0].data.string.len, (size_t)1u);
      check_equal(list_result.data.list.items[1].data.string.len, (size_t)2u);
      check(memcmp(
          list_result.data.list.items[0].data.string.data, "a", 1u) == 0);
      check(memcmp(
          list_result.data.list.items[1].data.string.data, "bb", 2u) == 0);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, collect_expr, &collect_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(collect_result.type, TOAD_VAL_LIST);
      check_equal(collect_result.data.list.count, (size_t)2u);
      check(memcmp(
          collect_result.data.list.items[1].data.string.data, "bb", 2u) == 0);

      error[0] = '\0';
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, legacy_expr, &legacy_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_NOT_APPLICABLE);
      check_equal(error[0], '\0');

      toad_value_destroy(&legacy_result);
      toad_value_destroy(&collect_result);
      toad_value_destroy(&list_result);
      toad_value_destroy(&count_result);
      toad_free(legacy_root);
      toad_free(collect_root);
      toad_free(list_root);
      toad_free(count_root);
      toad_script_free(ctx);
    }

    it("admits typed text FILTER before evaluating the source") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      ts_stream_test_string_source_t source_state = {0u, "a\n\n"};
      toad_node_t *supported_root = NULL;
      toad_node_t *legacy_root = NULL;
      toad_node_t *supported_expr;
      toad_node_t *legacy_expr;
      toad_value_t supported_result = toad_val_num(-1.0);
      toad_value_t legacy_result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      toad_env_register_func(
          &ctx->env, "make_text",
          ts_stream_test_counted_string, &source_state);

      supported_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(make_text()).lines()"
          ".filter(line => line.length() > 0).count();",
          &supported_root);
      check_not_null(supported_expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, supported_expr, &supported_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(source_state.calls, (size_t)1u);
      check(fabs(supported_result.data.number - 1.0) <= 1e-9);

      source_state.calls = 0u;
      legacy_expr = ts_stream_test_single_expr(
          ctx,
          "stream.text(make_text()).lines()"
          ".filter(line => line.length() >= 0).count();",
          &legacy_root);
      check_not_null(legacy_expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, legacy_expr, &legacy_result,
                      error, sizeof(error)),
                  TS_CFLOW_RUNTIME_NOT_APPLICABLE);
      check_equal(source_state.calls, (size_t)0u);
      check_equal(error[0], '\0');

      toad_value_destroy(&legacy_result);
      toad_value_destroy(&supported_result);
      toad_free(legacy_root);
      toad_free(supported_root);
      toad_script_free(ctx);
    }

    it("admits a bound vector source through runtime analysis") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      const double values[] = {-2.0, 0.0, 3.0};
      toad_node_t *root = NULL;
      toad_node_t *expr;
      toad_value_t result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      toad_env_set(&ctx->env, "values",
                     toad_val_vec((double *)values,
                                    sizeof(values) / sizeof(values[0])));
      expr = ts_stream_test_single_expr(
          ctx,
          "stream.of(values).filter(x => x > 0).map(x => x * 2).count();",
          &root);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, TOAD_VAL_NUMBER);
      check(fabs(result.data.number - 1.0) <= 1e-9);
      check_equal(error[0], '\0');

      toad_value_destroy(&result);
      toad_free(root);
      toad_script_free(ctx);
    }

    it("admits a numeric list source through runtime analysis") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr;
      toad_value_t result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_equal(toad_script_run(ctx, "values = list(-2, 0, 3);"), 0);
      expr = ts_stream_test_single_expr(
          ctx,
          "stream.of(values).filter(x => x > 0).map(x => x * 2).count();",
          &root);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, TOAD_VAL_NUMBER);
      check(fabs(result.data.number - 1.0) <= 1e-9);
      check_equal(error[0], '\0');

      toad_value_destroy(&result);
      toad_free(root);
      toad_script_free(ctx);
    }

    it("returns zero for an empty CFlow count terminal") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr = ts_stream_test_single_expr(
          ctx, "stream.of([]).map(x => x * 2).count();", &root);
      toad_value_t result = toad_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, TOAD_VAL_NUMBER);
      check(fabs(result.data.number) <= 1e-9);
      check_equal(error[0], '\0');

      toad_value_destroy(&result);
      toad_free(root);
      toad_script_free(ctx);
    }

    it("materializes toVector and toList through CFlow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *vector_root = NULL;
      toad_node_t *list_root = NULL;
      toad_node_t *vector_expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([-2, 0, 3]).filter(x => x > 0).map(x => x * 2).toVector();",
          &vector_root);
      toad_node_t *list_expr = ts_stream_test_single_expr(
          ctx,
          "[1, 2, 3].stream().filter(x => x > 1).toList();",
          &list_root);
      toad_value_t vector_result = toad_val_num(0.0);
      toad_value_t list_result = toad_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(vector_expr);
      check_not_null(list_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, vector_expr, &vector_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(vector_result.type, TOAD_VAL_VECTOR);
      check_equal(vector_result.data.vector.size, (size_t)1u);
      check(fabs(vector_result.data.vector.data[0] - 6.0) <= 1e-9);
      check_equal(error[0], '\0');

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, list_expr, &list_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(list_result.type, TOAD_VAL_LIST);
      check_equal(list_result.data.list.count, (size_t)2u);
      check(fabs(list_result.data.list.items[0].data.number - 2.0) <= 1e-9);
      check(fabs(list_result.data.list.items[1].data.number - 3.0) <= 1e-9);
      check_equal(error[0], '\0');

      toad_value_destroy(&list_result);
      toad_value_destroy(&vector_result);
      toad_free(list_root);
      toad_free(vector_root);
      toad_script_free(ctx);
    }

    it("preserves legacy collect vector-vs-list shape") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *filter_root = NULL;
      toad_node_t *map_root = NULL;
      toad_node_t *filter_expr = ts_stream_test_single_expr(
          ctx, "[1, 2, 3].stream().filter(x => x > 1).collect();",
          &filter_root);
      toad_node_t *map_expr = ts_stream_test_single_expr(
          ctx, "[1, 2, 3].stream().map(x => x * 2).collect();",
          &map_root);
      toad_value_t filter_result = toad_val_num(0.0);
      toad_value_t map_result = toad_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(filter_expr);
      check_not_null(map_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, filter_expr, &filter_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(filter_result.type, TOAD_VAL_VECTOR);
      check_equal(filter_result.data.vector.size, (size_t)2u);
      check(fabs(filter_result.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(filter_result.data.vector.data[1] - 3.0) <= 1e-9);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, map_expr, &map_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(map_result.type, TOAD_VAL_LIST);
      check_equal(map_result.data.list.count, (size_t)3u);
      check(fabs(map_result.data.list.items[0].data.number - 2.0) <= 1e-9);
      check(fabs(map_result.data.list.items[2].data.number - 6.0) <= 1e-9);

      toad_value_destroy(&map_result);
      toad_value_destroy(&filter_result);
      toad_free(map_root);
      toad_free(filter_root);
      toad_script_free(ctx);
    }

    it("does not reinterpret reduce output as a countable stream") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([1, 2, 3]).reduce(0, (acc, x) => acc + x).count();",
          &root);
      toad_value_t result = toad_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_NOT_APPLICABLE);
      check_equal(error[0], '\0');

      toad_free(root);
      toad_script_free(ctx);
    }

    it("leaves dynamic-seed reduce on the existing facade during this slice") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_node_t *root = NULL;
      toad_node_t *expr = ts_stream_test_single_expr(
          ctx, "[1, 2, 3].stream().reduce(seed, (acc, x) => acc + x);", &root);
      toad_value_t result = toad_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_NOT_APPLICABLE);
      check_equal(error[0], '\0');

      toad_free(root);
      toad_script_free(ctx);
    }
  }

  describe("legacy public run integration") {
    it("keeps interpreter and JIT parity for CFlow terminals") {
      const char *source =
          "reduced = [1, 2, 3].stream().filter(x => x > 1).map(x => x * 2)"
          ".reduce(10, (acc, x) => acc + x);"
          "counted = stream.of([-2, 0, 3]).filter(x => x > 0)"
          ".map(x => x * 2).count();"
          "vectorized = stream.of([1, 2, 3]).map(x => x * 3).toVector();"
          "listed = stream.of([1, 2, 3]).filter(x => x > 1).toList();"
          "collected_filter = stream.of([1, 2, 3]).filter(x => x > 1).collect();"
          "collected_map = stream.of([1, 2, 3]).map(x => x * 2).collect();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_vector;
      toad_value_t jit_vector;
      toad_value_t interp_list;
      toad_value_t jit_list;
      toad_value_t interp_filter_collect;
      toad_value_t jit_filter_collect;
      toad_value_t interp_map_collect;
      toad_value_t jit_map_collect;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "reduced") - 20.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "reduced") - 20.0) <= 1e-9);
      check(fabs(toad_script_get_num(interp, "counted") - 1.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "counted") - 1.0) <= 1e-9);

      interp_vector = toad_env_get(&interp->env, "vectorized");
      jit_vector = toad_env_get(&jit->env, "vectorized");
      check_equal(interp_vector.type, TOAD_VAL_VECTOR);
      check_equal(jit_vector.type, TOAD_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)3u);
      check_equal(jit_vector.data.vector.size, (size_t)3u);
      check(fabs(interp_vector.data.vector.data[2] - 9.0) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[2] - 9.0) <= 1e-9);

      interp_list = toad_env_get(&interp->env, "listed");
      jit_list = toad_env_get(&jit->env, "listed");
      check_equal(interp_list.type, TOAD_VAL_LIST);
      check_equal(jit_list.type, TOAD_VAL_LIST);
      check_equal(interp_list.data.list.count, (size_t)2u);
      check_equal(jit_list.data.list.count, (size_t)2u);

      interp_filter_collect = toad_env_get(&interp->env, "collected_filter");
      jit_filter_collect = toad_env_get(&jit->env, "collected_filter");
      check_equal(interp_filter_collect.type, TOAD_VAL_VECTOR);
      check_equal(jit_filter_collect.type, TOAD_VAL_VECTOR);
      check_equal(interp_filter_collect.data.vector.size, (size_t)2u);
      check_equal(jit_filter_collect.data.vector.size, (size_t)2u);

      interp_map_collect = toad_env_get(&interp->env, "collected_map");
      jit_map_collect = toad_env_get(&jit->env, "collected_map");
      check_equal(interp_map_collect.type, TOAD_VAL_LIST);
      check_equal(jit_map_collect.type, TOAD_VAL_LIST);
      check_equal(interp_map_collect.data.list.count, (size_t)3u);
      check_equal(jit_map_collect.data.list.count, (size_t)3u);
      check(fabs(interp_map_collect.data.list.items[2].data.number - 6.0) <= 1e-9);
      check(fabs(jit_map_collect.data.list.items[2].data.number - 6.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps text lines count aligned in interpreter and JIT") {
      const char *source =
          "line_count = stream.text(\"a\\r\\nb\\n\").lines().count();"
          "empty_count = stream.text(\"\").lines().count();"
          "coerced_empty_count = stream.text(42).lines().count();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "line_count") - 3.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "line_count") - 3.0) <= 1e-9);
      check(fabs(toad_script_get_num(interp, "empty_count") - 1.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "empty_count") - 1.0) <= 1e-9);
      check(fabs(toad_script_get_num(interp, "coerced_empty_count") - 1.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "coerced_empty_count") - 1.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps text line materialization aligned in interpreter and JIT") {
      const char *source =
          "collected = stream.text(\"a\\r\\nb\\n\").lines().collect();"
          "listed = stream.text(\"x\\ny\").lines().toList();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_collect;
      toad_value_t jit_collect;
      toad_value_t interp_list;
      toad_value_t jit_list;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);

      interp_collect = toad_env_get(&interp->env, "collected");
      jit_collect = toad_env_get(&jit->env, "collected");
      check_equal(interp_collect.type, TOAD_VAL_LIST);
      check_equal(jit_collect.type, TOAD_VAL_LIST);
      check_equal(interp_collect.data.list.count, (size_t)3u);
      check_equal(jit_collect.data.list.count, (size_t)3u);
      check(memcmp(interp_collect.data.list.items[0].data.string.data, "a", 1u) == 0);
      check(memcmp(jit_collect.data.list.items[1].data.string.data, "b", 1u) == 0);
      check_equal(interp_collect.data.list.items[2].data.string.len, (size_t)0u);
      check_equal(jit_collect.data.list.items[2].data.string.len, (size_t)0u);

      interp_list = toad_env_get(&interp->env, "listed");
      jit_list = toad_env_get(&jit->env, "listed");
      check_equal(interp_list.type, TOAD_VAL_LIST);
      check_equal(jit_list.type, TOAD_VAL_LIST);
      check_equal(interp_list.data.list.count, (size_t)2u);
      check_equal(jit_list.data.list.count, (size_t)2u);
      check(memcmp(interp_list.data.list.items[0].data.string.data, "x", 1u) == 0);
      check(memcmp(jit_list.data.list.items[1].data.string.data, "y", 1u) == 0);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps text split terminals aligned in interpreter and JIT") {
      const char *source =
          "split_count = stream.text(\",a,,b,\").split(\",\").count();"
          "tokens = stream.text(\",a,,b,\").split(\",\").toList();"
          "chars = stream.text(\"ABC\").split(\"\").collect();"
          "bad_sep_count = stream.text(\"abc\").split(42).count();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_tokens;
      toad_value_t jit_tokens;
      toad_value_t interp_chars;
      toad_value_t jit_chars;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "split_count") - 5.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "split_count") - 5.0) <= 1e-9);
      check(fabs(toad_script_get_num(interp, "bad_sep_count")) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "bad_sep_count")) <= 1e-9);

      interp_tokens = toad_env_get(&interp->env, "tokens");
      jit_tokens = toad_env_get(&jit->env, "tokens");
      check_equal(interp_tokens.type, TOAD_VAL_LIST);
      check_equal(jit_tokens.type, TOAD_VAL_LIST);
      check_equal(interp_tokens.data.list.count, (size_t)5u);
      check_equal(jit_tokens.data.list.count, (size_t)5u);
      check_equal(interp_tokens.data.list.items[0].data.string.len, (size_t)0u);
      check(memcmp(jit_tokens.data.list.items[3].data.string.data, "b", 1u) == 0);
      check_equal(jit_tokens.data.list.items[4].data.string.len, (size_t)0u);

      interp_chars = toad_env_get(&interp->env, "chars");
      jit_chars = toad_env_get(&jit->env, "chars");
      check_equal(interp_chars.type, TOAD_VAL_LIST);
      check_equal(jit_chars.type, TOAD_VAL_LIST);
      check_equal(interp_chars.data.list.count, (size_t)3u);
      check_equal(jit_chars.data.list.count, (size_t)3u);
      check(memcmp(interp_chars.data.list.items[0].data.string.data, "A", 1u) == 0);
      check(memcmp(jit_chars.data.list.items[2].data.string.data, "C", 1u) == 0);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("evaluates text split source and separator exactly once") {
      const char *source =
          "split_count = stream.text(make_text()).split(make_sep()).count();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      ts_stream_test_string_source_t interp_text = {0u, "a,b"};
      ts_stream_test_string_source_t interp_sep = {0u, ","};
      ts_stream_test_string_source_t jit_text = {0u, "a,b"};
      ts_stream_test_string_source_t jit_sep = {0u, ","};

      check_not_null(interp);
      check_not_null(jit);
      toad_env_register_func(
          &interp->env, "make_text", ts_stream_test_counted_string, &interp_text);
      toad_env_register_func(
          &interp->env, "make_sep", ts_stream_test_counted_string, &interp_sep);
      toad_env_register_func(
          &jit->env, "make_text", ts_stream_test_counted_string, &jit_text);
      toad_env_register_func(
          &jit->env, "make_sep", ts_stream_test_counted_string, &jit_sep);

      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "split_count") - 2.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "split_count") - 2.0) <= 1e-9);
      check_equal(interp_text.calls, (size_t)1u);
      check_equal(jit_text.calls, (size_t)1u);
      check_equal(interp_sep.calls, (size_t)1u);
      check_equal(jit_sep.calls, (size_t)1u);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps typed text line length MAP aligned in interpreter and JIT") {
      const char *source =
          "lengths = stream.text(\"a\\nbb\\n\").lines()"
          ".map(line => line.length()).toVector();"
          "length_list = stream.text(\"x\\nyy\").lines()"
          ".map(line => line.length()).toList();"
          "legacy_trim = stream.text(\" a \").lines()"
          ".map(line => line.trim()).toList();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_lengths;
      toad_value_t jit_lengths;
      toad_value_t interp_list;
      toad_value_t jit_list;
      toad_value_t interp_trim;
      toad_value_t jit_trim;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);

      interp_lengths = toad_env_get(&interp->env, "lengths");
      jit_lengths = toad_env_get(&jit->env, "lengths");
      check_equal(interp_lengths.type, TOAD_VAL_VECTOR);
      check_equal(jit_lengths.type, TOAD_VAL_VECTOR);
      check_equal(interp_lengths.data.vector.size, (size_t)3u);
      check_equal(jit_lengths.data.vector.size, (size_t)3u);
      check(fabs(interp_lengths.data.vector.data[0] - 1.0) <= 1e-9);
      check(fabs(interp_lengths.data.vector.data[1] - 2.0) <= 1e-9);
      check(fabs(jit_lengths.data.vector.data[2] - 0.0) <= 1e-9);

      interp_list = toad_env_get(&interp->env, "length_list");
      jit_list = toad_env_get(&jit->env, "length_list");
      check_equal(interp_list.type, TOAD_VAL_LIST);
      check_equal(jit_list.type, TOAD_VAL_LIST);
      check_equal(interp_list.data.list.count, (size_t)2u);
      check_equal(jit_list.data.list.count, (size_t)2u);
      check(fabs(interp_list.data.list.items[0].data.number - 1.0) <= 1e-9);
      check(fabs(jit_list.data.list.items[1].data.number - 2.0) <= 1e-9);

      interp_trim = toad_env_get(&interp->env, "legacy_trim");
      jit_trim = toad_env_get(&jit->env, "legacy_trim");
      check_equal(interp_trim.type, TOAD_VAL_LIST);
      check_equal(jit_trim.type, TOAD_VAL_LIST);
      check_equal(interp_trim.data.list.count, (size_t)1u);
      check_equal(jit_trim.data.list.count, (size_t)1u);
      check_equal(interp_trim.data.list.items[0].type, TOAD_VAL_STRING);
      check_equal(jit_trim.data.list.items[0].type, TOAD_VAL_STRING);
      check_equal(interp_trim.data.list.items[0].data.string.len, (size_t)1u);
      check_equal(jit_trim.data.list.items[0].data.string.len, (size_t)1u);
      check(memcmp(interp_trim.data.list.items[0].data.string.data, "a", 1u) == 0);
      check(memcmp(jit_trim.data.list.items[0].data.string.data, "a", 1u) == 0);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps typed text nonempty FILTER aligned in interpreter and JIT") {
      const char *source =
          "nonempty_count = stream.text(\"a\\n\\nbb\\n\").lines()"
          ".filter(line => line.length() > 0).count();"
          "nonempty_lines = stream.text(\"a\\n\\nbb\\n\").lines()"
          ".filter(line => line.length() > 0).toList();"
          "collected = stream.text(\"a\\n\\nbb\\n\").lines()"
          ".filter(line => line.length() > 0).collect();"
          "legacy_count = stream.text(\"a\\n\").lines()"
          ".filter(line => line.length() >= 0).count();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_lines;
      toad_value_t jit_lines;
      toad_value_t interp_collect;
      toad_value_t jit_collect;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);

      check(fabs(toad_script_get_num(interp, "nonempty_count") - 2.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "nonempty_count") - 2.0) <= 1e-9);
      check(fabs(toad_script_get_num(interp, "legacy_count") - 2.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "legacy_count") - 2.0) <= 1e-9);

      interp_lines = toad_env_get(&interp->env, "nonempty_lines");
      jit_lines = toad_env_get(&jit->env, "nonempty_lines");
      check_equal(interp_lines.type, TOAD_VAL_LIST);
      check_equal(jit_lines.type, TOAD_VAL_LIST);
      check_equal(interp_lines.data.list.count, (size_t)2u);
      check_equal(jit_lines.data.list.count, (size_t)2u);
      check(memcmp(
          interp_lines.data.list.items[0].data.string.data, "a", 1u) == 0);
      check(memcmp(
          jit_lines.data.list.items[1].data.string.data, "bb", 2u) == 0);

      interp_collect = toad_env_get(&interp->env, "collected");
      jit_collect = toad_env_get(&jit->env, "collected");
      check_equal(interp_collect.type, TOAD_VAL_LIST);
      check_equal(jit_collect.type, TOAD_VAL_LIST);
      check_equal(interp_collect.data.list.count, (size_t)2u);
      check_equal(jit_collect.data.list.count, (size_t)2u);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps bound vector sources aligned in interpreter and JIT") {
      const char *source =
          "values = [-2, 0, 3];"
          "counted = values.stream().filter(x => x > 0).map(x => x * 2).count();"
          "vectorized = stream.of(values).map(x => x + 1).toVector();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_vector;
      toad_value_t jit_vector;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "counted") - 1.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "counted") - 1.0) <= 1e-9);

      interp_vector = toad_env_get(&interp->env, "vectorized");
      jit_vector = toad_env_get(&jit->env, "vectorized");
      check_equal(interp_vector.type, TOAD_VAL_VECTOR);
      check_equal(jit_vector.type, TOAD_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)3u);
      check_equal(jit_vector.data.vector.size, (size_t)3u);
      check(fabs(interp_vector.data.vector.data[0] - (-1.0)) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[2] - 4.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps numeric list sources aligned in interpreter and JIT") {
      const char *source =
          "values = list(-2, 0, 3);"
          "counted = stream.of(values).filter(x => x > 0).map(x => x * 2).count();"
          "vectorized = stream.of(values).map(x => x + 1).toVector();";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t interp_vector;
      toad_value_t jit_vector;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "counted") - 1.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "counted") - 1.0) <= 1e-9);

      interp_vector = toad_env_get(&interp->env, "vectorized");
      jit_vector = toad_env_get(&jit->env, "vectorized");
      check_equal(interp_vector.type, TOAD_VAL_VECTOR);
      check_equal(jit_vector.type, TOAD_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)3u);
      check_equal(jit_vector.data.vector.size, (size_t)3u);
      check(fabs(interp_vector.data.vector.data[0] - (-1.0)) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[2] - 4.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("keeps heterogeneous list variables on the legacy stream facade") {
      const char *source =
          "values = list(1, \"x\", 3);"
          "answer = stream.of(values).count();";
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(toad_script_run(ctx, source), 0);
      check(fabs(toad_script_get_num(ctx, "answer") - 3.0) <= 1e-9);
      toad_script_free(ctx);
    }

    it("preserves dynamic-seed legacy semantics until that source slice migrates") {
      const char *source =
          "seed = 10;"
          "answer = [1, 2, 3].stream().reduce(seed, (acc, x) => acc + x);";
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(toad_script_run(ctx, source), 0);
      check(fabs(toad_script_get_num(ctx, "answer") - 16.0) <= 1e-9);
      toad_script_free(ctx);
    }
  }
}
