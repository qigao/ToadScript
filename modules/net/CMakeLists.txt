set(TARGET_NAME toad_net_plugin)
set(TARGET_NAME_LIB ${TARGET_NAME}_lib)

cmake_add_source(SOURCE_FILES RECURSE PATTERNS "*.c*" "*.h*")

# Static lib for tests
add_library(${TARGET_NAME_LIB} STATIC ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME_LIB} FOLDER modules/net)

target_link_libraries(${TARGET_NAME_LIB} PUBLIC toad_engine CHttp::Client Salts::DataBind Salts::JsonParser toad_script)
target_include_directories(
  ${TARGET_NAME_LIB}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/net>)
# Shared plugin DLL
add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME} FOLDER modules/net OUTPUT_NAME net
                    PLUGIN)

target_link_libraries(${TARGET_NAME} PUBLIC toad_engine CHttp::Client Salts::DataBind Salts::JsonParser toad_script)
target_include_directories(${TARGET_NAME} PRIVATE ${net_INCLUDES})
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES PREFIX ""
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# CMeta loads explicit plugin paths with a restricted Windows DLL search.
# Stage the selected target dependencies beside the plugin for that contract.
if(WIN32)
  add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${TARGET_NAME}>
            $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND_EXPAND_LISTS
    VERBATIM)
  install(FILES $<TARGET_RUNTIME_DLLS:${TARGET_NAME}>
    DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins)
endif()

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/net)
if(BUILD_TESTS)
  add_subdirectory(test)
endif()
