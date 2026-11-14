project(toad_script)

set(TARGET_NAME toad_script)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)
set(TARGET_NAME_PLUGIN_LOADER ${TARGET_NAME}_plugin_loader)

# Host-only native module loader. It is deliberately separate from the JIT
# runtime so the ABI boundary can be tested without admitting task/timer code.
add_library(
  ${TARGET_NAME_PLUGIN_LOADER}
  STATIC
  src/host/plugin_loader.c
  src/host/plugin_databind.c
  src/host/plugin_databind.h)
cmake_config_target(${TARGET_NAME_PLUGIN_LOADER} FOLDER script)
target_include_directories(
  ${TARGET_NAME_PLUGIN_LOADER}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_SOURCE_DIR}/engine/include
          ${CMAKE_SOURCE_DIR}/engine/src)
target_link_libraries(
  ${TARGET_NAME_PLUGIN_LOADER}
  PUBLIC CMeta::Plugin
  PRIVATE toad_engine Salts::DataBind)

# Source files listed explicitly to avoid CMake globbing configuration issues
set(SOURCE_FILES
  src/graph/cflow_lower.c
  src/graph/cflow_lower.h
  src/graph/cflow_mir_kernel.c
  src/graph/cflow_mir_kernel.h
  src/graph/cflow_runtime.c
  src/graph/cflow_runtime.h
  src/graph/cflow_text.c
  src/graph/cflow_text.h
  src/graph/cflow_text_kernel.c
  src/graph/cflow_text_kernel.h
  src/graph/cmeta_bridge.c
  src/graph/cmeta_bridge.h
  src/host/instance.c
  src/host/limits.c
  src/host/module.c
  src/host/registry.c
  src/host/result.c
  src/mir_mod.c
  src/script.c
  src/closure_analysis.c
  src/closure_analysis.h
  src/internal.h
  src/registry.c
  src/task.c
  src/task.h
  src/timer.c
  src/timer.h
)

# MIR backend modules. mir.c still owns the main AST lowering path
# while cohesive subsystems are moved into src/mir/*.c modules.
set(MIR_BACKEND_FILES
  src/mir.c
  src/mir/api.c
  src/mir/cache.c
  src/mir/compiler.c
  src/mir/constant.c
  src/mir/data.c
  src/mir/functions.c
  src/mir/oop.c
  src/mir/runtime.c
  src/mir/value_emit.c
  src/mir/value_runtime.c
)

# Combine all sources
set(ALL_SOURCE_FILES ${SOURCE_FILES} ${MIR_BACKEND_FILES})

# ── STATIC lib for testing ────────────────────────────────────────
add_library(${TARGET_NAME_LIB} STATIC ${ALL_SOURCE_FILES})
cmake_config_target(${TARGET_NAME_LIB} FOLDER script)

target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/engine/src
          ${CMAKE_CURRENT_SOURCE_DIR}/src/graph
          ${CMAKE_CURRENT_SOURCE_DIR}/src/host
          ${CMAKE_CURRENT_SOURCE_DIR}/src/mir)  # MIR backend internal headers

target_link_libraries(
  ${TARGET_NAME_LIB}
  PRIVATE toad_engine MIR::MIR CMeta::CSTL CMeta::CMeta CMeta::CFlow ${TARGET_NAME_PLUGIN_LOADER}
  PUBLIC Salts::Core Salts::DataBind
  PRIVATE  Salts::Coroutine)

# Enable strict warnings for modularized code
if(MSVC)
  target_compile_options(${TARGET_NAME_LIB} PRIVATE /W4)
else()
  target_compile_options(${TARGET_NAME_LIB} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── SHARED library ────────────────────────────────────────────────
add_library(${TARGET_NAME} SHARED ${ALL_SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  Toad::Script
  FOLDER
  script
  EXPORT_NAME
  Script)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/engine/src
          ${CMAKE_CURRENT_SOURCE_DIR}/src/host
          ${CMAKE_CURRENT_SOURCE_DIR}/src/mir)  # MIR backend internal headers

target_compile_definitions(
  ${TARGET_NAME}
  PRIVATE TOAD_SCRIPT_BUILD_SHARED
  INTERFACE TOAD_SCRIPT_USE_SHARED)

set(_toad_public_symbols_file
    "${CMAKE_CURRENT_SOURCE_DIR}/exports/public-symbols.txt")
file(STRINGS "${_toad_public_symbols_file}" _toad_public_symbols
     REGEX "^[A-Za-z_][A-Za-z0-9_]*$")

if(APPLE)
  set(_toad_export_file
      "${CMAKE_CURRENT_BINARY_DIR}/toad-exported-symbols.txt")
  file(WRITE "${_toad_export_file}" "")
  foreach(_toad_symbol IN LISTS _toad_public_symbols)
    file(APPEND "${_toad_export_file}" "_${_toad_symbol}\n")
  endforeach()
  target_link_options(
    ${TARGET_NAME}
    PRIVATE "-Wl,-exported_symbols_list,${_toad_export_file}")
elseif(UNIX)
  set(_toad_export_file
      "${CMAKE_CURRENT_BINARY_DIR}/toad-version-script.map")
  file(WRITE "${_toad_export_file}" "{\n  global:\n")
  foreach(_toad_symbol IN LISTS _toad_public_symbols)
    file(APPEND "${_toad_export_file}" "    ${_toad_symbol};\n")
  endforeach()
  file(APPEND "${_toad_export_file}" "  local:\n    *;\n};\n")
  target_link_options(
    ${TARGET_NAME}
    PRIVATE "-Wl,--version-script=${_toad_export_file}")
endif()

target_link_libraries(
  ${TARGET_NAME}
  PRIVATE toad_engine MIR::MIR CMeta::CSTL CMeta::CMeta CMeta::CFlow ${TARGET_NAME_PLUGIN_LOADER}
  PUBLIC Salts::Core
  PRIVATE Salts::Coroutine)

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)


if(BUILD_TESTS OR BUILD_BENCHMARK)
  add_subdirectory(test)
endif()
add_subdirectory(app)
