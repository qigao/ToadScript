set(TARGET_NAME toad_engine)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(engine_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/lexer.re)
set(engine_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/lexer_gen.c)

add_custom_command(
  OUTPUT ${engine_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${engine_LEXER_GEN} ${engine_LEXER_RE}
  DEPENDS ${engine_LEXER_RE} ${RE2C_EXECUTABLE}
  COMMENT "Generating Toad engine lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(engine_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/grammar.y)
set(engine_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/toad/detail/grammar_gen.c)
set(engine_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/toad/detail/grammar_gen.h)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/toad/detail)

add_custom_command(
  OUTPUT ${engine_GRAMMAR_GEN} ${engine_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${engine_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/toad/detail/grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/toad/detail/grammar_gen.y
  DEPENDS ${engine_GRAMMAR_Y} ${LEMON_DEPENDS} ${LEMPAR}
  COMMENT "Generating Toad engine parser with lemon"
  VERBATIM)

# =============================================================================
# Static frontend/runtime embedded by Toad and plugin binaries
# =============================================================================
cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*")
set(ENGINE_SYNTAX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/grammar/parser.c)
list(REMOVE_ITEM SOURCE_FILES ${ENGINE_SYNTAX_SOURCE})

add_library(
  toad_syntax STATIC
  ${ENGINE_SYNTAX_SOURCE}
  ${engine_LEXER_GEN}
  ${engine_GRAMMAR_GEN}
  ${engine_LEXER_RE}
  ${engine_GRAMMAR_Y}
  ${engine_GRAMMAR_H})

cmake_config_target(
  toad_syntax
  ALIAS
  Toad::Syntax
  FOLDER
  "engine"
  EXPORT_NAME
  Syntax)

target_include_directories(
  toad_syntax
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(toad_syntax PUBLIC Salts::Core Salts::DataBind Salts::DateTimeParser CMeta::CMeta)
if(UNIX)
  target_link_libraries(toad_syntax PRIVATE m)
endif()

add_library(
  ${TARGET_NAME} STATIC
  ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  Toad::Engine
  FOLDER
  "engine"
  EXPORT_NAME
  Engine)

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_include_directories(${TARGET_NAME} PRIVATE ${SIMDE_INCLUDE_DIRS})

target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Toad::Syntax Salts::Core Salts::DataBind Salts::DateTimeParser CMeta::CMeta CMeta::PluginABI
  PRIVATE CMeta::CSTL Salts::Mustache miniblas_linalg)

# Enable AVX/AVX2 for SIMD optimizations
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
  if(MSVC)
    target_compile_options(${TARGET_NAME} PRIVATE /arch:AVX2)
  else()
    target_compile_options(${TARGET_NAME} PRIVATE -mavx2 -mfma)
  endif()
endif()
if(UNIX)
  target_link_libraries(${TARGET_NAME} PUBLIC m)
endif()

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Install headers
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)
install(FILES ${engine_GRAMMAR_H} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/toad/detail)

if(BUILD_TESTS OR BUILD_BENCHMARK)
  add_subdirectory(test)
endif()
