#include "../src/internal.h"
#include <toad/types.h>
#include <toad/engine.h>
#include "tinytest.h"
#include "file/fs.h"
#include <toad/script.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  toad_script_ctx_t *ctx;
  const char *script;
  int result;
} script_coro_arg_t;

typedef struct {
  const char *allowed_name;
  size_t call_count;
  char last_name[32];
} plugin_authorizer_probe_t;

typedef struct {
  size_t checks;
  size_t request_after;
} interrupt_probe_t;

static int test_context_interrupt(void *user_data) {
  interrupt_probe_t *probe = (interrupt_probe_t *)user_data;
  if (!probe) return 0;
  probe->checks++;
  return probe->checks >= probe->request_after;
}

static bool test_plugin_authorizer(const char *plugin_name, void *user_data) {
  plugin_authorizer_probe_t *probe = (plugin_authorizer_probe_t *)user_data;
  if (!probe || !plugin_name) return false;
  probe->call_count++;
  snprintf(probe->last_name, sizeof(probe->last_name), "%s", plugin_name);
  return probe->allowed_name && strcmp(probe->allowed_name, plugin_name) == 0;
}

static unsigned long ts_test_nonce(void) {
  static unsigned long counter = 0;
  return ((unsigned long)time(NULL) << 16) ^ (unsigned long)clock() ^ ++counter;
}

static void ts_test_make_name(char *buf, size_t buf_size, const char *prefix, const char *suffix) {
  const char *tail = suffix ? suffix : "";
  if (!buf || buf_size == 0 || !prefix) return;
  snprintf(buf, buf_size, "%s_%lu%s", prefix, ts_test_nonce(), tail);
}

static toad_value_t test_triple_fn(size_t argc, toad_value_t *args, toad_env_t *env, void *user_data) {
  (void)user_data;
  if (argc != 1 || args[0].type != TOAD_VAL_NUMBER) {
    return (toad_value_t){TOAD_VAL_NUMBER, .data.number = 0.0};
  }
  return (toad_value_t){TOAD_VAL_NUMBER, .data.number = args[0].data.number * 3.0};
}


