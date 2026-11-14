#include "../src/internal.h"
#include "../src/host/plugin_databind.h"
#include <turboscript/engine.h>
#include "tinytest.h"

#include <string.h>

static int map_integer(
    const tbs_value_t *map, const char *name, int64_t expected) {
  tbs_value_t value;
  if (!tbs_value_is_object_like(map) ||
      !tbs_map_has(map, name))
    return 0;
  value = tbs_map_get(map, name);
  return value.type == TBS_VAL_INTEGER &&
         value.data.integer == expected;
}

static int map_string(
    const tbs_value_t *map, const char *name, const char *expected) {
  tbs_value_t value;
  size_t length;
  if (!expected || !tbs_value_is_object_like(map) ||
      !tbs_map_has(map, name))
    return 0;
  value = tbs_map_get(map, name);
  length = strlen(expected);
  return value.type == TBS_VAL_STRING &&
         value.data.string.len == length &&
         memcmp(value.data.string.data, expected, length) == 0;
}

spec("TurboScript generated DataBind Service Plugin binding") {
  it("rejects invalid bindings without staging output storage") {
    tbs_env_t env;
    tbs_env_init(&env);
    /* Repetition makes this early-return path visible to leak sanitizers. */
    for (size_t i = 0u; i < 128u; ++i) {
      tbs_value_t result =
          ts_plugin_databind_call(0u, NULL, &env, NULL);
      check_equal(result.type, TBS_VAL_NUMBER);
      check_equal(result.data.number, 0.0);
      check_true(env.aborted);
      check_contains(env.error_msg, "invalid DataBind Service binding");
      tbs_value_destroy(&result);
    }
    tbs_env_free(&env);
  }

  it("rejects missing argument storage before invoking the Service") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    void *binding;
    tbs_value_t result;
    check_not_null(ctx);
    check_equal(turbo_script_run(ctx, "import(\"Image.ImageProcessor\");"), 0);
    check_equal(ctx->plugin_count, 1u);
    binding = ts_plugin_databind_find(ctx->plugins[0], "Image.Codec.Decode");
    check_not_null(binding);
    result = ts_plugin_databind_call(1u, NULL, &ctx->env, binding);
    check_equal(result.type, TBS_VAL_NUMBER);
    check_true(ctx->env.aborted);
    check_contains(ctx->env.error_msg, "argument storage is missing");
    tbs_value_destroy(&result);
    turbo_script_free(ctx);
  }

  it("rejects an invalid argument shape without allocating a result map") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    void *binding;
    tbs_value_t argument = tbs_val_int(12);
    check_not_null(ctx);
    check_equal(turbo_script_run(ctx, "import(\"Image.ImageProcessor\");"), 0);
    binding = ts_plugin_databind_find(ctx->plugins[0], "Image.Codec.Decode");
    check_not_null(binding);
    for (size_t i = 0u; i < 128u; ++i) {
      tbs_value_t result =
          ts_plugin_databind_call(1u, &argument, &ctx->env, binding);
      check_equal(result.type, TBS_VAL_NUMBER);
      check_contains(ctx->env.error_msg, "expects one map/object argument");
      tbs_value_destroy(&result);
    }
    turbo_script_free(ctx);
  }

  it("keeps earlier results independent of later Service calls") {
    const char *source =
        "import(\"Image.ImageProcessor\");"
        "first=Image.Codec.Decode(map {width:12});"
        "second=Image.Codec.Decode(map {width:7,scale:3});";
    for (int use_jit = 0; use_jit != 2; ++use_jit) {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      tbs_value_t first;
      tbs_value_t second;
      check_not_null(ctx);
      check_equal(use_jit ? turbo_script_run_jit(ctx, source)
                          : turbo_script_run(ctx, source), 0);
      first = tbs_env_get(&ctx->env, "first");
      second = tbs_env_get(&ctx->env, "second");
      check_true(map_integer(&first, "pixels", 48));
      check_true(map_integer(&second, "pixels", 21));
      turbo_script_free(ctx);
    }
  }

  it("binds a generated Service with defaults in interpreter and JIT") {
    const char *source =
        "import(\"Image.ImageProcessor\");"
        "result=Image.Codec.Decode(map {width:12});";
    turbo_script_ctx_t *interp =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    turbo_script_ctx_t *jit =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    tbs_value_t interp_result;
    tbs_value_t jit_result;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(turbo_script_run(interp, source), 0);
    check_equal(turbo_script_run_jit(jit, source), 0);

    interp_result = tbs_env_get(&interp->env, "result");
    jit_result = tbs_env_get(&jit->env, "result");
    check_true(map_integer(&interp_result, "pixels", 48));
    check_true(map_integer(&jit_result, "pixels", 48));

    turbo_script_free(jit);
    turbo_script_free(interp);
  }

  it("runs DataBind validation before generated Service invocation") {
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(
        turbo_script_run(
            ctx,
            "import(\"Image.ImageProcessor\");"
            "result=Image.Codec.Decode(map {width:0});") != 0);
    check_not_null(strstr(
        turbo_script_get_error(ctx),
        "DataBind Service input binding failed"));

    turbo_script_free(ctx);
  }

  it("returns generated typed Service errors as structured data") {
    const char *source =
        "import(\"ErrorPlugin.StorePlugin\");"
        "result=ErrorPlugin.Store.Read(map {id:7});";
    turbo_script_ctx_t *interp =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    turbo_script_ctx_t *jit =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    tbs_value_t interp_result;
    tbs_value_t jit_result;
    tbs_value_t interp_error;
    tbs_value_t jit_error;
    tbs_value_t interp_payload;
    tbs_value_t jit_payload;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(turbo_script_run(interp, source), 0);
    check_equal(turbo_script_run_jit(jit, source), 0);

    interp_result = tbs_env_get(&interp->env, "result");
    jit_result = tbs_env_get(&jit->env, "result");
    check_true(tbs_map_has(&interp_result, "error"));
    check_true(tbs_map_has(&jit_result, "error"));

    interp_error = tbs_map_get(&interp_result, "error");
    jit_error = tbs_map_get(&jit_result, "error");
    check_true(map_string(&interp_error, "type", "NotFound"));
    check_true(map_string(&jit_error, "type", "NotFound"));

    interp_payload = tbs_map_get(&interp_error, "payload");
    jit_payload = tbs_map_get(&jit_error, "payload");
    check_true(map_integer(&interp_payload, "id", 7));
    check_true(map_integer(&jit_payload, "id", 7));

    turbo_script_free(jit);
    turbo_script_free(interp);
  }

  it("keeps native business status distinct from typed Service errors") {
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(
        turbo_script_run(
            ctx,
            "import(\"ErrorPlugin.StorePlugin\");"
            "result=ErrorPlugin.Store.Read(map {id:99});") != 0);
    check_not_null(strstr(
        turbo_script_get_error(ctx),
        "returned native status -9"));

    turbo_script_free(ctx);
  }

  it("returns generated Service success through the same BindingPlan") {
    const char *source =
        "import(\"ErrorPlugin.StorePlugin\");"
        "result=ErrorPlugin.Store.Read(map {id:3});";
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    tbs_value_t result;

    check_not_null(ctx);
    check_equal(turbo_script_run(ctx, source), 0);
    result = tbs_env_get(&ctx->env, "result");
    check_true(map_integer(&result, "value", 30));
    check_false(tbs_map_has(&result, "error"));

    turbo_script_free(ctx);
  }
}
