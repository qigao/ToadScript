if(BUILD_TESTS)
set(ENGINE_TEST_LIBS toad_engine Salts::TinyTest)
set(ENGINE_TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include)

cmake_add_test(
  TARGET test_engine_syntax
  SOURCES test_engine_syntax.c
  LIBS Toad::Syntax Salts::TinyTest
  INCLUDES ${ENGINE_TEST_INCLUDES}
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  SOURCES test_engine.c
  LIBS ${ENGINE_TEST_LIBS}
  INCLUDES ${ENGINE_TEST_INCLUDES}
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  SOURCES test_engine_checked_registration.c
  LIBS ${ENGINE_TEST_LIBS}
  INCLUDES ${ENGINE_TEST_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  SOURCES test_simple_opt.c
  LIBS toad_engine
  INCLUDES ${ENGINE_TEST_INCLUDES}
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  TARGET test_engine_optimizations
  SOURCES test_optimizations.c
  LIBS toad_engine
  INCLUDES ${ENGINE_TEST_INCLUDES}
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


cmake_add_test(
  SOURCES test_mod_math.c
  LIBS ${ENGINE_TEST_LIBS}
  INCLUDES ${ENGINE_TEST_INCLUDES}
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(UNIX)
  target_link_libraries(test_mod_math PRIVATE m)
endif()

cmake_add_test(
  SOURCES test_simd_helpers.c test_mod_io.c test_oop.c test_cmeta_class.c test_cmeta_function.c
  LIBS ${ENGINE_TEST_LIBS}
  INCLUDES ${ENGINE_TEST_INCLUDES}
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

cmake_add_test(
  TARGET test_tiny_regex
  SOURCES test_re.c
  LIBS ${ENGINE_TEST_LIBS}
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(BUILD_BENCHMARK)
cmake_add_benchmark(
  SOURCES test_simd_perf.c
  LIBS toad_engine
  INCLUDES ${ENGINE_TEST_INCLUDES}
  FOLDER engine
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME test_simd_perf COMMAND test_simd_perf)
set_tests_properties(test_simd_perf PROPERTIES LABELS benchmark)
if(UNIX)
  target_link_libraries(test_simd_perf PRIVATE m)
endif()
endif()