spec("toad_script_basics") {
  describe("Native plugin authorization") {
    it("should deny native plugins before load and bypass built-in modules") {
      plugin_authorizer_probe_t probe = {0};
      toad_script_ctx_t *ctx = toad_script_init_with_plugin_authorizer(
          TOAD_SCRIPT_INIT_DEFAULT, test_plugin_authorizer, &probe);

      check_not_null(ctx);
      check(((int)probe.call_count) == (1));
      check(strcmp((probe.last_name), ("parser")) == 0);

      check((toad_script_load_plugin(ctx, "math")) == (0));
      check((toad_script_run(ctx, "builtin_module = import(\"io\");")) == (0));
      check(((int)probe.call_count) == (1));

      check((toad_script_load_plugin(ctx, "ta")) == (-1));
      check(((int)probe.call_count) == (2));
      check(strcmp((probe.last_name), ("ta")) == 0);
      check(((int)toad_script_get_error_code(ctx)) == ((int)TOAD_SCRIPT_ERROR_PLUGIN));
      check_not_null(strstr(toad_script_get_error(ctx), "ta"));
      check_not_null(strstr(toad_script_get_error(ctx), "denied by host policy"));

      check((toad_script_run(ctx, "import(\"ta\");")) == (-1));
      check(((int)probe.call_count) == (3));
      check_not_null(strstr(toad_script_get_error(ctx), "import 'ta'"));
      check_not_null(strstr(toad_script_get_error(ctx), "denied by host policy"));
      toad_script_free(ctx);
    }

    it("should authorize only the first load of a native plugin") {
      plugin_authorizer_probe_t probe = {.allowed_name = "ta"};
      toad_script_ctx_t *ctx = toad_script_init_with_plugin_authorizer(
          TOAD_SCRIPT_INIT_BARE, test_plugin_authorizer, &probe);

      check_not_null(ctx);
      check((toad_script_load_plugin(ctx, "ta")) == (0));
      check(((int)probe.call_count) == (1));
      check(strcmp((probe.last_name), ("ta")) == 0);

      probe.allowed_name = NULL;
      check((toad_script_load_plugin(ctx, "ta")) == (0));
      check(((int)probe.call_count) == (1));
      toad_script_free(ctx);
    }

    it("should prefer the prefixless native plugin filename") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(toad_script_load_plugin(ctx, "loader_fixture"), 0);
      toad_script_free(ctx);
    }

    it("should call a canonical Function-only plugin in interpreter and JIT") {
      const char *source =
          "import(\"loader_function\");"
          "answer=loader_function.double(3.5);"
          "mixed=loader_function.increment(4)"
          "+loader_function.add(1.25,2.5)"
          "+loader_function.long_to_double(7)"
          "+loader_function.float_to_double(1.5);";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "answer") - 7.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "answer") - 7.0) <= 1e-9);
      check(fabs(toad_script_get_num(interp, "mixed") - 18.0) <= 1e-6);
      check(fabs(toad_script_get_num(jit, "mixed") - 18.0) <= 1e-6);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("should preserve exact integer values through interpreter and JIT plugin calls") {
      const char *source = "exact = loader_function.long_exact(big);";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      int64_t exact_input;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(toad_script_load_plugin(interp, "loader_function"), 0);
      check_equal(toad_script_load_plugin(jit, "loader_function"), 0);

#if LONG_MAX > 2147483647L
      exact_input = INT64_C(9007199254740993);
#else
      exact_input = (int64_t)LONG_MAX;
#endif
      toad_env_set(&interp->env, "big", toad_val_int(exact_input));
      toad_env_set(&jit->env, "big", toad_val_int(exact_input));

      check_equal(toad_script_run(interp, source), 0);
      check_equal(toad_script_run_jit(jit, source), 0);
      check(fabs(toad_script_get_num(interp, "exact") - 1.0) <= 1e-9);
      check(fabs(toad_script_get_num(jit, "exact") - 1.0) <= 1e-9);

      toad_script_free(jit);
      toad_script_free(interp);
    }

    it("should load finance from the assembled plugins") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(toad_script_load_plugin(ctx, "fin"), 0);
      toad_script_free(ctx);
    }

    it("should report the loader stage when a native plugin cannot be opened") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(toad_script_load_plugin(ctx, "definitely_missing_loader_fixture"), -1);
      check_equal(toad_script_get_error_code(ctx), TOAD_SCRIPT_ERROR_PLUGIN);
      check_not_null(strstr(toad_script_get_error(ctx), "open"));
      toad_script_free(ctx);
    }

    it("should reject path traversal in a logical native plugin name") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(toad_script_load_plugin(ctx, "../loader_fixture"), -1);
      check_equal(toad_script_get_error_code(ctx), TOAD_SCRIPT_ERROR_ARGUMENT);
      check_not_null(strstr(toad_script_get_error(ctx), "invalid plugin name"));
      toad_script_free(ctx);
    }

    it("should reject malformed qualified native plugin names") {
      const char *invalid[] = {
          ".Image", "Image.", "Image..Processor", "Image.9Processor",
          "Image./Processor", "Image\\Processor"};
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        check_equal(toad_script_load_plugin(ctx, invalid[i]), -1);
        check_equal(
            toad_script_get_error_code(ctx), TOAD_SCRIPT_ERROR_ARGUMENT);
        check_not_null(strstr(
            toad_script_get_error(ctx), "invalid plugin name"));
      }
      toad_script_free(ctx);
    }
  }

  describe("Memory policy") {
    it("should provide bounded profiles and report usage") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      toad_script_memory_policy_t policy;
      toad_script_memory_stats_t stats;

      check_not_null(ctx);
      check((toad_script_memory_policy_init(TOAD_SCRIPT_MEMORY_STREAMING, &policy)) == (0));
      check_true(policy.max_external_value_bytes > 0);
      check_true(policy.max_external_value_bytes <= policy.max_task_bytes);
      check_true(policy.max_task_bytes <= policy.max_context_bytes);
      check((toad_script_set_memory_policy(ctx, &policy)) == (0));
      check((toad_script_run(ctx, "memory_policy_probe = \"ok\";")) == (0));
      check((toad_script_get_memory_stats(ctx, &stats)) == (0));
      check_true(stats.context_bytes > 0);
      check_true(stats.peak_context_bytes >= stats.context_bytes);

      policy.max_external_value_bytes = 0;
      check((toad_script_set_memory_policy(ctx, &policy)) == (-1));
      toad_script_free(ctx);
    }

    it("should count retained closure snapshots in context_bytes") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      toad_script_memory_stats_t before;
      toad_script_memory_stats_t after;

      check_not_null(ctx);
      check((toad_script_get_memory_stats(ctx, &before)) == (0));
      /* Each iteration creates a closure that captures a fresh 1 KiB string;
       * the captured snapshot is retained by the root env closure chain. */
      check((toad_script_run(ctx, "for (i = 0; i < 50; i += 1) { txt = repeat(\"x\", 1024); f = (x) => x + txt; } result = 1;")) == (0));
      check((toad_script_get_memory_stats(ctx, &after)) == (0));
      check_true(after.context_bytes > before.context_bytes + 1024);
      toad_script_free(ctx);
    }
  }

  describe("Context interruption") {
    it("should cooperatively interrupt legacy interpreter and JIT runs without breaking imports") {
      const char *source =
          "import(\"io\");"
          "i = 0;"
          "while (i < 1000000) { i = i + 1; };"
          "answer = 42;";
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      interrupt_probe_t interp_probe = {.request_after = 2};
      interrupt_probe_t jit_probe = {.request_after = 2};

      check_not_null(interp);
      check_not_null(jit);

      check_equal(toad_script_set_interrupt(interp, test_context_interrupt, &interp_probe), 0);
      check_equal(toad_script_run(interp, source), -1);
      check_equal(toad_script_get_error_code(interp), TOAD_SCRIPT_ERROR_CANCELLED);
      check_not_null(strstr(toad_script_get_error(interp), "interrupted"));
      check_true(interp_probe.checks >= interp_probe.request_after);

      check_equal(toad_script_set_interrupt(jit, test_context_interrupt, &jit_probe), 0);
      check_equal(toad_script_run_jit(jit, source), -1);
      check_equal(toad_script_get_error_code(jit), TOAD_SCRIPT_ERROR_CANCELLED);
      check_not_null(strstr(toad_script_get_error(jit), "interrupted"));
      check_true(jit_probe.checks >= jit_probe.request_after);

      check_equal(toad_script_set_interrupt(interp, NULL, NULL), 0);
      check_equal(toad_script_run(interp, "import(\"io\"); answer = 6 * 7;"), 0);
      check_equal(toad_script_get_num(interp, "answer"), 42.0);

      check_equal(toad_script_set_interrupt(jit, NULL, NULL), 0);
      check_equal(toad_script_run_jit(jit, "import(\"io\"); answer = 6 * 7;"), 0);
      check_equal(toad_script_get_num(jit, "answer"), 42.0);

      toad_script_free(interp);
      toad_script_free(jit);
    }
  }

  describe("Basics") {
    it("should produce the same result through the interpreter and JIT") {
      toad_script_ctx_t *interp = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      toad_script_ctx_t *jit = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);

      check_not_null(interp);
      check_not_null(jit);
      if (interp && jit) {
        check_equal(toad_script_run(interp, "answer = 6 * 7;"), 0);
        check_equal(toad_script_run_jit(jit, "answer = 6 * 7;"), 0);
        check_equal(toad_script_get_num(interp, "answer"), 42.0);
        check_equal(toad_script_get_num(jit, "answer"), 42.0);
      }

      toad_script_free(interp);
      toad_script_free(jit);
    }

    it("should execute math scripts") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check((toad_script_run(ctx, " x = 10; y = x * 2;")) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "y")) - (double)(20.0)) <= (double)(0.001));

      toad_script_free(ctx);
    }

    it("should support prefixed binding helpers without breaking old ones") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      toad_script_bind_num(ctx, "x", 7.0);
      check(fabs((double)(toad_script_get_num(ctx, "x")) - (double)(7.0)) <= (double)(0.001));
      toad_script_bind_num(ctx, "x", 9.0);
      check(fabs((double)(toad_script_get_num(ctx, "x")) - (double)(9.0)) <= (double)(0.001));

      toad_script_free(ctx);
    }

    it("should expose a stable version string") {
      check(strcmp((toad_script_version()), (TOAD_SCRIPT_VERSION_STRING)) == 0);
    }

    it("should expose host map and borrowed list values") {
      toad_value_t items[2] = {
          {TOAD_VAL_NUMBER, .data.number = 3.0},
          {TOAD_VAL_NUMBER, .data.number = 5.0},
      };
      toad_value_t list = toad_script_value_list_borrowed(items, 2);
      toad_value_t map = toad_script_value_map();
      toad_script_value_map_iterator_t iterator;
      const char *key = NULL;
      toad_value_t entry;

      check((list.type) == (TOAD_VAL_LIST));
      check((list.data.list.items) == (items));
      check((list.data.list.count) == (2));
      check((list.data.list.heap_owned) == (0));

      toad_script_value_map_set(&map, "values", list);
      iterator = toad_script_value_map_iter_begin(&map);
      check((toad_script_value_map_iter_next(&iterator, &key, &entry)) == (1));
      check(strcmp((key), ("values")) == 0);
      check((entry.type) == (TOAD_VAL_LIST));
      check((entry.data.list.count) == (2));
      check(fabs((double)(entry.data.list.items[1].data.number) - (double)(5.0)) <= (double)(0.001));
      check((toad_script_value_map_iter_next(&iterator, &key, &entry)) == (0));

      toad_map_free(&map);
    }

    it("should reuse parsed ast for repeated runs of the same script") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      toad_node_t *first_expr = NULL;
      toad_node_t *assign = NULL;
      check_not_null(ctx);

      check((toad_script_run(ctx, "x = 1; y = x + 2;")) == (0));
      first_expr = ctx->expr;
      check_not_null(first_expr);

      check((toad_script_run(ctx, "x = 1; y = x + 2;")) == (0));
      check((ctx->expr) == (first_expr));
      check(fabs((double)(toad_script_get_num(ctx, "y")) - (double)(3.0)) <= (double)(0.001));

      check((toad_script_run(ctx, "x = 2; y = x + 2;")) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "y")) - (double)(4.0)) <= (double)(0.001));
      check(strcmp((ctx->expr_source), ("x = 2; y = x + 2;")) == 0);
      check((ctx->expr->type) == (TOAD_NODE_BLOCK));
      check(((int)ctx->expr->data.block.count) == (2));
      assign = ctx->expr->data.block.statements[0];
      check_not_null(assign);
      check((assign->type) == (TOAD_NODE_ASSIGNMENT));
      check_not_null(assign->data.assignment.value);
      check(assign->data.assignment.value->type == TOAD_NODE_NUMBER ||
            assign->data.assignment.value->type == TOAD_NODE_INTEGER);
      if (assign->data.assignment.value->type == TOAD_NODE_INTEGER) {
        check(((int)assign->data.assignment.value->data.integer) == (2));
      } else {
        check(fabs((double)(assign->data.assignment.value->data.number) - (double)(2.0)) <= (double)(0.001));
      }

      toad_script_free(ctx);
    }

    it("should support the var keyword") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      check((toad_script_run(ctx, " var "
                                         "a = 123; var b = 456; var c = a + b;")) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "c")) - (double)(579.0)) <= (double)(0.001));
      toad_script_free(ctx);
    }

    it("should support complex expressions and multiple assignments") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      const char *script = " a = 5; "
                           "b = 10; "
                           "c = (a + b) * 2; "
                           "d = c / 3;";
      check((toad_script_run(ctx, script)) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "c")) - (double)(30.0)) <= (double)(0.001));
      check(fabs((double)(toad_script_get_num(ctx, "d")) - (double)(10.0)) <= (double)(0.001));
      toad_script_free(ctx);
    }

    it("should persist variables across multiple runs") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      check((toad_script_run(ctx, " x = 100;")) == (0));
      check((toad_script_run(ctx, " y = x + 50;")) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "y")) - (double)(150.0)) <= (double)(0.001));
      toad_script_free(ctx);
    }

    it("should support simple control flow") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      const char *script = " sum = 0; "
                           "i = 0; "
                           "while (i < 5) { "
                           "  sum = sum + i; "
                           "  i = i + 1; "
                           "}";
      check((toad_script_run(ctx, script)) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "sum")) - (double)(10.0)) <= (double)(0.001)); // 0+1+2+3+4
      toad_script_free(ctx);
    }
  }

  describe("DateTime") {
    it("should support date parsing and now()") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);

      // 2024-01-01 12:00:00 UTC is 1704110400
      int res = toad_script_run(ctx, "t = date(\"2024-01-01 12:00:00\");");
      if (res != 0) printf("DateTime Parse Error: %s\n", toad_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "t")) - (double)(1704110400.0)) <= (double)(1.0));

      check((toad_script_run(ctx, "curr = now();")) == (0));
      check((toad_script_get_num(ctx, "curr")) > (1700000000.0));

      toad_script_free(ctx);
    }
  }

  describe("String Utilities") {
    it("should support string conversions and tokens") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);

      const char *script = " var s = \"123.45\";"
                           "var n = str_to_num(s);"
                           "var s2 = num_to_str(n);"
                           "var data = \"apple,banana,cherry\";"
                           "var count = str_count(data, \",\");"
                           "var item = str_token(data, \",\", 1);";

      int res_str = toad_script_run(ctx, script);
      if (res_str != 0) printf("String Utils Error: %s\n", toad_script_get_error(ctx));
      check((res_str) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "n")) - (double)(123.45)) <= (double)(0.01));
      check(fabs((double)(toad_script_get_num(ctx, "count")) - (double)(3.0)) <= (double)(0.1));

      toad_script_free(ctx);
    }
  }

  describe("File System") {
    it("should support file operations") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);

      // 1. Write
      check((toad_script_run(ctx, "res "
                                         "= write_file(\"test_ts.txt\", \"Hello Turbo Script!\");")) == (0));
      double write_res = toad_script_get_num(ctx, "res");
      printf("DEBUG: file_write res = %g\n", write_res);
      check(fabs((double)(write_res) - (double)(0.0)) <= (double)(0.1));

      // 2. Exists
      check((toad_script_run(ctx, ""
                                         "exists = file_exists(\"test_ts.txt\");")) == (0));
      double exists_res = toad_script_get_num(ctx, "exists");
      printf("DEBUG: file_exists res = %g\n", exists_res);
      check(fabs((double)(exists_res) - (double)(1.0)) <= (double)(0.1));

      // 3. Read
      check((toad_script_run(ctx, "data "
                                         "= read_file(\"test_ts.txt\");")) == (0));
      const char *data = toad_script_get_str(ctx, "data");
      check_not_null(data);
      check(strcmp((data), ("Hello Turbo Script!")) == 0);

      // 4. Remove
      check((toad_script_run(ctx, "del "
                                         "= file_remove(\"test_ts.txt\");")) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "del")) - (double)(0.0)) <= (double)(0.1));

      // 5. Not exists
      check((toad_script_run(ctx, ""
                                         "exists2 = file_exists(\"test_ts.txt\");")) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "exists2")) - (double)(0.0)) <= (double)(0.1));

      toad_script_free(ctx);
    }
  }

  describe("Vector Functions") {
    it("should support split, avg, len, sum, min, max") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      const char *script = " data = \"10,20,30,40,50\";"
                           "v = split(data, \",\");"
                           "a = vec.avg(v);"
                           "l = vec.len(v);"
                           "s = vec.sum(v);"
                           "mi = vec.min(v);"
                           "ma = vec.max(v);";

      check((toad_script_run(ctx, script)) == (0));
      check(fabs((double)(toad_script_get_num(ctx, "a")) - (double)(30.0)) <= (double)(0.1));
      check(fabs((double)(toad_script_get_num(ctx, "l")) - (double)(5.0)) <= (double)(0.1));
      check(fabs((double)(toad_script_get_num(ctx, "s")) - (double)(150.0)) <= (double)(0.1));
      check(fabs((double)(toad_script_get_num(ctx, "mi")) - (double)(10.0)) <= (double)(0.1));
      check(fabs((double)(toad_script_get_num(ctx, "ma")) - (double)(50.0)) <= (double)(0.1));

      toad_script_free(ctx);
    }
  }

  describe("Error Handling") {
    it("should report parse errors") {
      toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
      check((toad_script_run(ctx, "x = 10 + * 5")) == (-1));
      check_not_null(toad_script_get_error(ctx));
      check(((int)toad_script_get_error_code(ctx)) == ((int)TOAD_SCRIPT_ERROR_PARSE));
      check_not_null(strstr(toad_script_get_error(ctx), "line 1"));
      check_not_null(strstr(toad_script_get_error(ctx), "near '*'"));
      toad_script_free(ctx);
    }
  }

}
