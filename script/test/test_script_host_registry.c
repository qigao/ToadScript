#include "tinytest.h"

#include <toad/engine.h>
#include "threads/threads.h"
#include "../src/host/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

toad_script_status_t ts_host_check_owner_thread(const toad_script_ctx_t *ctx);
toad_script_status_t ts_host_registry_acquire_module(toad_script_ctx_t *ctx);
toad_script_status_t ts_host_registry_release_module(toad_script_ctx_t *ctx);
toad_script_status_t ts_host_registry_find_slot(const toad_script_ctx_t *ctx,
                                                 toad_script_string_view_t name,
                                                 size_t *out_slot);
toad_script_status_t ts_host_registry_get_slot(
    const toad_script_ctx_t *ctx, size_t slot,
    const ts_host_function_entry_t **out_entry);
toad_script_status_t ts_host_registry_bind_runtime(toad_script_ctx_t *ctx,
                                                    toad_env_t *runtime_ctx);

enum { TEST_HOST_REGISTRY_CAPACITY = 256 };

typedef struct registry_thread_probe_s {
  toad_script_ctx_t *ctx;
  toad_script_result_t *result;
  toad_script_host_function_descriptor_t descriptor;
  toad_script_status_t owner_status;
  toad_script_status_t register_status;
  toad_script_status_t unregister_status;
  toad_script_status_t bind_status;
  toad_script_status_t acquire_status;
  toad_script_status_t release_status;
  toad_script_status_t find_status;
  toad_script_status_t find_null_status;
  toad_script_status_t get_status;
  toad_script_status_t get_null_status;
  size_t slot_output;
  const ts_host_function_entry_t *entry_output;
} registry_thread_probe_t;

typedef struct callback_probe_s {
  size_t calls;
  toad_script_status_t second_write_status;
  toad_script_status_t first_write_status;
  int saw_nested_argument;
  const toad_script_value_view_t *borrowed_argument;
} callback_probe_t;

static toad_script_host_function_descriptor_t descriptor_for(const char *name) {
  toad_script_host_function_descriptor_t descriptor = {
      .struct_size = sizeof(descriptor),
      .min_arity = 1,
      .max_arity = 1,
      .name = {name, strlen(name)},
  };
  return descriptor;
}

static toad_value_t toad_native_noop(size_t arg_count, toad_value_t *args,
                                         toad_env_t *env, void *user_data) {
  (void)arg_count;
  (void)args;
  (void)env;
  (void)user_data;
  return toad_val_num(0.0);
}

static toad_script_status_t host_echo(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  (void)user_data;
  if (arg_count != 1) return TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  return toad_script_host_result_set_value(builder, &args[0]);
}

