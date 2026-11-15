add_executable(toad main.c)
cmake_config_target(toad FOLDER script)

target_link_libraries(toad
  PRIVATE toad_script Salts::DataBind)

add_dependencies(toad toad_fin_plugin toad_ta_plugin toad_ts_plugin)

if(BUILD_TESTS)
  set(TOAD_SCRIPT_GOF_EXAMPLES
    factory_builder.toad
    adapter_decorator.toad
    composite_visitor.toad
    command_memento.toad
    state_chain.toad)

  foreach(EXAMPLE_FILE IN LISTS TOAD_SCRIPT_GOF_EXAMPLES)
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_FILE} NAME_WE)
    add_test(
      NAME example_gof_${EXAMPLE_NAME}
      COMMAND $<TARGET_FILE:toad>
              --file ${CMAKE_SOURCE_DIR}/examples/gof/${EXAMPLE_FILE})
    set_tests_properties(example_gof_${EXAMPLE_NAME}
      PROPERTIES LABELS "examples;gof")
  endforeach()

  add_test(
    NAME example_polymarket_patterns_load
    COMMAND $<TARGET_FILE:toad>
            --file ${CMAKE_SOURCE_DIR}/examples/polymarket_patterns.toad)
  set_tests_properties(example_polymarket_patterns_load
    PROPERTIES LABELS "examples;polymarket")

  add_test(
    NAME example_getting_started
    COMMAND $<TARGET_FILE:toad>
            --file ${CMAKE_SOURCE_DIR}/examples/getting_started.toad)
  set_tests_properties(example_getting_started
    PROPERTIES LABELS "examples;getting-started")

  add_test(
    NAME toad_script_cli_uses_actual_module_names
    COMMAND $<TARGET_FILE:toad>
            --eval "import(\"fin\"); import(\"ts\"); module_names_ok = 1;")
  set_tests_properties(toad_script_cli_uses_actual_module_names
    PROPERTIES LABELS "toad_script;cli;plugins")

  add_test(
    NAME toad_script_cli_reports_batch_errors
    COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:toad>
            -DINVALID_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/test/invalid.toad
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_cli_errors.cmake)
  set_tests_properties(toad_script_cli_reports_batch_errors
    PROPERTIES LABELS "toad_script;cli")
endif()
