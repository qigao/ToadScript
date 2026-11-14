#include "tinytest.h"

#include <toad/module.h>
#include "threads/threads.h"
#include "../src/host/internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  TEST_MAX_DEPTH = 64,
  TEST_MAX_NODES = 65536,
  TEST_MAX_BYTES = 4 * 1024 * 1024,
};

static ts_host_value_limits_t test_limits(void) {
  ts_host_value_limits_t limits = {
      TEST_MAX_DEPTH,
      TEST_MAX_NODES,
      TEST_MAX_BYTES,
  };
  return limits;
}

static void check_no_published_value(toad_script_result_t *result) {
  toad_script_value_view_t actual = {0};
  check_equal(toad_script_result_get_value(result, &actual), TOAD_SCRIPT_STATUS_INVALID_STATE);
}

static void check_empty_result(toad_script_result_t *result) {
  toad_script_value_view_t value = {0};
  toad_script_error_info_t error = {0};
  check_equal(toad_script_result_get_value(result, &value), TOAD_SCRIPT_STATUS_INVALID_STATE);
  check_equal(toad_script_result_get_error(result, &error), TOAD_SCRIPT_STATUS_INVALID_STATE);
}

typedef struct result_thread_probe_s {
  toad_script_result_t *result;
  toad_script_ctx_t *ctx;
  toad_script_status_t getter_status;
  toad_script_status_t context_status;
  toad_script_status_t reset_status;
  toad_script_status_t destroy_status;
} result_thread_probe_t;

typedef struct context_thread_probe_s {
  toad_script_ctx_t *ctx;
  toad_script_status_t create_status;
  toad_script_status_t conversion_status;
  int conversion_type;
} context_thread_probe_t;

static void result_thread_probe_run(void *arg) {
  result_thread_probe_t *probe = (result_thread_probe_t *)arg;
  toad_script_value_view_t value = {0};
  probe->getter_status = toad_script_result_get_value(probe->result, &value);
  probe->context_status = ts_host_result_check_context(probe->result, probe->ctx);
  probe->reset_status = ts_host_result_reset_checked(probe->result);
  probe->destroy_status = ts_host_result_destroy_checked(probe->result);
}

static void context_thread_probe_run(void *arg) {
  context_thread_probe_t *probe = (context_thread_probe_t *)arg;
  toad_script_result_t *created = NULL;
  toad_script_value_view_t input = {.kind = TOAD_SCRIPT_VALUE_NULL};
  toad_value_t converted = toad_val_int(99);
  ts_host_value_limits_t limits = test_limits();

  probe->create_status = toad_script_result_create(probe->ctx, &created);
  if (created) toad_script_result_destroy(created);
  probe->conversion_status = ts_host_value_from_view(probe->ctx, &input, &limits, &converted);
  probe->conversion_type = converted.type;
  toad_value_destroy(&converted);
}

