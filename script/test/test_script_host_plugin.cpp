#include "tinytest.hpp"
#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <toad/script.h>

namespace {
  toad_script_string_view_t text(const char *value) { return {value, std::strlen(value)}; }

  struct Host {
    cmeta_plugin_registry registry{};
    cmeta_plugin_ref plugins[3]{};
    bool started[3]{}, stopping[3]{};
    size_t count = 0;
    toad_script_ctx_t *ctx = nullptr;
    toad_script_result_t *result = nullptr;
    toad_script_module_t *module = nullptr;
    toad_script_instance_t *instance = nullptr;
    toad_script_export_handle_t handle = 0;

    Host() {
      cmeta_plugin_registry_config config{};
      config.capacity = 3;
      check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
      ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
      check_not_null(ctx);
      check_equal(toad_script_result_create(ctx, &result), TOAD_SCRIPT_STATUS_OK);
    }
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;
    ~Host() {
      close_module();
      toad_script_result_destroy(result);
      toad_script_free(ctx);
      for (size_t i = count; i > 0; --i) {
        size_t index = i - 1;
        if (started[index]) {
          bool quiet = false;
          if ((!stopping[index] &&
               cmeta_plugin_registry_request_stop(&registry, plugins[index]) != CMETA_PLUGIN_OK) ||
              cmeta_plugin_registry_poll_quiescent(&registry, plugins[index], &quiet) !=
                  CMETA_PLUGIN_OK ||
              !quiet)
            std::abort();
        }
        if (cmeta_plugin_registry_unload(&registry, plugins[index]) != CMETA_PLUGIN_OK)
          std::abort();
      }
      if (cmeta_plugin_registry_destroy(&registry) != CMETA_PLUGIN_OK) std::abort();
    }
    size_t load(const char *path, bool start = true) {
      size_t index = count;
      check_equal(cmeta_plugin_registry_load(&registry, path, &plugins[index]), CMETA_PLUGIN_OK);
      ++count;
      if (start) {
        check_equal(cmeta_plugin_registry_start(&registry, plugins[index]), CMETA_PLUGIN_OK);
        started[index] = true;
      }
      return index;
    }
    toad_script_status_t attach(size_t index) {
      return toad_script_context_register_plugin(ctx, &registry, plugins[index], result);
    }
    toad_script_status_t compile(const char *source) {
      toad_script_module_options_t options;
      toad_script_module_options_init(&options);
      options.flags = TOAD_SCRIPT_MODULE_EXPLICIT_CALLS;
      return toad_script_module_compile(ctx, text(source), &options, result, &module);
    }
    void instantiate(toad_script_execution_mode_t mode) {
      toad_script_instance_options_t options;
      toad_script_instance_options_init(&options);
      options.mode = mode;
      check_equal(toad_script_instance_create(module, &options, result, &instance),
                  TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_instance_resolve_export(instance, text("calculate"), result, &handle),
                  TOAD_SCRIPT_STATUS_OK);
    }
    toad_script_status_t call(const toad_script_value_view_t *args = nullptr, size_t count = 0) {
      toad_script_call_options_t options;
      toad_script_call_options_init(&options);
      return toad_script_instance_call(instance, handle, args, count, &options, result);
    }
    void close_instance() {
      if (instance && toad_script_instance_destroy(instance, result) != TOAD_SCRIPT_STATUS_OK)
        std::abort();
      instance = nullptr;
    }
    void close_module() {
      close_instance();
      if (module && toad_script_module_destroy(module, result) != TOAD_SCRIPT_STATUS_OK)
        std::abort();
      module = nullptr;
    }
    void stop(size_t index) {
      check_equal(cmeta_plugin_registry_request_stop(&registry, plugins[index]), CMETA_PLUGIN_OK);
      stopping[index] = true;
    }
  };

  toad_script_status_t echo(void *, const toad_script_value_view_t *args, size_t count,
                            toad_script_host_result_builder_t *builder) {
    return count == 1 ? toad_script_host_result_set_value(builder, args)
                      : TOAD_SCRIPT_STATUS_INVALID_ARGUMENT;
  }
  void add_echo(Host &host, const char *name) {
    toad_script_host_function_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.name = text(name);
    descriptor.min_arity = descriptor.max_arity = 1;
    check_equal(toad_script_context_register_host_function(host.ctx, &descriptor, echo, nullptr,
                                                           host.result),
                TOAD_SCRIPT_STATUS_OK);
  }
} // namespace

