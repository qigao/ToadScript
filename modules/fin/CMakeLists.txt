# fin_plugin — import("fin")
set(TARGET_NAME fin)
set(TARGET_NAME_PLUGIN toad_fin_plugin)

cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*"
  EXCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/strategy_plugin.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ta_plugin.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/timeseries_plugin.c")

# ── STATIC lib (for unit tests — direct linking) ─────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME} FOLDER modules/fin EXPORT_NAME Fin)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/fin>)
target_include_directories(${TARGET_NAME} PRIVATE ${SIMDE_INCLUDE_DIRS})

target_link_libraries(${TARGET_NAME} PUBLIC toad_engine Salts::Core)
if(UNIX)
  target_link_libraries(${TARGET_NAME} PRIVATE m)
endif()
# ── SHARED plugin DLL (loaded at runtime via toad_plugin_load) ─────
add_library(${TARGET_NAME_PLUGIN} SHARED src/strategy_plugin.c)

cmake_config_target(${TARGET_NAME_PLUGIN} FOLDER modules/fin OUTPUT_NAME
                    fin PLUGIN)

target_link_libraries(${TARGET_NAME_PLUGIN} PRIVATE ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME_PLUGIN} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                ${SIMDE_INCLUDE_DIRS})

set_target_properties(
  ${TARGET_NAME_PLUGIN}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# Each language module retains its own loader entry point after the source merge.
add_library(toad_ta_plugin SHARED src/ta_plugin.c)
cmake_config_target(toad_ta_plugin FOLDER modules/fin OUTPUT_NAME ta PLUGIN)
target_link_libraries(toad_ta_plugin PRIVATE fin)
set_target_properties(toad_ta_plugin PROPERTIES PREFIX "")

add_library(toad_ts_plugin SHARED src/timeseries_plugin.c)
cmake_config_target(toad_ts_plugin FOLDER modules/fin OUTPUT_NAME ts PLUGIN)
target_link_libraries(toad_ts_plugin PRIVATE fin)
set_target_properties(toad_ts_plugin PROPERTIES PREFIX "")

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fin)

# ── Tests ─────────────────────────────────────────────────────────
if(BUILD_TESTS)
  add_subdirectory(test)
endif()