spec("toad_script_host_result") {
  static toad_script_ctx_t *ctx;
  static toad_script_result_t *result;

  before_each() {
    ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
    result = NULL;
    check_not_null(ctx);
    check_equal(toad_script_result_create(ctx, &result), TOAD_SCRIPT_STATUS_OK);
    check_not_null(result);
  }

  after_each() {
    toad_script_result_destroy(result);
    toad_script_free(ctx);
    result = NULL;
    ctx = NULL;
  }

  describe("owned result values") {
    it("round-trips every scalar kind") {
      const toad_script_value_view_t inputs[] = {
          {.kind = TOAD_SCRIPT_VALUE_NULL},
          {.kind = TOAD_SCRIPT_VALUE_BOOL, .as.boolean = 1},
          {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = INT64_C(-42)},
          {.kind = TOAD_SCRIPT_VALUE_NUMBER, .as.number = 3.25},
          {.kind = TOAD_SCRIPT_VALUE_STRING, .as.string = {"hello", 5}},
      };
      ts_host_value_limits_t limits = test_limits();

      for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        toad_script_value_view_t actual = {0};
        check_equal(ts_host_result_store_view(result, &inputs[i], &limits), TOAD_SCRIPT_STATUS_OK);
        check_equal(toad_script_result_get_value(result, &actual), TOAD_SCRIPT_STATUS_OK);
        check_equal(actual.kind, inputs[i].kind);
        if (inputs[i].kind == TOAD_SCRIPT_VALUE_BOOL) check_equal(actual.as.boolean, (uint8_t)1);
        if (inputs[i].kind == TOAD_SCRIPT_VALUE_INT64)
          check_equal(actual.as.integer, INT64_C(-42));
        if (inputs[i].kind == TOAD_SCRIPT_VALUE_NUMBER) check_equal(actual.as.number, 3.25);
        if (inputs[i].kind == TOAD_SCRIPT_VALUE_STRING) {
          check_equal(actual.as.string.size, (size_t)5);
          check_equal(actual.as.string.data, "hello");
        }
      }
    }

    it("preserves an empty string") {
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_STRING,
          .as.string = {NULL, 0},
      };
      toad_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(result, &actual), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual.kind, TOAD_SCRIPT_VALUE_STRING);
      check_equal(actual.as.string.size, (size_t)0);
      check_not_null(actual.as.string.data);
      check_equal(actual.as.string.data, "");
    }

    it("preserves an empty array") {
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_ARRAY,
          .as.array = {NULL, 0},
      };
      toad_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(result, &actual), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual.kind, TOAD_SCRIPT_VALUE_ARRAY);
      check_equal(actual.as.array.count, (size_t)0);
      check_null(actual.as.array.items);
    }

    it("preserves an empty record") {
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {NULL, 0},
      };
      toad_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(result, &actual), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual.kind, TOAD_SCRIPT_VALUE_RECORD);
      check_equal(actual.as.record.count, (size_t)0);
      check_null(actual.as.record.entries);
    }

    it("deep-copies nested arrays records keys and strings") {
      char key[] = "items";
      char text[] = "alpha";
      toad_script_value_view_t items[2] = {
          {.kind = TOAD_SCRIPT_VALUE_STRING, .as.string = {text, 5}},
          {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = 7},
      };
      toad_script_record_entry_view_t entries[1] = {
          {{key, 5}, {.kind = TOAD_SCRIPT_VALUE_ARRAY, .as.array = {items, 2}}},
      };
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {entries, 1},
      };
      toad_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits), TOAD_SCRIPT_STATUS_OK);
      memset(key, 'x', sizeof(key) - 1);
      memset(text, 'y', sizeof(text) - 1);
      items[1].as.integer = 99;

      check_equal(toad_script_result_get_value(result, &actual), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual.as.record.entries[0].key.data, "items");
      check_equal(actual.as.record.entries[0].value.as.array.count, (size_t)2);
      check_equal(actual.as.record.entries[0].value.as.array.items[0].as.string.data, "alpha");
      check_equal(actual.as.record.entries[0].value.as.array.items[1].as.integer, INT64_C(7));
    }
  }

  describe("borrowed value validation") {
    it("rejects non-empty views with null backing pointers") {
      const toad_script_value_view_t inputs[] = {
          {.kind = TOAD_SCRIPT_VALUE_STRING, .as.string = {NULL, 1}},
          {.kind = TOAD_SCRIPT_VALUE_ARRAY, .as.array = {NULL, 1}},
          {.kind = TOAD_SCRIPT_VALUE_RECORD, .as.record = {NULL, 1}},
      };
      ts_host_value_limits_t limits = test_limits();

      for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        check_equal(ts_host_result_store_view(result, &inputs[i], &limits),
                    TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
        check_no_published_value(result);
      }
    }

    it("rejects invalid UTF-8 without publishing a value") {
      const char invalid[] = "\xC0\xAF";
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_STRING,
          .as.string = {invalid, sizeof(invalid) - 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_INVALID_UTF8);
      check_no_published_value(result);
    }

    it("rejects an invalid UTF-8 record key") {
      const char invalid[] = "\xED\xA0\x80";
      toad_script_record_entry_view_t entry = {{invalid, sizeof(invalid) - 1},
                                                {.kind = TOAD_SCRIPT_VALUE_NULL}};
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {&entry, 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_validate_value_view(&input, &limits), TOAD_SCRIPT_STATUS_INVALID_UTF8);
    }

    it("rejects an empty record key") {
      toad_script_record_entry_view_t entry = {{NULL, 0}, {.kind = TOAD_SCRIPT_VALUE_NULL}};
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {&entry, 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
      check_no_published_value(result);
    }

    it("rejects an embedded NUL record key without publishing a value") {
      const char key[] = {'a', '\0', 'b'};
      toad_script_record_entry_view_t entry = {
          {key, sizeof(key)},
          {.kind = TOAD_SCRIPT_VALUE_NULL},
      };
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {&entry, 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
      check_no_published_value(result);
    }

    it("rejects embedded NUL keys with the same C-string prefix") {
      const char first_key[] = {'a', '\0', 'b'};
      const char second_key[] = {'a', '\0', 'c'};
      toad_script_record_entry_view_t entries[2] = {
          {{first_key, sizeof(first_key)}, {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = 1}},
          {{second_key, sizeof(second_key)}, {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = 2}},
      };
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {entries, 2},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
      check_no_published_value(result);
    }

    it("rejects duplicate record keys without publishing a value") {
      toad_script_record_entry_view_t entries[2] = {
          {{"id", 2}, {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = 1}},
          {{"id", 2}, {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = 2}},
      };
      toad_script_value_view_t value = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {entries, 2},
      };
      toad_script_value_view_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &value, &limits),
                  TOAD_SCRIPT_STATUS_DUPLICATE_RECORD_KEY);
      check_equal(toad_script_result_get_value(result, &actual),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
    }

    it("rejects a cyclic input graph") {
      toad_script_value_view_t input = {.kind = TOAD_SCRIPT_VALUE_ARRAY};
      ts_host_value_limits_t limits = test_limits();
      input.as.array.items = &input;
      input.as.array.count = 1;

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
      check_no_published_value(result);
    }

    it("rejects value depth 65") {
      toad_script_value_view_t nodes[65] = {0};
      ts_host_value_limits_t limits = test_limits();
      for (size_t i = 0; i + 1 < sizeof(nodes) / sizeof(nodes[0]); ++i) {
        nodes[i].kind = TOAD_SCRIPT_VALUE_ARRAY;
        nodes[i].as.array.items = &nodes[i + 1];
        nodes[i].as.array.count = 1;
      }
      nodes[64].kind = TOAD_SCRIPT_VALUE_NULL;

      check_equal(ts_host_result_store_view(result, &nodes[0], &limits),
                  TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
    }

    it("rejects node count 65537") {
      toad_script_value_view_t *items =
          (toad_script_value_view_t *)calloc(TEST_MAX_NODES, sizeof(*items));
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_ARRAY,
          .as.array = {items, TEST_MAX_NODES},
      };
      ts_host_value_limits_t limits = test_limits();
      limits.max_bytes = SIZE_MAX;
      check_not_null(items);

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
      free(items);
    }

    it("rejects byte quota plus one") {
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_STRING,
          .as.string = {"12345", 5},
      };
      ts_host_value_limits_t limits = test_limits();
      limits.max_bytes = 4;

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
    }

    it("rejects SIZE_MAX copy arithmetic overflow") {
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_STRING,
          .as.string = {"x", SIZE_MAX},
      };
      ts_host_value_limits_t limits = test_limits();
      limits.max_bytes = SIZE_MAX;

      check_equal(ts_host_result_store_view(result, &input, &limits),
                  TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED);
      check_no_published_value(result);
    }
  }

  describe("engine conversion") {
    it("preserves embedded NUL in an ordinary string value") {
      const char text[] = {'a', '\0', 'b'};
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_STRING,
          .as.string = {text, sizeof(text)},
      };
      toad_value_t actual = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_value_from_view(ctx, &input, &limits, &actual), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual.type, TOAD_VAL_STRING);
      check_equal(actual.data.string.len, sizeof(text));
      check_equal(actual.data.string.data[0], 'a');
      check_equal(actual.data.string.data[1], '\0');
      check_equal(actual.data.string.data[2], 'b');
      toad_value_destroy(&actual);
    }

    it("converts a nested value into the target context allocator") {
      char key[] = "values";
      char text[] = "owned";
      toad_script_value_view_t array_items[3] = {
          {.kind = TOAD_SCRIPT_VALUE_BOOL, .as.boolean = 1},
          {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = 23},
          {.kind = TOAD_SCRIPT_VALUE_STRING, .as.string = {text, 5}},
      };
      toad_script_record_entry_view_t entries[1] = {
          {{key, 6}, {.kind = TOAD_SCRIPT_VALUE_ARRAY, .as.array = {array_items, 3}}},
      };
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_RECORD,
          .as.record = {entries, 1},
      };
      toad_value_t actual = {0};
      toad_value_t values;
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_value_from_view(ctx, &input, &limits, &actual), TOAD_SCRIPT_STATUS_OK);
      memset(key, 'x', sizeof(key) - 1);
      memset(text, 'y', sizeof(text) - 1);
      check_equal(actual.type, TOAD_VAL_MAP);
      values = toad_map_get(&actual, "values");
      check_equal(values.type, TOAD_VAL_LIST);
      check_equal(values.data.list.count, (size_t)3);
      check_equal(values.data.list.items[0].type, TOAD_VAL_BOOL);
      check_equal(values.data.list.items[0].data.boolean, 1);
      check_equal(values.data.list.items[1].type, TOAD_VAL_INTEGER);
      check_equal(values.data.list.items[1].data.integer, INT64_C(23));
      check_equal(values.data.list.items[2].type, TOAD_VAL_STRING);
      check_equal(values.data.list.items[2].data.string.len, (size_t)5);
      check_equal(values.data.list.items[2].data.string.data, "owned");
      toad_value_destroy(&actual);
    }

    it("leaves no output ownership when conversion fails") {
      toad_script_value_view_t input = {
          .kind = TOAD_SCRIPT_VALUE_STRING,
          .as.string = {NULL, 1},
      };
      toad_value_t actual = toad_val_int(99);
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_value_from_view(ctx, &input, &limits, &actual),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(actual.type, TOAD_VAL_NULL);
      check_null(actual.storage);
      check_null(actual.storage_aux);
    }
  }

  describe("result lifecycle") {
    it("reuses one result while keeping value and error mutually exclusive") {
      toad_script_value_view_t value = {
          .kind = TOAD_SCRIPT_VALUE_STRING,
          .as.string = {"first", 5},
      };
      toad_script_error_info_t error = {
          .struct_size = sizeof(error),
          .status = TOAD_SCRIPT_STATUS_RUNTIME_ERROR,
          .error_code = 17,
          .phase = TOAD_SCRIPT_ERROR_PHASE_CALL,
          .line = 3,
          .column = 4,
          .length = 2,
          .cause_code = -7,
          .module_name = {"module", 6},
          .function_name = {"run", 3},
          .message = {"failed", 6},
      };
      toad_script_value_view_t actual_value = {0};
      toad_script_error_info_t actual_error = {0};
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &value, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_error(result, &actual_error),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);

      check_equal(ts_host_result_set_error(result, &error, TEST_MAX_BYTES), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(result, &actual_value),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(toad_script_result_get_error(result, &actual_error), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual_error.status, TOAD_SCRIPT_STATUS_RUNTIME_ERROR);
      check_equal(actual_error.module_name.data, "module");
      check_equal(actual_error.function_name.data, "run");
      check_equal(actual_error.message.data, "failed");

      value.as.string.data = "second";
      value.as.string.size = 6;
      check_equal(ts_host_result_store_view(result, &value, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_error(result, &actual_error),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(toad_script_result_get_value(result, &actual_value), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual_value.as.string.data, "second");

      toad_script_result_reset(result);
      check_equal(toad_script_result_get_value(result, &actual_value),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(toad_script_result_get_error(result, &actual_error),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
    }

    it("deep-copies error views") {
      char module[] = "mod";
      char function[] = "fn";
      char message[] = "boom";
      toad_script_error_info_t input = {
          .struct_size = sizeof(input),
          .status = TOAD_SCRIPT_STATUS_HOST_ERROR,
          .phase = TOAD_SCRIPT_ERROR_PHASE_HOST_CALLBACK,
          .module_name = {module, 3},
          .function_name = {function, 2},
          .message = {message, 4},
      };
      toad_script_error_info_t actual = {0};

      check_equal(ts_host_result_set_error(result, &input, TEST_MAX_BYTES), TOAD_SCRIPT_STATUS_OK);
      memset(module, 'x', sizeof(module) - 1);
      memset(function, 'y', sizeof(function) - 1);
      memset(message, 'z', sizeof(message) - 1);
      check_equal(toad_script_result_get_error(result, &actual), TOAD_SCRIPT_STATUS_OK);
      check_equal(actual.module_name.data, "mod");
      check_equal(actual.function_name.data, "fn");
      check_equal(actual.message.data, "boom");
    }

    it("rejects an unknown error status and leaves the result empty") {
      toad_script_value_view_t prior = {.kind = TOAD_SCRIPT_VALUE_NULL};
      toad_script_error_info_t input = {
          .struct_size = sizeof(input),
          .status = TOAD_SCRIPT_STATUS_INTERRUPTED + 1,
          .phase = TOAD_SCRIPT_ERROR_PHASE_CALL,
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &prior, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_result_set_error(result, &input, TEST_MAX_BYTES),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_empty_result(result);
    }

    it("rejects an invalid error phase and leaves the result empty") {
      toad_script_value_view_t prior = {.kind = TOAD_SCRIPT_VALUE_NULL};
      toad_script_error_info_t input = {
          .struct_size = sizeof(input),
          .status = TOAD_SCRIPT_STATUS_RUNTIME_ERROR,
          .phase = TOAD_SCRIPT_ERROR_PHASE_INTERRUPT + 1,
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &prior, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_result_set_error(result, &input, TEST_MAX_BYTES),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_empty_result(result);
    }

    it("rejects nonzero error reserved fields and leaves the result empty") {
      toad_script_value_view_t prior = {.kind = TOAD_SCRIPT_VALUE_NULL};
      toad_script_error_info_t input = {
          .struct_size = sizeof(input),
          .status = TOAD_SCRIPT_STATUS_RUNTIME_ERROR,
          .phase = TOAD_SCRIPT_ERROR_PHASE_CALL,
          .reserved = {0, 0, 0, 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &prior, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_result_set_error(result, &input, TEST_MAX_BYTES),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_empty_result(result);
    }

    it("rejects invalid UTF-8 in error views and leaves the result empty") {
      const char invalid[] = "\xC0\xAF";
      toad_script_value_view_t prior = {.kind = TOAD_SCRIPT_VALUE_NULL};
      toad_script_error_info_t input = {
          .struct_size = sizeof(input),
          .status = TOAD_SCRIPT_STATUS_RUNTIME_ERROR,
          .phase = TOAD_SCRIPT_ERROR_PHASE_CALL,
          .message = {invalid, sizeof(invalid) - 1},
      };
      ts_host_value_limits_t limits = test_limits();

      check_equal(ts_host_result_store_view(result, &prior, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_result_set_error(result, &input, TEST_MAX_BYTES),
                  TOAD_SCRIPT_STATUS_INVALID_UTF8);
      check_empty_result(result);
    }

    it("detects wrong-context access through the internal boundary helper") {
      toad_script_ctx_t *other = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      check_not_null(other);

      check_equal(ts_host_result_check_context(result, ctx), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_result_check_context(result, other),
                  TOAD_SCRIPT_STATUS_CONTEXT_MISMATCH);
      toad_script_free(other);
    }

    it("rejects result access from a non-owner thread") {
      toad_script_value_view_t value = {.kind = TOAD_SCRIPT_VALUE_NULL};
      ts_host_value_limits_t limits = test_limits();
      sync_thread_t thread = NULL;
      result_thread_probe_t probe = {
          .result = result,
          .ctx = ctx,
      };

      check_equal(ts_host_result_store_view(result, &value, &limits), TOAD_SCRIPT_STATUS_OK);
      check_equal(sync_thread_create(&thread, result_thread_probe_run, &probe), 0);
      check_equal(sync_thread_join(&thread), 0);
      sync_thread_destroy(&thread);
      check_equal(probe.getter_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.context_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.reset_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.destroy_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(toad_script_result_get_value(result, &value), TOAD_SCRIPT_STATUS_OK);
    }

    it("rejects result creation and value conversion outside the context owner thread") {
      sync_thread_t thread = NULL;
      context_thread_probe_t probe = {
          .ctx = ctx,
      };

      check_equal(sync_thread_create(&thread, context_thread_probe_run, &probe), 0);
      check_equal(sync_thread_join(&thread), 0);
      sync_thread_destroy(&thread);
      check_equal(probe.create_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.conversion_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.conversion_type, TOAD_VAL_NULL);
    }
  }
}
