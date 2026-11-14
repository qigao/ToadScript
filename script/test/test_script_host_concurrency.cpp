#include "tinytest.hpp"
#include <atomic>
#include <cstring>
#include <thread>
#include <toad/script.h>
#include <vector>

static int run_context(unsigned iteration) {
  const char source[] = "func echo(x){return x;};export(\"echo\");";
  auto *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
  if (!ctx) return 1;
  toad_script_result_t *result = nullptr;
  toad_script_module_t *module = nullptr;
  toad_script_instance_t *instance = nullptr;
  toad_script_export_handle_t handle = 0;
  toad_script_module_options_t options;
  toad_script_instance_options_t instance_options;
  toad_script_call_options_t call_options;
  toad_script_module_options_init(&options);
  options.flags = TOAD_SCRIPT_MODULE_EXPLICIT_CALLS;
  toad_script_instance_options_init(&instance_options);
  instance_options.mode = iteration % 2 ? TOAD_SCRIPT_EXEC_JIT : TOAD_SCRIPT_EXEC_INTERPRETER;
  toad_script_call_options_init(&call_options);
  int status = toad_script_result_create(ctx, &result);
  if (!status)
    status =
        toad_script_module_compile(ctx, {source, sizeof(source) - 1}, &options, result, &module);
  if (!status) status = toad_script_instance_create(module, &instance_options, result, &instance);
  if (!status) status = toad_script_instance_resolve_export(instance, {"echo", 4}, result, &handle);
  toad_script_value_view_t argument{}, value{};
  argument.kind = TOAD_SCRIPT_VALUE_INT64;
  argument.as.integer = INT64_MAX;
  if (!status)
    status = toad_script_instance_call(instance, handle, &argument, 1, &call_options, result);
  if (!status) status = toad_script_result_get_value(result, &value);
  if (!status && (value.kind != TOAD_SCRIPT_VALUE_INT64 || value.as.integer != INT64_MAX))
    status = 2;
  if (instance && toad_script_instance_destroy(instance, result)) status = 3;
  if (module && toad_script_module_destroy(module, result)) status = 4;
  if (result) toad_script_result_destroy(result);
  toad_script_free(ctx);
  return status;
}

suite("Concurrent independent Host contexts") {
  it("initializes executes and destroys contexts on eight simultaneous owners") {
    std::atomic<unsigned> ready{0}, failures{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < 8; ++n)
      threads.emplace_back([&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire))
          std::this_thread::yield();
        for (unsigned iteration = 0; iteration < 16; ++iteration)
          if (run_context(iteration)) failures.fetch_add(1, std::memory_order_relaxed);
      });
    while (ready.load(std::memory_order_acquire) != 8)
      std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto &thread : threads)
      thread.join();
    check_equal(failures.load(), 0u);
  }
}
