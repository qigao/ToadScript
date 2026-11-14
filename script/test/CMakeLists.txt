if(BUILD_TESTS)
# Plugin loader fixtures use the same output layout as production plugins.
add_library(test_plugin_loader_fixture SHARED plugin_loader_fixture.c)
cmake_config_target(
  test_plugin_loader_fixture
  FOLDER script
  OUTPUT_NAME loader_fixture
  PLUGIN)
target_include_directories(
  test_plugin_loader_fixture PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(test_plugin_loader_fixture PRIVATE CMeta::PluginABI)
set_target_properties(
  test_plugin_loader_fixture
  PROPERTIES PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)

function(add_plugin_loader_fixture target output_name)
  add_library(${target} SHARED plugin_loader_fixture.c)
  cmake_config_target(
    ${target}
    FOLDER script
    OUTPUT_NAME ${output_name}
    PLUGIN)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
  target_link_libraries(${target} PRIVATE CMeta::PluginABI)
  set_target_properties(
    ${target}
    PROPERTIES PREFIX ""
               RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
               LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)
endfunction()

add_plugin_loader_fixture(test_plugin_loader_prefixed_conflict toad_loader_fixture)
target_compile_definitions(
  test_plugin_loader_prefixed_conflict PRIVATE FIXTURE_PLUGIN_NAME="different_name")

add_plugin_loader_fixture(test_plugin_loader_bad_abi loader_bad_abi)
target_compile_definitions(
  test_plugin_loader_bad_abi
  PRIVATE FIXTURE_PLUGIN_NAME="loader_bad_abi" FIXTURE_ABI_VERSION=999)

add_plugin_loader_fixture(test_plugin_loader_wrong_name loader_wrong_name)
target_compile_definitions(
  test_plugin_loader_wrong_name PRIVATE FIXTURE_PLUGIN_NAME="different_name")

add_plugin_loader_fixture(test_plugin_loader_init_failure loader_init_failure)
target_compile_definitions(
  test_plugin_loader_init_failure
  PRIVATE FIXTURE_PLUGIN_NAME="loader_init_failure" FIXTURE_INIT_FAILURE)

add_plugin_loader_fixture(test_plugin_loader_function loader_function)
target_compile_definitions(
  test_plugin_loader_function
  PRIVATE FIXTURE_PLUGIN_NAME="loader_function" FIXTURE_FUNCTION_ONLY)

add_plugin_loader_fixture(test_plugin_loader_unknown_result loader_unknown_result)
target_compile_definitions(
  test_plugin_loader_unknown_result
  PRIVATE FIXTURE_PLUGIN_NAME="loader_unknown_result"
          FIXTURE_FUNCTION_ONLY FIXTURE_UNKNOWN_RESULT)

add_plugin_loader_fixture(test_plugin_loader_legacy loader_legacy)
target_compile_definitions(
  test_plugin_loader_legacy PRIVATE FIXTURE_PLUGIN_NAME="loader_legacy")
set_target_properties(
  test_plugin_loader_legacy
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_plugin_loader_fixture(test_plugin_loader_cwd_only loader_cwd_only)
target_compile_definitions(
  test_plugin_loader_cwd_only PRIVATE FIXTURE_PLUGIN_NAME="loader_cwd_only")
set_target_properties(
  test_plugin_loader_cwd_only
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_plugin_loader_fixture(test_plugin_loader_package loader_package)
set_target_properties(
  test_plugin_loader_package
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY
             ${CMAKE_BINARY_DIR}/bin/loader-package/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY
             ${CMAKE_BINARY_DIR}/bin/loader-package/bin/plugins)

add_library(test_plugin_loader_missing_entry SHARED plugin_loader_missing_entry.c)
cmake_config_target(
  test_plugin_loader_missing_entry
  FOLDER script
  OUTPUT_NAME loader_missing_entry
  PLUGIN)
target_include_directories(
  test_plugin_loader_missing_entry PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
target_link_libraries(test_plugin_loader_missing_entry PRIVATE CMeta::PluginABI)
set_target_properties(
  test_plugin_loader_missing_entry
  PROPERTIES PREFIX ""
             RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins
             LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/plugins)

cmake_add_test(
  TARGET test_ts_plugin_loader
  SOURCES test_plugin_loader.c
  LIBS toad_script_plugin_loader Salts::TinyTest toad_engine
  INCLUDES ${CMAKE_SOURCE_DIR}/engine/include
  DEPENDS test_plugin_loader_fixture test_plugin_loader_bad_abi
          test_plugin_loader_prefixed_conflict
          test_plugin_loader_wrong_name test_plugin_loader_missing_entry
          test_plugin_loader_init_failure test_plugin_loader_function
          test_plugin_loader_unknown_result test_plugin_loader_legacy test_plugin_loader_cwd_only
          test_plugin_loader_package
  FOLDER script
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_tests_properties(
  test_ts_plugin_loader
  PROPERTIES ENVIRONMENT
             "TOAD_ROOT=$<TARGET_FILE_DIR:test_ts_plugin_loader>/loader-package")

macro(add_ts_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS toad_script_lib Salts::TinyTest CMeta::CSTL Salts::Coroutine toad_engine ta MIR::MIR
    DEPENDS toad_mapper_plugin toad_fin_plugin toad_ta_plugin toad_ts_plugin toad_net_plugin
    FOLDER script)
endmacro()

# Generated DataBind Service Plugin qualification is host-only: salts_idl_target
# requires a host salts-idlc when cross-compiling. Core catalog binding still
# compiles on every target platform through toad_script_plugin_loader.
if(NOT CMAKE_CROSSCOMPILING)
  salts_idl_target(
    TARGET toad_databind_service_fixture
    IDL "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service.schema"
    COMPONENT "Image.ImageProcessor"
    VERSION "1.0.0"
    ARTIFACT_NAME "Image.ImageProcessor"
    # These fixtures exercise the DataBind codec/catalog plugin boundary.
    BINARY_CODEC
    ARTIFACTS PLUGIN
    SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service_business.c")

  salts_idl_target(
    TARGET toad_databind_error_fixture
    IDL "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service_error.schema"
    COMPONENT "ErrorPlugin.StorePlugin"
    VERSION "1.0.0"
    ARTIFACT_NAME "ErrorPlugin.StorePlugin"
    BINARY_CODEC
    ARTIFACTS PLUGIN
    SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/databind_service_error_business.c")

  foreach(_fixture_target IN ITEMS
          ${toad_databind_service_fixture_PLUGIN_TARGET}
          ${toad_databind_error_fixture_PLUGIN_TARGET})
    set_target_properties(
      ${_fixture_target}
      PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")
  endforeach()
endif()

# Interpreter test suites (split by module)
add_ts_test(test_script_basics     test_script_basics.c)
add_dependencies(test_script_basics
  test_plugin_loader_fixture test_plugin_loader_prefixed_conflict
  test_plugin_loader_function toad_crypto_plugin)
add_ts_test(test_script_quant      test_script_quant.c)
add_ts_test(test_script_scientific test_script_scientific.c)
add_ts_test(test_script_io         test_script_io.c)
add_ts_test(test_script_timer      test_script_timer.c)
add_ts_test(test_script_task       test_script_task.c)
if(NOT CMAKE_CROSSCOMPILING)
  add_ts_test(
    test_script_databind_plugin
    test_script_databind_plugin.c)
  add_dependencies(
    test_script_databind_plugin
    ${toad_databind_service_fixture_PLUGIN_TARGET}
    ${toad_databind_error_fixture_PLUGIN_TARGET})
endif()
if(TARGET toad_db_plugin)
  add_ts_test(test_script_db test_script_db.c)
  add_dependencies(test_script_db toad_db_plugin)
  target_compile_definitions(
    test_script_db
    PRIVATE TOAD_TEST_SQLITE_DRIVER_PATH="${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}")
endif()

cmake_add_test(
  TARGET test_script_cmeta_bridge
  SOURCES test_script_cmeta_bridge.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CMeta toad_engine
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
  FOLDER script)
cmake_add_test(
  TARGET test_script_cflow_lower
  SOURCES test_script_cflow_lower.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CMeta CMeta::CFlow toad_engine
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
  FOLDER script)
cmake_add_test(
  TARGET test_script_cflow_mir_kernel
  SOURCES test_script_cflow_mir_kernel.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CMeta CMeta::CFlow MIR::MIR toad_engine
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)
cmake_add_test(
  TARGET test_script_cflow_runtime
  SOURCES test_script_cflow_runtime.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CMeta CMeta::CFlow MIR::MIR toad_engine
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)
add_dependencies(test_script_cflow_runtime test_plugin_loader_function)
cmake_add_test(
  TARGET test_script_cflow_qualification
  SOURCES test_script_cflow_qualification.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CMeta CMeta::CFlow MIR::MIR toad_engine
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)


cmake_add_test(
  TARGET test_script_host_result
  SOURCES test_script_host_result.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CSTL toad_engine ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER script)

cmake_add_test(
  TARGET test_script_host_registry
  SOURCES test_script_host_registry.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CSTL toad_engine ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
  FOLDER script)

cmake_add_test(
  TARGET test_script_host_module
  SOURCES test_script_host_module.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CSTL toad_engine ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)

cmake_add_test(
  TARGET test_script_host_instance
  SOURCES test_script_host_instance.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CSTL toad_engine ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)

cmake_add_test(
  TARGET test_script_host_call
  SOURCES test_script_host_call.c
  LIBS toad_script_lib Salts::TinyTest toad_engine ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)

cmake_add_test(
  TARGET test_script_host_state
  SOURCES test_script_host_state.c
  LIBS toad_script_lib Salts::TinyTest toad_engine ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)

cmake_add_test(
  TARGET test_script_host_limits
  SOURCES test_script_host_limits.c
  LIBS toad_script_lib Salts::TinyTest toad_engine ta MIR::MIR
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/host
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)


# MIR / JIT test suites (split by module)
macro(add_mir_test TARGET_NAME SOURCE_FILE)
  cmake_add_test(
    TARGET ${TARGET_NAME}
    SOURCES ${SOURCE_FILE}
    LIBS toad_script_lib Salts::TinyTest CMeta::CSTL toad_engine MIR::MIR
    DEPENDS toad_mapper_plugin
    FOLDER script)
endmacro()

add_mir_test(test_script_mir        test_script_mir.c)
add_mir_test(test_script_mir_core     test_script_mir_core.c)
add_mir_test(test_script_mir_oop      test_script_mir_oop.c)
add_mir_test(test_script_mir_advanced test_script_mir_advanced.c)

# JIT Performance Benchmarks
endif()

if(BUILD_BENCHMARK)
cmake_add_benchmark(
  TARGET bench_script_cflow
  SOURCES bench_script_cflow.c
  LIBS toad_script_lib CMeta::CMeta CMeta::CFlow MIR::MIR toad_engine
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src/graph
           ${CMAKE_CURRENT_SOURCE_DIR}/../src/mir
  FOLDER script)

add_test(NAME bench_script_cflow COMMAND bench_script_cflow)
set_tests_properties(bench_script_cflow PROPERTIES LABELS benchmark)
cmake_add_benchmark(
  SOURCES bench_script_mir.c
  LIBS toad_script_lib toad_engine MIR::MIR
  FOLDER script)

add_test(NAME bench_script_mir COMMAND bench_script_mir)
set_tests_properties(bench_script_mir PROPERTIES LABELS benchmark)
cmake_add_benchmark(
  SOURCES bench_script.c
  LIBS toad_script_lib Salts::TinyTest CMeta::CSTL toad_engine MIR::MIR
  FOLDER script)

add_test(NAME bench_script COMMAND bench_script)
set_tests_properties(bench_script PROPERTIES LABELS benchmark)
endif()
