#include "tinytest.h"

#include "../src/host/internal.h"
#include "../src/host/api_internal.h"

#include <stdint.h>
#include <string.h>

enum {
  TEST_INSTANCE_READY = 1,
  TEST_INSTANCE_CALLING_HOST = 3,
  TEST_INSTANCE_FAULTED = 4,
};

static const char k_state_source[] =
    "func ok(){return 7;};\n"
    "func handled(){return try {throw \"caught\";} catch(e){42;};};\n"
    "func runtime_fail(){return runtime_fail();};\n"
    "func host_failure(){return host_fail();};\n"
    "func nested(){return host_nested();};\n"
    "func large(){return \"large\";};\n"
    "export(\"ok\");export(\"handled\");export(\"runtime_fail\");"
    "export(\"host_failure\");export(\"nested\");export(\"large\");";

typedef struct host_state_fixture_s host_state_fixture_t;

typedef struct nested_probe_s {
  toad_script_instance_t *outer;
  toad_script_instance_t *target;
  toad_script_export_handle_t target_handle;
  toad_script_result_t *nested_result;
  toad_script_call_options_t *options;
  toad_script_status_t nested_status;
  ts_host_instance_state_t observed_outer_state;
  size_t callback_depth;
  size_t calls;
} nested_probe_t;

struct host_state_fixture_s {
  toad_script_ctx_t *ctx;
  toad_script_result_t *result;
  toad_script_result_t *nested_result;
  toad_script_module_t *module;
  toad_script_instance_t *first;
  toad_script_instance_t *second;
  toad_script_call_options_t options;
  nested_probe_t nested;
};

typedef struct host_failure_case_s {
  const char *name;
  toad_script_status_t expected_status;
  toad_script_error_phase_t expected_phase;
  ts_host_instance_state_t expected_state;
} host_failure_case_t;

static int string_view_equal(toad_script_string_view_t actual, const char *expected) {
  size_t expected_size = strlen(expected);
  return actual.size == expected_size && memcmp(actual.data, expected, expected_size) == 0;
}

static toad_script_status_t host_fail(void *user_data, const toad_script_value_view_t *args,
                                       size_t arg_count,
                                       toad_script_host_result_builder_t *builder) {
  toad_script_string_view_t message = {"host callback rejected input", 28};
  (void)user_data;
  (void)args;
  (void)arg_count;
  return toad_script_host_result_set_error(builder, -77, message);
}

static toad_script_status_t host_nested(void *user_data, const toad_script_value_view_t *args,
                                         size_t arg_count,
                                         toad_script_host_result_builder_t *builder) {
  nested_probe_t *probe = (nested_probe_t *)user_data;
  toad_script_value_view_t value = {
      .kind = TOAD_SCRIPT_VALUE_INT64,
      .as.integer = 23,
  };
  (void)args;
  (void)arg_count;
  probe->calls++;
  probe->observed_outer_state = probe->outer->state;
  if (probe->callback_depth == 0) {
    probe->callback_depth++;
    probe->nested_status = toad_script_instance_call(probe->target, probe->target_handle, NULL, 0,
                                                      probe->options, probe->nested_result);
    probe->callback_depth--;
  }
  return toad_script_host_result_set_value(builder, &value);
}

static toad_script_export_handle_t
resolve_export(host_state_fixture_t *fixture, toad_script_instance_t *instance, const char *name) {
  toad_script_export_handle_t handle = 0;
  check_equal(toad_script_instance_resolve_export(instance,
                                                   (toad_script_string_view_t){name, strlen(name)},
                                                   fixture->result, &handle),
              TOAD_SCRIPT_STATUS_OK);
  check(handle != 0);
  return handle;
}

