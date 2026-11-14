#include "../src/internal.h"
#include "../src/host/plugin_databind.h"
#include <toad/engine.h>
#include "tinytest.h"

#include <string.h>

static int map_integer(
    const toad_value_t *map, const char *name, int64_t expected) {
  toad_value_t value;
  if (!toad_value_is_object_like(map) ||
      !toad_map_has(map, name))
    return 0;
  value = toad_map_get(map, name);
  return value.type == TOAD_VAL_INTEGER &&
         value.data.integer == expected;
}

static int map_string(
    const toad_value_t *map, const char *name, const char *expected) {
  toad_value_t value;
  size_t length;
  if (!expected || !toad_value_is_object_like(map) ||
      !toad_map_has(map, name))
    return 0;
  value = toad_map_get(map, name);
  length = strlen(expected);
  return value.type == TOAD_VAL_STRING &&
         value.data.string.len == length &&
         memcmp(value.data.string.data, expected, length) == 0;
}

spec("Toad generated DataBind Service Plugin binding") {
  it("rejects invalid bindings without staging output storage") {
    toad_env_t env;
    toad_env_init(&env);
    /* Repetition makes this early-return path visible to leak sanitizers. */
    for (size_t i = 0u; i < 128u; ++i) {
      toad_value_t result =
          toad_plugin_databind_call(0u, NULL, &env, NULL);
      check_equal(result.type, TOAD_VAL_NUMBER);
      check_equal(result.data.number, 0.0);
      check_true(env.aborted);
      check_contains(env.error_msg, "invalid DataBind Service binding");
      toad_value_destroy(&result);
    }
    toad_env_free(&env);
  }

  it("rejects missing argument storage before invoking the Service") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
    void *binding;
    toad_value_t result;
    check_not_null(ctx);
    check_equal(toad_script_run(ctx, "import(\"Image.ImageProcessor\");"), 0);
    check_equal(ctx->plugin_count, 1u);
    binding = toad_plugin_databind_find(ctx->plugins[0], "Image.Codec.Decode");
    check_not_null(binding);
    result = toad_plugin_databind_call(1u, NULL, &ctx->env, binding);
    check_equal(result.type, TOAD_VAL_NUMBER);
    check_true(ctx->env.aborted);
    check_contains(ctx->env.error_msg, "argument storage is missing");
    toad_value_destroy(&result);
    toad_script_free(ctx);
  }

  it("rejects an invalid argument shape without allocating a result map") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
    void *binding;
    toad_value_t argument = toad_val_int(12);
    check_not_null(ctx);
    check_equal(toad_script_run(ctx, "import(\"Image.ImageProcessor\");"), 0);
    binding = toad_plugin_databind_find(ctx->plugins[0], "Image.Codec.Decode");
    check_not_null(binding);
    for (size_t i = 0u; i < 128u; ++i) {
      toad_value_t result =
          toad_plugin_databind_call(1u, &argument, &ctx->env, binding);
      check_equal(result.type, TOAD_VAL_NUMBER);
      check_contains(ctx->env.error_msg, "expects one map/object argument");
      toad_value_destroy(&result);
    }
    toad_script_free(ctx);
  }

  it("keeps earlier results independent of later Service calls") {
    const char *source =
        "import(\"Image.ImageProcessor\");"
        "first=Image.Codec.Decode(map {width:12});"
        "second=Image.Codec.Decode(map {width:7,scale:3});";
    for (int use_jit = 0; use_jit != 2; ++use_jit) {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_value_t first;
      toad_value_t second;
      check_not_null(ctx);
      check_equal(use_jit ? toad_script_run_jit(ctx, source)
                          : toad_script_run(ctx, source), 0);
      first = toad_env_get(&ctx->env, "first");
      second = toad_env_get(&ctx->env, "second");
      check_true(map_integer(&first, "pixels", 48));
      check_true(map_integer(&second, "pixels", 21));
      toad_script_free(ctx);
    }
  }

  it("binds a generated Service with defaults in interpreter and JIT") {
    const char *source =
        "import(\"Image.ImageProcessor\");"
        "result=Image.Codec.Decode(map {width:12});";
    toad_script_ctx_t *interp =
        toad_script_init(TOAD_SCRIPT_INIT_BARE);
    toad_script_ctx_t *jit =
        toad_script_init(TOAD_SCRIPT_INIT_BARE);
    toad_value_t interp_result;
    toad_value_t jit_result;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(toad_script_run(interp, source), 0);
    check_equal(toad_script_run_jit(jit, source), 0);

    interp_result = toad_env_get(&interp->env, "result");
    jit_result = toad_env_get(&jit->env, "result");
    check_true(map_integer(&interp_result, "pixels", 48));
    check_true(map_integer(&jit_result, "pixels", 48));

    toad_script_free(jit);
    toad_script_free(interp);
  }

  it("runs DataBind validation before generated Service invocation") {
    toad_script_ctx_t *ctx =
        toad_script_init(TOAD_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(
        toad_script_run(
            ctx,
            "import(\"Image.ImageProcessor\");"
            "result=Image.Codec.Decode(map {width:0});") != 0);
    check_not_null(strstr(
        toad_script_get_error(ctx),
        "DataBind Service input binding failed"));

    toad_script_free(ctx);
  }

  it("returns generated typed Service errors as structured data") {
    const char *source =
        "import(\"ErrorPlugin.StorePlugin\");"
        "result=ErrorPlugin.Store.Read(map {id:7});";
    toad_script_ctx_t *interp =
        toad_script_init(TOAD_SCRIPT_INIT_BARE);
    toad_script_ctx_t *jit =
        toad_script_init(TOAD_SCRIPT_INIT_BARE);
    toad_value_t interp_result;
    toad_value_t jit_result;
    toad_value_t interp_error;
    toad_value_t jit_error;
    toad_value_t interp_payload;
    toad_value_t jit_payload;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(toad_script_run(interp, source), 0);
    check_equal(toad_script_run_jit(jit, source), 0);

    interp_result = toad_env_get(&interp->env, "result");
    jit_result = toad_env_get(&jit->env, "result");
    check_true(toad_map_has(&interp_result, "error"));
    check_true(toad_map_has(&jit_result, "error"));

    interp_error = toad_map_get(&interp_result, "error");
    jit_error = toad_map_get(&jit_result, "error");
    check_true(map_string(&interp_error, "type", "NotFound"));
    check_true(map_string(&jit_error, "type", "NotFound"));

    interp_payload = toad_map_get(&interp_error, "payload");
    jit_payload = toad_map_get(&jit_error, "payload");
    check_true(map_integer(&interp_payload, "id", 7));
    check_true(map_integer(&jit_payload, "id", 7));

    toad_script_free(jit);
    toad_script_free(interp);
  }

  it("keeps native business status distinct from typed Service errors") {
    toad_script_ctx_t *ctx =
        toad_script_init(TOAD_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(
        toad_script_run(
            ctx,
            "import(\"ErrorPlugin.StorePlugin\");"
            "result=ErrorPlugin.Store.Read(map {id:99});") != 0);
    check_not_null(strstr(
        toad_script_get_error(ctx),
        "returned native status -9"));

    toad_script_free(ctx);
  }

  it("returns generated Service success through the same BindingPlan") {
    const char *source =
        "import(\"ErrorPlugin.StorePlugin\");"
        "result=ErrorPlugin.Store.Read(map {id:3});";
    toad_script_ctx_t *ctx =
        toad_script_init(TOAD_SCRIPT_INIT_BARE);
    toad_value_t result;

    check_not_null(ctx);
    check_equal(toad_script_run(ctx, source), 0);
    result = toad_env_get(&ctx->env, "result");
    check_true(map_integer(&result, "value", 30));
    check_false(toad_map_has(&result, "error"));

    toad_script_free(ctx);
  }
}