suite("CMeta native modules attached to Host contexts") {
  it("composes independent C and C++ modules in both execution backends") {
    Host host;
    check_equal(host.attach(host.load(CALIBRATION_PATH)), TOAD_SCRIPT_STATUS_OK);
    check_equal(host.attach(host.load(MAINTENANCE_PATH)), TOAD_SCRIPT_STATUS_OK);
    check_equal(
        host.compile(
            "func calculate(x){return "
            "plant_cost(plant_normalize(x,2.0),10.0)+plant_limit();};export(\"calculate\");"),
        TOAD_SCRIPT_STATUS_OK);
    for (auto mode : {TOAD_SCRIPT_EXEC_INTERPRETER, TOAD_SCRIPT_EXEC_JIT}) {
      host.instantiate(mode);
      toad_script_value_view_t input{}, output{};
      input.kind = TOAD_SCRIPT_VALUE_INT64;
      input.as.integer = 8;
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(host.result, &output), TOAD_SCRIPT_STATUS_OK);
      check_equal(output.kind, TOAD_SCRIPT_VALUE_NUMBER);
      check_equal(output.as.number, 132.0);
      host.close_instance();
    }
  }
  it("preserves bool results and rejects mismatched integer arguments") {
    Host host;
    check_equal(host.attach(host.load(CALIBRATION_PATH)), TOAD_SCRIPT_STATUS_OK);
    check_equal(host.compile("func calculate(x){return plant_positive(x);};export(\"calculate\");"),
                TOAD_SCRIPT_STATUS_OK);
    for (auto mode : {TOAD_SCRIPT_EXEC_INTERPRETER, TOAD_SCRIPT_EXEC_JIT}) {
      host.instantiate(mode);
      toad_script_value_view_t input{}, output{};
      input.kind = TOAD_SCRIPT_VALUE_INT64;
      input.as.integer = INT_MAX;
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(host.result, &output), TOAD_SCRIPT_STATUS_OK);
      check_equal(output.kind, TOAD_SCRIPT_VALUE_BOOL);
      check_equal(output.as.boolean, 1);
      input.as.integer = INT64_MAX;
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_HOST_ERROR);
      host.close_instance();
      host.instantiate(mode);
      input.kind = TOAD_SCRIPT_VALUE_NUMBER;
      input.as.number = 1.0;
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_HOST_ERROR);
      host.close_instance();
    }
  }
  it("rejects lossy integer promotion and bool coercion without changing module bindings") {
    Host host;
    check_equal(host.attach(host.load(CALIBRATION_PATH)), TOAD_SCRIPT_STATUS_OK);
    check_equal(host.compile("func calculate(x){return plant_gain(x);};export(\"calculate\");"),
                TOAD_SCRIPT_STATUS_OK);
    for (auto mode : {TOAD_SCRIPT_EXEC_INTERPRETER, TOAD_SCRIPT_EXEC_JIT}) {
      host.instantiate(mode);
      toad_script_value_view_t input{}, output{};
      input.kind = TOAD_SCRIPT_VALUE_INT64;
      input.as.integer = INT64_C(9007199254740993);
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_HOST_ERROR);
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_INVALID_STATE);
      host.close_instance();
      host.instantiate(mode);
      input.kind = TOAD_SCRIPT_VALUE_BOOL;
      input.as.boolean = 1;
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_HOST_ERROR);
      host.close_instance();
      host.instantiate(mode);
      input.kind = TOAD_SCRIPT_VALUE_INT64;
      input.as.integer = INT64_C(9007199254740992);
      check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_OK);
      check_equal(toad_script_result_get_value(host.result, &output), TOAD_SCRIPT_STATUS_OK);
      check_equal(output.as.number, 18014398509481984.0);
      host.close_instance();
    }
  }
  it("rejects a non-finite native result in both execution backends") {
    Host host;
    check_equal(host.attach(host.load(CALIBRATION_PATH)), TOAD_SCRIPT_STATUS_OK);
    check_equal(
        host.compile("func calculate(){return plant_normalize(1.0,0.0);};export(\"calculate\");"),
        TOAD_SCRIPT_STATUS_OK);
    for (auto mode : {TOAD_SCRIPT_EXEC_INTERPRETER, TOAD_SCRIPT_EXEC_JIT}) {
      host.instantiate(mode);
      check_equal(host.call(), TOAD_SCRIPT_STATUS_HOST_ERROR);
      host.close_instance();
    }
  }
  it("preserves bool long float and void contracts through nested script helpers") {
    Host host;
    check_equal(host.attach(host.load(SCALARS_PATH)), TOAD_SCRIPT_STATUS_OK);
    const char *sources[] = {
        "func helper(x){return native_enabled(x);};func calculate(x){return "
        "helper(x);};export(\"calculate\");",
        "func helper(x){return native_counter(x);};func calculate(x){return "
        "helper(x);};export(\"calculate\");",
        "func helper(x){return native_sample(x);};func calculate(x){return "
        "helper(x);};export(\"calculate\");",
        "func calculate(){native_notify();return native_notified();};export(\"calculate\");",
        "func calculate(){return native_notify();};export(\"calculate\");"};
    for (unsigned kind = 0; kind < sizeof(sources) / sizeof(sources[0]); ++kind) {
      check_equal(host.compile(sources[kind]), TOAD_SCRIPT_STATUS_OK);
      for (auto mode : {TOAD_SCRIPT_EXEC_INTERPRETER, TOAD_SCRIPT_EXEC_JIT}) {
        host.instantiate(mode);
        toad_script_value_view_t input{}, output{};
        if (kind == 0) {
          input.kind = TOAD_SCRIPT_VALUE_BOOL;
          input.as.boolean = 1;
        }
        if (kind == 1) {
          input.kind = TOAD_SCRIPT_VALUE_INT64;
          input.as.integer = LONG_MAX;
        }
        if (kind == 2) {
          input.kind = TOAD_SCRIPT_VALUE_NUMBER;
          input.as.number = 1.25;
        }
        check_equal(host.call(kind >= 3 ? nullptr : &input, kind >= 3 ? 0 : 1),
                    TOAD_SCRIPT_STATUS_OK);
        check_equal(toad_script_result_get_value(host.result, &output), TOAD_SCRIPT_STATUS_OK);
        check_equal(output.kind, kind == 3 ? TOAD_SCRIPT_VALUE_INT64 : input.kind);
        if (kind == 0) check_equal(output.as.boolean, 1);
        if (kind == 1) check_equal(output.as.integer, LONG_MAX);
        if (kind == 3) check_equal(output.as.integer, 1);
        if (kind == 2) {
          check_equal(output.as.number, 1.25);
          input.as.number = static_cast<double>(FLT_MAX) * 2.0;
          check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_HOST_ERROR);
        }
        host.close_instance();
      }
      host.close_module();
    }
  }
  it("rejects a later export collision atomically and allows a corrected attachment") {
    Host host;
    size_t plugin = host.load(CALIBRATION_PATH);
    add_echo(host, "plant_gain");
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_INVALID_STATE);
    check_equal(
        host.compile("func calculate(x){return plant_normalize(x,2.0);};export(\"calculate\");"),
        TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
    check_equal(host.compile("func calculate(x){return plant_gain(x);};export(\"calculate\");"),
                TOAD_SCRIPT_STATUS_OK);
    host.instantiate(TOAD_SCRIPT_EXEC_INTERPRETER);
    toad_script_value_view_t input{}, output{};
    input.kind = TOAD_SCRIPT_VALUE_INT64;
    input.as.integer = 9;
    check_equal(host.call(&input, 1), TOAD_SCRIPT_STATUS_OK);
    check_equal(toad_script_result_get_value(host.result, &output), TOAD_SCRIPT_STATUS_OK);
    check_equal(output.kind, TOAD_SCRIPT_VALUE_NUMBER);
    check_equal(output.as.number, 9.0);
    host.close_module();
    check_equal(
        toad_script_context_unregister_host_function(host.ctx, text("plant_gain"), host.result),
        TOAD_SCRIPT_STATUS_OK);
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_OK);
  }
  it("requires STARTED state and the owner thread and freezes registration during compiled module "
     "lifetime") {
    Host host;
    size_t plugin = host.load(CALIBRATION_PATH, false);
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_INVALID_STATE);
    check_equal(cmeta_plugin_registry_start(&host.registry, host.plugins[plugin]), CMETA_PLUGIN_OK);
    host.started[plugin] = true;
    toad_script_status_t other = TOAD_SCRIPT_STATUS_OK;
    std::thread worker([&] { other = host.attach(plugin); });
    worker.join();
    check_equal(other, TOAD_SCRIPT_STATUS_WRONG_THREAD);
    check_equal(host.compile("func calculate(x){return x;};export(\"calculate\");"),
                TOAD_SCRIPT_STATUS_OK);
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_INVALID_STATE);
    host.close_module();
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_OK);
  }
  it("rejects interface and unknown result manifests without publishing the earlier valid export") {
    Host host;
    check_equal(host.attach(host.load(INTERFACE_PATH)), TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
    check_equal(host.attach(host.load(UNKNOWN_PATH)), TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
    check_equal(
        host.compile("func calculate(){return native_admissible();};export(\"calculate\");"),
        TOAD_SCRIPT_STATUS_VALIDATION_ERROR);
  }
  it("preserves a full callback table and recovers after the owner frees capacity") {
    Host host;
    size_t plugin = host.load(CALIBRATION_PATH);
    for (unsigned i = 0; i < 256; ++i)
      add_echo(host, ("occupied_" + std::to_string(i)).c_str());
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_LIMIT_EXCEEDED);
    for (unsigned i = 0; i < 256; ++i) {
      auto name = "occupied_" + std::to_string(i);
      check_equal(
          toad_script_context_unregister_host_function(host.ctx, text(name.c_str()), host.result),
          TOAD_SCRIPT_STATUS_OK);
    }
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_OK);
  }
  it("holds its lease through stop and compiled module retention until final context cleanup") {
    Host host;
    size_t plugin = host.load(MAINTENANCE_PATH);
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_OK);
    check_equal(host.compile("func calculate(){return plant_limit();};export(\"calculate\");"),
                TOAD_SCRIPT_STATUS_OK);
    host.instantiate(TOAD_SCRIPT_EXEC_JIT);
    host.stop(plugin);
    bool quiet = true;
    check_equal(cmeta_plugin_registry_poll_quiescent(&host.registry, host.plugins[plugin], &quiet),
                CMETA_PLUGIN_OK);
    check_false(quiet);
    check_equal(cmeta_plugin_registry_unload(&host.registry, host.plugins[plugin]),
                CMETA_PLUGIN_BUSY);
    check_equal(host.call(), TOAD_SCRIPT_STATUS_OK);
    toad_script_value_view_t output{};
    check_equal(toad_script_result_get_value(host.result, &output), TOAD_SCRIPT_STATUS_OK);
    check_equal(output.kind, TOAD_SCRIPT_VALUE_INT64);
    check_equal(output.as.integer, 42);
    host.close_module();
    check_equal(cmeta_plugin_registry_unload(&host.registry, host.plugins[plugin]),
                CMETA_PLUGIN_BUSY);
    toad_script_result_destroy(host.result);
    host.result = nullptr;
    toad_script_free(host.ctx);
    host.ctx = nullptr;
    quiet = false;
    check_equal(cmeta_plugin_registry_poll_quiescent(&host.registry, host.plugins[plugin], &quiet),
                CMETA_PLUGIN_OK);
    check_true(quiet);
  }
  it("retains the lease after an early context close until the final instance releases its "
     "module") {
    Host host;
    size_t plugin = host.load(MAINTENANCE_PATH);
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_OK);
    check_equal(host.compile("func calculate(){return plant_limit();};export(\"calculate\");"),
                TOAD_SCRIPT_STATUS_OK);
    host.instantiate(TOAD_SCRIPT_EXEC_INTERPRETER);
    host.stop(plugin);
    toad_script_free(host.ctx);
    check_equal(host.attach(plugin), TOAD_SCRIPT_STATUS_INVALID_STATE);
    host.ctx = nullptr;
    check_equal(toad_script_module_destroy(host.module, host.result), TOAD_SCRIPT_STATUS_OK);
    host.module = nullptr;
    check_equal(cmeta_plugin_registry_unload(&host.registry, host.plugins[plugin]),
                CMETA_PLUGIN_BUSY);
    check_equal(host.call(), TOAD_SCRIPT_STATUS_INVALID_STATE);
    host.close_instance();
    /* Result storage also retains the context. */
    check_equal(cmeta_plugin_registry_unload(&host.registry, host.plugins[plugin]),
                CMETA_PLUGIN_BUSY);
    toad_script_result_destroy(host.result);
    host.result = nullptr;
    bool quiet = false;
    check_equal(cmeta_plugin_registry_poll_quiescent(&host.registry, host.plugins[plugin], &quiet),
                CMETA_PLUGIN_OK);
    check_true(quiet);
  }
}