static void host_state_fixture_open(host_state_fixture_t *fixture,
                                    toad_script_execution_mode_t mode) {
  toad_script_module_options_t module_options;
  toad_script_instance_options_t instance_options;
  toad_script_host_function_descriptor_t host_fail_descriptor = {
      .struct_size = sizeof(host_fail_descriptor),
      .min_arity = 0,
      .max_arity = 0,
      .name = {"host_fail", 9},
  };
  toad_script_host_function_descriptor_t host_nested_descriptor = {
      .struct_size = sizeof(host_nested_descriptor),
      .min_arity = 0,
      .max_arity = 0,
      .name = {"host_nested", 11},
  };
  memset(fixture, 0, sizeof(*fixture));
  fixture->ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
  check_not_null(fixture->ctx);
  check_equal(toad_script_result_create(fixture->ctx, &fixture->result), TOAD_SCRIPT_STATUS_OK);
  check_equal(toad_script_result_create(fixture->ctx, &fixture->nested_result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(toad_script_context_register_host_function(fixture->ctx, &host_fail_descriptor,
                                                          host_fail, NULL, fixture->result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(toad_script_context_register_host_function(fixture->ctx, &host_nested_descriptor,
                                                          host_nested, &fixture->nested,
                                                          fixture->result),
              TOAD_SCRIPT_STATUS_OK);
  toad_script_module_options_init(&module_options);
  module_options.module_name = (toad_script_string_view_t){"state-test", 10};
  check_equal(toad_script_module_compile(
                  fixture->ctx,
                  (toad_script_string_view_t){k_state_source, sizeof(k_state_source) - 1},
                  &module_options, fixture->result, &fixture->module),
              TOAD_SCRIPT_STATUS_OK);
  toad_script_instance_options_init(&instance_options);
  instance_options.mode = mode;
  check_equal(toad_script_instance_create(fixture->module, &instance_options, fixture->result,
                                           &fixture->first),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(toad_script_instance_create(fixture->module, &instance_options, fixture->result,
                                           &fixture->second),
              TOAD_SCRIPT_STATUS_OK);
  toad_script_call_options_init(&fixture->options);
  fixture->nested.options = &fixture->options;
  fixture->nested.nested_result = fixture->nested_result;
}

static void host_state_fixture_close(host_state_fixture_t *fixture) {
  if (fixture->second)
    check_equal(toad_script_instance_destroy(fixture->second, fixture->result),
                TOAD_SCRIPT_STATUS_OK);
  if (fixture->first)
    check_equal(toad_script_instance_destroy(fixture->first, fixture->result),
                TOAD_SCRIPT_STATUS_OK);
  if (fixture->module)
    check_equal(toad_script_module_destroy(fixture->module, fixture->result),
                TOAD_SCRIPT_STATUS_OK);
  if (fixture->nested_result) toad_script_result_destroy(fixture->nested_result);
  if (fixture->result) toad_script_result_destroy(fixture->result);
  if (fixture->ctx) toad_script_free(fixture->ctx);
}

static toad_script_error_info_t get_error(toad_script_result_t *result) {
  toad_script_error_info_t error = {.struct_size = sizeof(error)};
  check_equal(toad_script_result_get_error(result, &error), TOAD_SCRIPT_STATUS_OK);
  return error;
}

static void check_failure(host_state_fixture_t *fixture, const host_failure_case_t *failure,
                          toad_script_status_t actual_status, const char *function_name,
                          const char *message_part, int32_t cause_code) {
  toad_script_error_info_t error = get_error(fixture->result);
  check_equal(actual_status, failure->expected_status);
  check_equal(fixture->first->state, failure->expected_state);
  check_equal(error.status, failure->expected_status);
  check_equal(error.phase, failure->expected_phase);
  check(error.error_code != TOAD_SCRIPT_ERROR_NONE);
  check(string_view_equal(error.module_name, "state-test"));
  check(string_view_equal(error.function_name, function_name));
  check_greater(error.line, (uint32_t)0);
  check_greater(error.column, (uint32_t)0);
  check_equal(error.length, (uint32_t)1);
  check_equal(error.cause_code, cause_code);
  check_not_null(strstr(error.message.data, message_part));
}

static int interrupt_now(void *user_data) {
  size_t *calls = (size_t *)user_data;
  (*calls)++;
  return 1;
}

static void run_preflight_and_handled_matrix(toad_script_execution_mode_t mode) {
  host_state_fixture_t fixture;
  toad_script_export_handle_t ok;
  toad_script_export_handle_t handled;
  toad_script_value_view_t argument = {.kind = TOAD_SCRIPT_VALUE_NULL};
  toad_script_value_view_t value = {0};
  host_state_fixture_open(&fixture, mode);
  ok = resolve_export(&fixture, fixture.first, "ok");
  handled = resolve_export(&fixture, fixture.first, "handled");
  check_equal(
      toad_script_instance_call(fixture.first, ok, &argument, 1, &fixture.options, fixture.result),
      TOAD_SCRIPT_STATUS_INVALID_ARGUMENT);
  check_equal(fixture.first->state, (ts_host_instance_state_t)TEST_INSTANCE_READY);
  check_equal(
      toad_script_instance_call(fixture.first, handled, NULL, 0, &fixture.options, fixture.result),
      TOAD_SCRIPT_STATUS_OK);
  check_equal(fixture.first->state, (ts_host_instance_state_t)TEST_INSTANCE_READY);
  check_equal(toad_script_result_get_value(fixture.result, &value), TOAD_SCRIPT_STATUS_OK);
  check(value.kind == TOAD_SCRIPT_VALUE_INT64 || value.kind == TOAD_SCRIPT_VALUE_NUMBER);
  host_state_fixture_close(&fixture);
}

static void run_runtime_failure(toad_script_execution_mode_t mode) {
  static const host_failure_case_t failure = {
      "uncaught recursion limit",
      TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED,
      TOAD_SCRIPT_ERROR_PHASE_CALL,
      (ts_host_instance_state_t)TEST_INSTANCE_FAULTED,
  };
  host_state_fixture_t fixture;
  toad_script_export_handle_t runtime_fail;
  toad_script_export_handle_t ok;
  toad_script_status_t status;
  (void)failure.name;
  host_state_fixture_open(&fixture, mode);
  runtime_fail = resolve_export(&fixture, fixture.first, "runtime_fail");
  ok = resolve_export(&fixture, fixture.first, "ok");
  status = toad_script_instance_call(fixture.first, runtime_fail, NULL, 0, &fixture.options,
                                      fixture.result);
  check_failure(&fixture, &failure, status, "runtime_fail", "recursion", 0);
  check_equal(
      toad_script_instance_call(fixture.first, ok, NULL, 0, &fixture.options, fixture.result),
      TOAD_SCRIPT_STATUS_INVALID_STATE);
  check_equal(fixture.first->state, (ts_host_instance_state_t)TEST_INSTANCE_FAULTED);
  host_state_fixture_close(&fixture);
}

static void run_host_failure(toad_script_execution_mode_t mode) {
  static const host_failure_case_t failure = {
      "host callback",
      TOAD_SCRIPT_STATUS_HOST_ERROR,
      TOAD_SCRIPT_ERROR_PHASE_HOST_CALLBACK,
      (ts_host_instance_state_t)TEST_INSTANCE_FAULTED,
  };
  host_state_fixture_t fixture;
  toad_script_export_handle_t handle;
  toad_script_status_t status;
  (void)failure.name;
  host_state_fixture_open(&fixture, mode);
  handle = resolve_export(&fixture, fixture.first, "host_failure");
  status =
      toad_script_instance_call(fixture.first, handle, NULL, 0, &fixture.options, fixture.result);
  check_failure(&fixture, &failure, status, "host_fail", "host callback rejected input", -77);
  host_state_fixture_close(&fixture);
}

static void run_quota_and_interrupt(toad_script_execution_mode_t mode) {
  static const host_failure_case_t quota_failure = {
      "result quota",
      TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED,
      TOAD_SCRIPT_ERROR_PHASE_CALL,
      (ts_host_instance_state_t)TEST_INSTANCE_FAULTED,
  };
  static const host_failure_case_t interrupt_failure = {
      "interrupt",
      TOAD_SCRIPT_STATUS_INTERRUPTED,
      TOAD_SCRIPT_ERROR_PHASE_INTERRUPT,
      (ts_host_instance_state_t)TEST_INSTANCE_FAULTED,
  };
  host_state_fixture_t quota_fixture;
  host_state_fixture_t interrupt_fixture;
  toad_script_export_handle_t large;
  toad_script_export_handle_t ok;
  toad_script_export_handle_t handled;
  toad_script_status_t status;
  size_t interrupt_calls = 0;
  (void)quota_failure.name;
  (void)interrupt_failure.name;

  host_state_fixture_open(&quota_fixture, mode);
  large = resolve_export(&quota_fixture, quota_fixture.first, "large");
  quota_fixture.options.max_result_bytes = 1;
  status = toad_script_instance_call(quota_fixture.first, large, NULL, 0, &quota_fixture.options,
                                      quota_fixture.result);
  check_failure(&quota_fixture, &quota_failure, status, "large", "result", 0);
  host_state_fixture_close(&quota_fixture);

  host_state_fixture_open(&interrupt_fixture, mode);
  ok = resolve_export(&interrupt_fixture, interrupt_fixture.first, "ok");
  handled = resolve_export(&interrupt_fixture, interrupt_fixture.first, "handled");
  check_equal(toad_script_instance_call(interrupt_fixture.first, handled, NULL, 0,
                                         &interrupt_fixture.options, interrupt_fixture.result),
              TOAD_SCRIPT_STATUS_OK);
  interrupt_fixture.options.interrupt = interrupt_now;
  interrupt_fixture.options.interrupt_user_data = &interrupt_calls;
  status = toad_script_instance_call(interrupt_fixture.first, ok, NULL, 0,
                                      &interrupt_fixture.options, interrupt_fixture.result);
  check_equal(interrupt_calls, (size_t)1);
  check_failure(&interrupt_fixture, &interrupt_failure, status, "ok", "interrupted", 0);
  host_state_fixture_close(&interrupt_fixture);
}

static void run_reentrancy_matrix(toad_script_execution_mode_t mode) {
  host_state_fixture_t fixture;
  toad_script_export_handle_t first_nested;
  toad_script_export_handle_t second_nested;
  toad_script_value_view_t value = {0};
  toad_script_error_info_t nested_error;
  host_state_fixture_open(&fixture, mode);
  first_nested = resolve_export(&fixture, fixture.first, "nested");
  second_nested = resolve_export(&fixture, fixture.second, "nested");
  fixture.nested.outer = fixture.first;

  fixture.nested.target = fixture.first;
  fixture.nested.target_handle = first_nested;
  fixture.nested.nested_status = TOAD_SCRIPT_STATUS_OK;
  check_equal(toad_script_instance_call(fixture.first, first_nested, NULL, 0, &fixture.options,
                                         fixture.result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(fixture.nested.nested_status, TOAD_SCRIPT_STATUS_REENTRANT_CALL);
  check_equal(fixture.nested.observed_outer_state,
              (ts_host_instance_state_t)TEST_INSTANCE_CALLING_HOST);
  check_equal(fixture.first->state, (ts_host_instance_state_t)TEST_INSTANCE_READY);
  nested_error = get_error(fixture.nested_result);
  check_equal(nested_error.status, TOAD_SCRIPT_STATUS_REENTRANT_CALL);

  fixture.nested.target = fixture.second;
  fixture.nested.target_handle = second_nested;
  fixture.nested.nested_status = TOAD_SCRIPT_STATUS_OK;
  check_equal(toad_script_instance_call(fixture.first, first_nested, NULL, 0, &fixture.options,
                                         fixture.result),
              TOAD_SCRIPT_STATUS_OK);
  check_equal(fixture.nested.nested_status, TOAD_SCRIPT_STATUS_REENTRANT_CALL);
  check_equal(fixture.first->state, (ts_host_instance_state_t)TEST_INSTANCE_READY);
  check_equal(fixture.second->state, (ts_host_instance_state_t)TEST_INSTANCE_READY);
  check_equal(toad_script_result_get_value(fixture.result, &value), TOAD_SCRIPT_STATUS_OK);
  check_equal(value.kind, TOAD_SCRIPT_VALUE_INT64);
  check_equal(value.as.integer, INT64_C(23));
  nested_error = get_error(fixture.nested_result);
  check_equal(nested_error.status, TOAD_SCRIPT_STATUS_REENTRANT_CALL);
  host_state_fixture_close(&fixture);
}

spec("Toad Host call state and diagnostics") {
  it("keeps the fixed internal state values") {
    check_equal(TS_HOST_INSTANCE_READY, (ts_host_instance_state_t)1);
    check_equal(TS_HOST_INSTANCE_CALLING, (ts_host_instance_state_t)2);
    check_equal(TS_HOST_INSTANCE_CALLING_HOST, (ts_host_instance_state_t)3);
    check_equal(TS_HOST_INSTANCE_FAULTED, (ts_host_instance_state_t)4);
    check_equal(TS_HOST_INSTANCE_DESTROYED, (ts_host_instance_state_t)5);
  }
  it("keeps preflight and handled failures Ready in the interpreter") {
    run_preflight_and_handled_matrix(TOAD_SCRIPT_EXEC_INTERPRETER);
  }
  it("keeps preflight and handled failures Ready in the JIT") {
    run_preflight_and_handled_matrix(TOAD_SCRIPT_EXEC_JIT);
  }
  it("faults on uncaught interpreter recursion-limit failures") {
    run_runtime_failure(TOAD_SCRIPT_EXEC_INTERPRETER);
  }
  it("faults on uncaught JIT recursion-limit failures") {
    run_runtime_failure(TOAD_SCRIPT_EXEC_JIT);
  }
  it("preserves interpreter host callback causes") {
    run_host_failure(TOAD_SCRIPT_EXEC_INTERPRETER);
  }
  it("preserves JIT host callback causes") { run_host_failure(TOAD_SCRIPT_EXEC_JIT); }
  it("faults interpreter instances on result quota and interrupt") {
    run_quota_and_interrupt(TOAD_SCRIPT_EXEC_INTERPRETER);
  }
  it("faults JIT instances on result quota and interrupt") {
    run_quota_and_interrupt(TOAD_SCRIPT_EXEC_JIT);
  }
  it("rejects same and other interpreter instance reentrancy") {
    run_reentrancy_matrix(TOAD_SCRIPT_EXEC_INTERPRETER);
  }
  it("rejects same and other JIT instance reentrancy") {
    run_reentrancy_matrix(TOAD_SCRIPT_EXEC_JIT);
  }
}
