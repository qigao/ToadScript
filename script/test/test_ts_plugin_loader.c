#include "tinytest.h" /* Toad host plugin-loader regression. */
#include <toad/plugin_loader.h>
#include <toad/engine.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#if defined(_WIN32)
#define LOADER_FIXTURE_FILE "loader_fixture.dll"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.dll"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.dll"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.dll"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.dll"
#define LOADER_FUNCTION_FILE "loader_function.dll"
#define LOADER_UNKNOWN_RESULT_FILE "loader_unknown_result.dll"
#define LOADER_LEGACY_FILE "loader_legacy.dll"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.dll"
#define LOADER_PACKAGE_FILE "loader_package.dll"
#elif defined(__APPLE__)
#define LOADER_FIXTURE_FILE "loader_fixture.dylib"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.dylib"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.dylib"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.dylib"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.dylib"
#define LOADER_FUNCTION_FILE "loader_function.dylib"
#define LOADER_UNKNOWN_RESULT_FILE "loader_unknown_result.dylib"
#define LOADER_LEGACY_FILE "loader_legacy.dylib"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.dylib"
#define LOADER_PACKAGE_FILE "loader_package.dylib"
#else
#define LOADER_FIXTURE_FILE "loader_fixture.so"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.so"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.so"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.so"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.so"
#define LOADER_FUNCTION_FILE "loader_function.so"
#define LOADER_UNKNOWN_RESULT_FILE "loader_unknown_result.so"
#define LOADER_LEGACY_FILE "loader_legacy.so"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.so"
#define LOADER_PACKAGE_FILE "loader_package.so"
#endif