static toad_script_status_t host_empty_success(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  (void)args;
  (void)arg_count;
  (void)builder;
  probe->calls++;
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t host_write_twice(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  toad_script_value_view_t value = {
      .kind = TOAD_SCRIPT_VALUE_INT64,
      .as.integer = 23,
  };
  toad_script_string_view_t message = {"ignored", 7};
  (void)args;
  (void)arg_count;
  probe->calls++;
  if (toad_script_host_result_set_value(builder, &value) != TOAD_SCRIPT_STATUS_OK)
    return TOAD_SCRIPT_STATUS_HOST_ERROR;
  probe->second_write_status = toad_script_host_result_set_error(builder, -9, message);
  return TOAD_SCRIPT_STATUS_OK;
}

static toad_script_status_t host_error(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  toad_script_string_view_t message = {"host callback rejected input", 28};
  (void)args;
  (void)arg_count;
  probe->calls++;
  return toad_script_host_result_set_error(builder, -77, message);
}

static toad_script_status_t host_nested_echo(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  const toad_script_value_view_t *map_value;
  const toad_script_value_view_t *object_value;
  probe->calls++;
  if (arg_count != 1 || args[0].kind != TOAD_SCRIPT_VALUE_ARRAY ||
      args[0].as.array.count != 2)
    return TOAD_SCRIPT_STATUS_VALIDATION_ERROR;
  map_value = &args[0].as.array.items[0];
  object_value = &args[0].as.array.items[1];
  probe->borrowed_argument = &args[0];
  probe->saw_nested_argument =
      map_value->kind == TOAD_SCRIPT_VALUE_RECORD &&
      map_value->as.record.count == 1 &&
      object_value->kind == TOAD_SCRIPT_VALUE_RECORD &&
      object_value->as.record.count == 1 &&
      map_value->as.record.entries[0].value.kind == TOAD_SCRIPT_VALUE_ARRAY &&
      map_value->as.record.entries[0].value.as.array.count == 2 &&
      map_value->as.record.entries[0].value.as.array.items[1].kind ==
          TOAD_SCRIPT_VALUE_RECORD;
  return toad_script_host_result_set_value(builder, &args[0]);
}

static toad_script_status_t host_invalid_value_then_write_again(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  const char invalid_utf8[] = "\xC0\xAF";
  toad_script_value_view_t invalid = {
      .kind = TOAD_SCRIPT_VALUE_STRING,
      .as.string = {invalid_utf8, sizeof(invalid_utf8) - 1},
  };
  toad_script_string_view_t message = {"second", 6};
  (void)args;
  (void)arg_count;
  probe->first_write_status = toad_script_host_result_set_value(builder, &invalid);
  probe->second_write_status = toad_script_host_result_set_error(builder, -1, message);
  return probe->first_write_status;
}

static toad_script_status_t host_invalid_error_then_write_again(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  const char invalid_utf8[] = "\xC0\xAF";
  toad_script_string_view_t message = {invalid_utf8, sizeof(invalid_utf8) - 1};
  toad_script_value_view_t value = {.kind = TOAD_SCRIPT_VALUE_NULL};
  (void)args;
  (void)arg_count;
  probe->first_write_status = toad_script_host_result_set_error(builder, -1, message);
  probe->second_write_status = toad_script_host_result_set_value(builder, &value);
  return probe->first_write_status;
}

static toad_script_status_t host_composite_value_then_fail(
    void *user_data, const toad_script_value_view_t *args, size_t arg_count,
    toad_script_host_result_builder_t *builder) {
  callback_probe_t *probe = (callback_probe_t *)user_data;
  toad_script_value_view_t items[2] = {
      {.kind = TOAD_SCRIPT_VALUE_INT64, .as.integer = 7},
      {.kind = TOAD_SCRIPT_VALUE_STRING, .as.string = {"owned", 5}},
  };
  toad_script_record_entry_view_t entries[1] = {
      {{"items", 5}, {.kind = TOAD_SCRIPT_VALUE_ARRAY, .as.array = {items, 2}}},
  };
  toad_script_value_view_t value = {
      .kind = TOAD_SCRIPT_VALUE_RECORD,
      .as.record = {entries, 1},
  };
  (void)args;
  (void)arg_count;
  probe->calls++;
  probe->first_write_status = toad_script_host_result_set_value(builder, &value);
  return TOAD_SCRIPT_STATUS_HOST_ERROR;
}

static void registry_thread_probe_run(void *arg) {
  registry_thread_probe_t *probe = (registry_thread_probe_t *)arg;
  toad_env_t runtime;
  toad_env_init(&runtime);
  probe->owner_status = ts_host_check_owner_thread(probe->ctx);
  probe->register_status = toad_script_context_register_host_function(
      probe->ctx, &probe->descriptor, host_echo, NULL, probe->result);
  probe->unregister_status = toad_script_context_unregister_host_function(
      probe->ctx, probe->descriptor.name, probe->result);
  probe->bind_status = ts_host_registry_bind_runtime(probe->ctx, &runtime);
  probe->acquire_status = ts_host_registry_acquire_module(probe->ctx);
  probe->release_status = ts_host_registry_release_module(probe->ctx);
  probe->find_status = ts_host_registry_find_slot(
      probe->ctx, probe->descriptor.name, &probe->slot_output);
  probe->find_null_status = ts_host_registry_find_slot(
      probe->ctx, probe->descriptor.name, NULL);
  probe->get_status = ts_host_registry_get_slot(
      probe->ctx, 0, &probe->entry_output);
  probe->get_null_status = ts_host_registry_get_slot(probe->ctx, 0, NULL);
  toad_env_free(&runtime);
}

static void bind_one(toad_script_ctx_t *ctx, toad_script_result_t *result,
                     const char *name, toad_script_host_function_t callback,
                     void *user_data, toad_env_t *runtime) {
  toad_script_host_function_descriptor_t descriptor = descriptor_for(name);
  check_equal(toad_script_context_register_host_function(
                  ctx, &descriptor, callback, user_data, result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(ts_host_registry_acquire_module(ctx), TOAD_SCRIPT_STATUS_OK);
  toad_env_init(runtime);
  check_equal(ts_host_registry_bind_runtime(ctx, runtime), TOAD_SCRIPT_STATUS_OK);
}

static void unbind_one(toad_script_ctx_t *ctx, toad_env_t *runtime) {
  toad_env_free(runtime);
  check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_OK);
}

static void check_bind_transaction_failure_at(toad_script_ctx_t *ctx,
                                              toad_script_result_t *result,
                                              size_t fail_at) {
  toad_script_host_function_descriptor_t first = descriptor_for("batch.first");
  toad_script_host_function_descriptor_t middle = descriptor_for("batch.middle");
  toad_script_host_function_descriptor_t last = descriptor_for("batch.last");
  toad_env_t runtime;
  toad_func_t *before;

  check_equal(toad_script_context_register_host_function(
                  ctx, &first, host_echo, NULL, result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(toad_script_context_register_host_function(
                  ctx, &middle, host_echo, NULL, result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(toad_script_context_register_host_function(
                  ctx, &last, host_echo, NULL, result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(ts_host_registry_acquire_module(ctx), TOAD_SCRIPT_STATUS_OK);
  toad_env_init(&runtime);
  toad_env_register_func(&runtime, "sentinel", toad_native_noop, NULL);
  before = runtime.funcs;

  check_equal(ts_host_registry_bind_runtime_test_fault(ctx, &runtime, fail_at),
              TOAD_SCRIPT_STATUS_OUT_OF_MEMORY);
  check(runtime.funcs == before);
  check_equal(toad_env_has_func(&runtime, "sentinel"), 1);
  check_equal(toad_env_has_func(&runtime, "batch.first"), 0);
  check_equal(toad_env_has_func(&runtime, "batch.middle"), 0);
  check_equal(toad_env_has_func(&runtime, "batch.last"), 0);
  toad_env_free(&runtime);
  check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_OK);
}

spec("toad_script_host_registry") {
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

  describe("descriptor validation") {
    it("requires descriptor struct_size to exactly match this ABI") {
      toad_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      descriptor.struct_size = sizeof(descriptor) - 1;

      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);

      descriptor.struct_size = sizeof(descriptor) + 1;
      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    }

    it("rejects empty invalid UTF-8 and embedded-NUL names") {
      const char invalid[] = "\xC0\xAF";
      const char embedded_nul[] = {'e', '\0', 'x'};
      toad_script_host_function_descriptor_t descriptor = descriptor_for("echo");

      descriptor.name.data = NULL;
      descriptor.name.size = 0;
      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_VALIDATION_ERROR);

      descriptor.name.data = invalid;
      descriptor.name.size = sizeof(invalid) - 1;
      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_UTF8);

      descriptor.name.data = embedded_nul;
      descriptor.name.size = sizeof(embedded_nul);
      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
    }

    it("rejects reversed arity bounds") {
      toad_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      descriptor.min_arity = 2;
      descriptor.max_arity = 1;

      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    }

    it("rejects nonzero reserved descriptor fields") {
      toad_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      descriptor.reserved[3] = 1;

      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
    }
  }

  describe("mutable registry") {
    it("owns the registered name bytes") {
      char name[] = "owned_echo";
      toad_script_host_function_descriptor_t descriptor = descriptor_for(name);
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      toad_value_t actual;

      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_OK);
      memset(name, 'x', sizeof(name) - 1);
      check_equal(ts_host_registry_acquire_module(ctx), TOAD_SCRIPT_STATUS_OK);
      toad_env_init(&runtime);
      check_equal(ts_host_registry_bind_runtime(ctx, &runtime), TOAD_SCRIPT_STATUS_OK);
      actual = toad_call_internal("owned_echo", 1, &argument, &runtime);
      check_equal(actual.type, TOAD_VAL_BOOL);
      check_equal(actual.data.boolean, 1);
      toad_value_destroy(&actual);
      toad_env_free(&runtime);
      check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_OK);
    }

    it("rejects a duplicate name and reports missing unregister") {
      toad_script_host_function_descriptor_t descriptor = descriptor_for("echo");
      toad_script_string_view_t missing = {"missing", 7};

      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(toad_script_context_unregister_host_function(ctx, missing, result),
                  TOAD_SCRIPT_STATUS_NOT_FOUND);
    }

    it("publishes a structured error and clears it on the next success") {
      toad_script_host_function_descriptor_t invalid = descriptor_for("bad");
      toad_script_host_function_descriptor_t valid = descriptor_for("good");
      toad_script_error_info_t error = {0};
      invalid.min_arity = 2;
      invalid.max_arity = 1;

      check_equal(toad_script_context_register_host_function(
                      ctx, &invalid, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(toad_script_result_get_error(result, &error), TOAD_SCRIPT_STATUS_OK);
      check_equal(error.status, TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
      check_equal(error.phase, TOAD_SCRIPT_ERROR_PHASE_NONE);
      check_greater(error.message.size, (size_t)0);

      check_equal(toad_script_context_register_host_function(
                      ctx, &valid, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_error(result, &error),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
    }

    it("caps the registry at 256 slots") {
      char name[32];
      for (size_t i = 0; i < TEST_HOST_REGISTRY_CAPACITY; ++i) {
        toad_script_host_function_descriptor_t descriptor;
        check_greater(snprintf(name, sizeof(name), "host_%03zu", i), 0);
        descriptor = descriptor_for(name);
        check_equal(toad_script_context_register_host_function(
                        ctx, &descriptor, host_echo, NULL, result),
                    TOAD_SCRIPT_STATUS_OK);
      }
      {
        toad_script_host_function_descriptor_t descriptor = descriptor_for("overflow");
        check_equal(toad_script_context_register_host_function(
                        ctx, &descriptor, host_echo, NULL, result),
                    TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED);
      }
    }

    it("rejects registry operations from a non-owner Salts thread") {
      sync_thread_t thread = NULL;
      registry_thread_probe_t probe = {
          .ctx = ctx,
          .result = result,
          .descriptor = descriptor_for("echo"),
          .slot_output = 77,
          .entry_output = (const ts_host_function_entry_t *)(uintptr_t)1,
      };

      check_equal(sync_thread_create(&thread, registry_thread_probe_run, &probe), 0);
      check_equal(sync_thread_join(&thread), 0);
      sync_thread_destroy(&thread);
      check_equal(probe.owner_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.register_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.unregister_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.bind_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.acquire_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.release_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.find_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.find_null_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.get_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.get_null_status, TOAD_SCRIPT_STATUS_WRONG_THREAD);
      check_equal(probe.slot_output, (size_t)77);
      check(probe.entry_output ==
            (const ts_host_function_entry_t *)(uintptr_t)1);
    }
  }

  describe("freeze boundary") {
    it("rejects mutation while any host module is active") {
      toad_script_host_function_descriptor_t first = descriptor_for("first");
      toad_script_host_function_descriptor_t second = descriptor_for("second");

      check_equal(toad_script_context_register_host_function(
                      ctx, &first, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_acquire_module(ctx), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_context_register_host_function(
                      ctx, &second, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(toad_script_context_unregister_host_function(ctx, first.name, result),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_OK);
    }

    it("unfreezes only after the last module releases the registry") {
      toad_script_host_function_descriptor_t descriptor = descriptor_for("echo");

      check_equal(toad_script_context_register_host_function(
                      ctx, &descriptor, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_acquire_module(ctx), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_acquire_module(ctx), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_context_unregister_host_function(ctx, descriptor.name, result),
                  TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_context_unregister_host_function(ctx, descriptor.name, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_INVALID_STATE);
    }

    it("keeps resolved slot indexes stable for the frozen lifetime") {
      toad_script_host_function_descriptor_t first = descriptor_for("first");
      toad_script_host_function_descriptor_t second = descriptor_for("second");
      size_t before_freeze = SIZE_MAX;
      size_t while_frozen = SIZE_MAX;

      check_equal(toad_script_context_register_host_function(
                      ctx, &first, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_context_register_host_function(
                      ctx, &second, host_echo, NULL, result),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_find_slot(ctx, second.name, &before_freeze),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(before_freeze, (size_t)1);
      check_equal(ts_host_registry_acquire_module(ctx), TOAD_SCRIPT_STATUS_OK);
      check_equal(ts_host_registry_find_slot(ctx, second.name, &while_frozen),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(while_frozen, before_freeze);
      check_equal(ts_host_registry_release_module(ctx), TOAD_SCRIPT_STATUS_OK);
    }
  }

  describe("callback adapter and terminal builder") {
    it("round-trips nested list map and object values after borrowed args expire") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t nested_object = toad_val_object();
      toad_value_t nested_list = toad_val_list_empty();
      toad_value_t map_value = toad_val_map();
      toad_value_t object_value = toad_val_object();
      toad_value_t argument = toad_val_list_empty();
      toad_value_t actual;
      toad_value_t actual_map;
      toad_value_t actual_nested;
      toad_value_t actual_nested_object;
      toad_value_t actual_object;

      check_equal(toad_map_set(&nested_object, "leaf",
                                 toad_val_str(vstr_from_cstr("value"))),
                  0);
      check_equal(toad_list_push(&nested_list, toad_val_int(7)), 0);
      check_equal(toad_list_push(&nested_list, nested_object), 0);
      check_equal(toad_map_set(&map_value, "nested", nested_list), 0);
      check_equal(toad_map_set(&object_value, "flag", toad_val_bool(1)), 0);
      check_equal(toad_list_push(&argument, map_value), 0);
      check_equal(toad_list_push(&argument, object_value), 0);
      bind_one(ctx, result, "nested", host_nested_echo, &probe, &runtime);

      actual = toad_call_internal("nested", 1, &argument, &runtime);
      toad_value_destroy(&argument);
      toad_value_destroy(&object_value);
      toad_value_destroy(&map_value);
      toad_value_destroy(&nested_list);
      toad_value_destroy(&nested_object);

      check_equal(probe.calls, (size_t)1);
      check_equal(probe.saw_nested_argument, 1);
      check_not_null(probe.borrowed_argument);
      check_equal(runtime.flow, toad_FLOW_NORMAL);
      check_equal(actual.type, TOAD_VAL_LIST);
      check_equal(actual.data.list.count, (size_t)2);
      actual_map = toad_list_get(&actual, 0);
      actual_object = toad_list_get(&actual, 1);
      check_equal(actual_map.type, TOAD_VAL_MAP);
      check_equal(actual_object.type, TOAD_VAL_MAP);
      actual_nested = toad_map_get(&actual_map, "nested");
      check_equal(actual_nested.type, TOAD_VAL_LIST);
      check_equal(actual_nested.data.list.count, (size_t)2);
      actual_nested_object = toad_list_get(&actual_nested, 1);
      check_equal(actual_nested_object.type, TOAD_VAL_MAP);
      {
        toad_value_t leaf = toad_map_get(&actual_nested_object, "leaf");
        toad_value_t flag = toad_map_get(&actual_object, "flag");
        check_equal(leaf.type, TOAD_VAL_STRING);
        check_equal(leaf.data.string.len, (size_t)5);
        check_equal(leaf.data.string.data, "value");
        check_equal(flag.type, TOAD_VAL_BOOL);
        check_equal(flag.data.boolean, 1);
      }
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("passes borrowed arguments and returns one host value") {
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      toad_value_t actual;
      bind_one(ctx, result, "echo", host_echo, NULL, &runtime);

      actual = toad_call_internal("echo", 1, &argument, &runtime);
      check_equal(actual.type, TOAD_VAL_BOOL);
      check_equal(actual.data.boolean, 1);
      check_equal(runtime.flow, toad_FLOW_NORMAL);
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("turns callback success with an empty builder into a host error") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      toad_value_t actual;
      bind_one(ctx, result, "empty", host_empty_success, &probe, &runtime);

      actual = toad_call_internal("empty", 1, &argument, &runtime);
      check_equal(probe.calls, (size_t)1);
      check_equal(runtime.flow, toad_FLOW_THROW);
      check_not_null(strstr(runtime.error_msg, "HOST_ERROR"));
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("rejects a second builder write and keeps the first value") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      toad_value_t actual;
      bind_one(ctx, result, "twice", host_write_twice, &probe, &runtime);

      actual = toad_call_internal("twice", 1, &argument, &runtime);
      check_equal(probe.calls, (size_t)1);
      check_equal(probe.second_write_status, TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(actual.type, TOAD_VAL_INTEGER);
      check_equal(actual.data.integer, INT64_C(23));
      check_equal(runtime.flow, toad_FLOW_NORMAL);
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("keeps builder terminal after an invalid UTF-8 value write") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      toad_value_t actual;
      bind_one(ctx, result, "invalid_value", host_invalid_value_then_write_again,
               &probe, &runtime);

      actual = toad_call_internal("invalid_value", 1, &argument, &runtime);
      check_equal(probe.first_write_status, TOAD_SCRIPT_STATUS_INVALID_UTF8);
      check_equal(probe.second_write_status, TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(runtime.flow, toad_FLOW_THROW);
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("keeps builder terminal after an invalid UTF-8 error write") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      toad_value_t actual;
      bind_one(ctx, result, "invalid_error", host_invalid_error_then_write_again,
               &probe, &runtime);

      actual = toad_call_internal("invalid_error", 1, &argument, &runtime);
      check_equal(probe.first_write_status, TOAD_SCRIPT_STATUS_INVALID_UTF8);
      check_equal(probe.second_write_status, TOAD_SCRIPT_STATUS_INVALID_STATE);
      check_equal(runtime.flow, toad_FLOW_THROW);
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("cleans a composite builder value once when the callback returns an error") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      bind_one(ctx, result, "composite_error", host_composite_value_then_fail,
               &probe, &runtime);

      for (size_t i = 0; i < 32; ++i) {
        toad_value_t actual =
            toad_call_internal("composite_error", 1, &argument, &runtime);
        check_equal(probe.first_write_status, TOAD_SCRIPT_STATUS_OK);
        check_equal(runtime.flow, toad_FLOW_THROW);
        check_equal(actual.type, TOAD_VAL_NULL);
        toad_value_destroy(&actual);
        runtime.flow = toad_FLOW_NORMAL;
      }
      check_equal(probe.calls, (size_t)32);
      unbind_one(ctx, &runtime);
    }

    it("maps a builder error into engine throw state") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t argument = toad_val_bool(1);
      toad_value_t actual;
      bind_one(ctx, result, "fail", host_error, &probe, &runtime);

      actual = toad_call_internal("fail", 1, &argument, &runtime);
      check_equal(probe.calls, (size_t)1);
      check_equal(runtime.flow, toad_FLOW_THROW);
      check_not_null(strstr(runtime.error_msg, "host callback rejected input"));
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }

    it("enforces the registered arity before invoking the callback") {
      callback_probe_t probe = {0};
      toad_env_t runtime;
      toad_value_t actual;
      bind_one(ctx, result, "arity", host_empty_success, &probe, &runtime);

      actual = toad_call_internal("arity", 0, NULL, &runtime);
      check_equal(probe.calls, (size_t)0);
      check_equal(runtime.flow, toad_FLOW_THROW);
      check_not_null(strstr(runtime.error_msg, "arity"));
      toad_value_destroy(&actual);
      unbind_one(ctx, &runtime);
    }
  }

  describe("transactional runtime binding") {
    it("leaves runtime unchanged when the first allocation fails") {
      check_bind_transaction_failure_at(ctx, result, 0);
    }

    it("leaves runtime unchanged when a middle allocation fails") {
      check_bind_transaction_failure_at(ctx, result, 3);
    }

    it("leaves runtime unchanged when the last allocation fails") {
      check_bind_transaction_failure_at(ctx, result, 5);
    }
  }
}