spec("plugin loader") {
  describe("executable-relative discovery") {
    it("loads a plugin from the plugins directory beside the executable") {
      toad_plugin_handle_t *handle = toad_plugin_load(LOADER_FIXTURE_FILE);

      check_not_null(handle);
      if (handle) {
        check_not_null(handle->manifest);
        check_equal(handle->manifest->plugin_id, "loader_fixture");
        toad_plugin_unload(handle);
      }
    }

    it("keeps the executable directory as a compatibility fallback") {
      toad_plugin_handle_t *handle = toad_plugin_load(LOADER_LEGACY_FILE);

      check_not_null(handle);
      if (handle) toad_plugin_unload(handle);
    }

    it("loads a plugin from the configured Toad package") {
      toad_plugin_handle_t *handle = toad_plugin_load(LOADER_PACKAGE_FILE);

      check_not_null(handle);
      if (handle) {
        check_not_null(handle->manifest);
        check_equal(handle->manifest->plugin_id, "loader_fixture");
        toad_plugin_unload(handle);
      }
    }

    it("does not search the current working directory") {
      toad_plugin_error_t error = {0};
      toad_plugin_handle_t *handle = NULL;

      check_equal(toad_plugin_load_ex(LOADER_CWD_ONLY_FILE, NULL, &handle, &error),
                  TOAD_PLUGIN_ERROR_OPEN);
      check_null(handle);
      check_equal(error.stage, TOAD_PLUGIN_STAGE_OPEN);
    }
  }

  describe("descriptor validation") {
    it("rejects a plugin built for a different ABI") {
      toad_plugin_error_t error = {0};
      toad_plugin_handle_t *handle = NULL;

      check_equal(toad_plugin_load_ex(LOADER_BAD_ABI_FILE, "loader_bad_abi", &handle, &error),
                  TOAD_PLUGIN_ERROR_ABI);
      check_null(handle);
      check_equal(error.stage, TOAD_PLUGIN_STAGE_ABI);
      check_not_null(strstr(error.message, "ABI"));
    }

    it("rejects a descriptor whose name differs from the requested plugin") {
      toad_plugin_error_t error = {0};
      toad_plugin_handle_t *handle = NULL;

      check_equal(toad_plugin_load_ex(LOADER_WRONG_NAME_FILE, "loader_wrong_name", &handle, &error),
                  TOAD_PLUGIN_ERROR_NAME);
      check_null(handle);
      check_equal(error.stage, TOAD_PLUGIN_STAGE_ABI);
      check_not_null(strstr(error.message, "name"));
    }

    it("reports a missing plugin entry point separately from open failures") {
      toad_plugin_error_t error = {0};
      toad_plugin_handle_t *handle = NULL;

      check_equal(toad_plugin_load_ex(LOADER_MISSING_ENTRY_FILE, NULL, &handle, &error),
                  TOAD_PLUGIN_ERROR_SYMBOL);
      check_null(handle);
      check_equal(error.stage, TOAD_PLUGIN_STAGE_SYMBOL);
      check_not_null(strstr(error.message, "cmeta_plugin_query"));
    }
  }

  describe("canonical Function exports") {
    it("rejects legacy UNKNOWN semantics for non-void scalar returns") {
      toad_plugin_error_t error = {0};
      toad_plugin_handle_t *handle = NULL;
      toad_env_t env;

      toad_env_init(&env);
      check_equal(
          toad_plugin_load_ex(
              LOADER_UNKNOWN_RESULT_FILE, "loader_unknown_result",
              &handle, &error),
          TOAD_PLUGIN_ERROR_NONE);
      check_not_null(handle);
      if (handle) {
        check_equal(
            toad_plugin_init_ex(handle, &env, NULL, &error),
            TOAD_PLUGIN_ERROR_DESCRIPTOR);
        check_equal(error.stage, TOAD_PLUGIN_STAGE_INITIALIZE);
        check_not_null(strstr(error.message, "canonical finite scalar ABI"));
        toad_plugin_unload(handle);
      }
      toad_env_free(&env);
    }

    it("loads and binds a Function-only plugin without a module adapter") {
      toad_plugin_error_t error = {0};
      toad_plugin_handle_t *handle = NULL;
      toad_env_t env;
      toad_value_t arg;
      toad_value_t result;

      toad_env_init(&env);
      check_equal(
          toad_plugin_load_ex(
              LOADER_FUNCTION_FILE, "loader_function", &handle, &error),
          TOAD_PLUGIN_ERROR_NONE);
      check_not_null(handle);
      if (handle) {
        check_null(handle->module);
        check_equal(handle->manifest->export_count, (size_t)10u);
        check_equal(handle->manifest->exports[0].kind,
                    CMETA_PLUGIN_EXPORT_FUNCTION);
        for (size_t i = 0u; i < 9u; ++i) {
          check_not_null(handle->manifest->exports[i].value.function.desc);
          check_equal(
              handle->manifest->exports[i].value.function.desc->result_flags,
              (cmeta_result_flags)CMETA_RESULT_VALUE);
        }
        check_not_null(handle->manifest->exports[9].value.function.desc);
        check_equal(
            handle->manifest->exports[9].value.function.desc->result_flags,
            (cmeta_result_flags)CMETA_RESULT_UNKNOWN);
        check_equal(
            toad_plugin_init_ex(handle, &env, NULL, &error),
            TOAD_PLUGIN_ERROR_NONE);
        check_equal(handle->function_binding_count, (size_t)10u);
        {
          toad_plugin_function_view_t view = {0};
          check_true(toad_plugin_find_bound_function(
              handle, "loader_function.double", &view));
          check_not_null(view.entry);
          check_not_null(view.entry->value.function.desc);
          check_equal(
              view.entry->value.function.desc->result_flags,
              (cmeta_result_flags)CMETA_RESULT_VALUE);
        }
        check_true(toad_env_has_func(&env, "loader_function.double"));
        check_true(toad_env_has_func(&env, "loader_function.stateful"));
        check_true(toad_env_has_func(&env, "loader_function.increment"));
        check_true(toad_env_has_func(&env, "loader_function.positive"));
        check_true(toad_env_has_func(&env, "loader_function.long_to_double"));
        check_true(toad_env_has_func(&env, "loader_function.long_exact"));
        check_true(toad_env_has_func(&env, "loader_function.float_to_double"));
        check_true(toad_env_has_func(&env, "loader_function.add"));
        check_true(toad_env_has_func(&env, "loader_function.answer"));
        check_true(toad_env_has_func(&env, "loader_function.notify"));

        arg = toad_val_num(3.5);
        result = toad_call_internal(
            "loader_function.double", 1u, &arg, &env);
        check_equal(result.type, TOAD_VAL_NUMBER);
        check(fabs(result.data.number - 7.0) <= 1e-9);
        toad_value_destroy(&result);

        arg = toad_val_num(4.0);
        result = toad_call_internal(
            "loader_function.increment", 1u, &arg, &env);
        check_equal(result.type, TOAD_VAL_INTEGER);
        check_equal(result.data.integer, (int64_t)5);
        toad_value_destroy(&result);

        arg = toad_val_num(-2.0);
        result = toad_call_internal(
            "loader_function.positive", 1u, &arg, &env);
        check_equal(result.type, TOAD_VAL_BOOL);
        check_false(result.data.boolean);
        toad_value_destroy(&result);

        arg = toad_val_num(7.0);
        result = toad_call_internal(
            "loader_function.long_to_double", 1u, &arg, &env);
        check_equal(result.type, TOAD_VAL_NUMBER);
        check(fabs(result.data.number - 7.5) <= 1e-9);
        toad_value_destroy(&result);

#if LONG_MAX > 2147483647L
        arg = toad_val_int(INT64_C(9007199254740993));
#else
        arg = toad_val_int((int64_t)LONG_MAX);
#endif
        result = toad_call_internal(
            "loader_function.long_exact", 1u, &arg, &env);
        check_equal(result.type, TOAD_VAL_NUMBER);
        check(fabs(result.data.number - 1.0) <= 1e-9);
        toad_value_destroy(&result);

        arg = toad_val_num(1.5);
        result = toad_call_internal(
            "loader_function.float_to_double", 1u, &arg, &env);
        check_equal(result.type, TOAD_VAL_NUMBER);
        check(fabs(result.data.number - 1.75) <= 1e-6);
        toad_value_destroy(&result);

        {
          toad_value_t args[2] = {
              toad_val_num(1.25), toad_val_num(2.5)};
          result = toad_call_internal(
              "loader_function.add", 2u, args, &env);
          check_equal(result.type, TOAD_VAL_NUMBER);
          check(fabs(result.data.number - 3.75) <= 1e-9);
          toad_value_destroy(&result);
        }

        result = toad_call_internal(
            "loader_function.answer", 0u, NULL, &env);
        check_equal(result.type, TOAD_VAL_INTEGER);
        check_equal(result.data.integer, (int64_t)42);
        toad_value_destroy(&result);

        result = toad_call_internal(
            "loader_function.notify", 0u, NULL, &env);
        check_equal(result.type, TOAD_VAL_NULL);
        toad_value_destroy(&result);

        check_false(env.aborted);

        arg = toad_val_num(1.5);
        result = toad_call_internal(
            "loader_function.increment", 1u, &arg, &env);
        check_true(env.aborted);
        check_not_null(strstr(env.error_msg, "argument 0"));
        toad_value_destroy(&result);
      }

      toad_env_free(&env);
      if (handle) toad_plugin_unload(handle);
    }
  }

  describe("initialization cleanup") {
    it("does not call plugin unload when initialization returned no instance") {
      int failed_init_unload_count = 0;
      toad_plugin_error_t error = {0};
      toad_plugin_handle_t *handle = NULL;

      check_equal(toad_plugin_load_ex(LOADER_INIT_FAILURE_FILE, "loader_init_failure", &handle,
                                    &error), TOAD_PLUGIN_ERROR_NONE);
      check_not_null(handle);
      if (handle) {
        check_equal(toad_plugin_init_ex(handle, (void *)1, &failed_init_unload_count, &error),
                    TOAD_PLUGIN_ERROR_INITIALIZE);
        check_equal(error.stage, TOAD_PLUGIN_STAGE_INITIALIZE);
        toad_plugin_unload(handle);
      }
      check_equal(failed_init_unload_count, 0);
    }
  }
}
