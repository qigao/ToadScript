cmake_minimum_required(VERSION 3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(CMakeOptions.cmake)

# Toad Project - A Simple DSL with MIR JIT Backend
project(
  Toad
  VERSION 0.1.2
  LANGUAGES C CXX)

# Project configuration - output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Normalize install prefix (remove trailing slashes)
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)

# Project description
set(PROJECT_DESCRIPTION "A simple DSL")
set(PROJECT_HOMEPAGE_URL "https://github.com/qigao/TurboScript")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# Language standards - unified approach
set(CMAKE_CXX_STANDARD 17) # C++17 for compatibility
set(CMAKE_C_STANDARD 11) # C17 to ensure C11 atomic support
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  add_link_options("-Wl,-z,max-page-size=16384")
endif()
# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")

# Enable testing
if(BUILD_TESTS OR BUILD_BENCHMARK)
  enable_testing()
endif()
include(Sanitizers)
include(GNUInstallDirs)
include(CmakeUtils)

# Presets select a preinstalled triplet without running the vcpkg toolchain.
list(PREPEND CMAKE_PREFIX_PATH
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$ENV{VCPKG_CONFIGURATION_SUBDIR}"
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")

find_package(Threads REQUIRED)

find_package(SaltsBase    CONFIG REQUIRED PATHS "$ENV{SALTS_BASE_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(CMeta    CONFIG REQUIRED PATHS "$ENV{CMETA_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils     CONFIG REQUIRED PATHS "$ENV{SALTS_UTILS_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsCNet     CONFIG REQUIRED PATHS "$ENV{SALTSCORE_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(MIR CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_path(SIMDE_INCLUDE_DIRS "simde/arm/neon.h" REQUIRED)

include(FindTools)
add_subdirectory(miniblas)

add_subdirectory(engine)
add_subdirectory(script)
add_subdirectory(modules)

# Package Configuration
include(CMakePackageConfigHelpers)

# Project-owned installation policy
install(
  TARGETS toad_script toad_engine toad_syntax miniblas_linalg
  EXPORT ToadTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(_toad_module_targets
    toad_net_plugin
    toad_fin_plugin
    toad_ta_plugin
    toad_ts_plugin
 )
install(
  TARGETS ${_toad_module_targets}
  LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins)
unset(_toad_module_targets)

install(
  EXPORT ToadTargets
  FILE ToadTargets.cmake
  NAMESPACE Toad::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Toad)

# Generate project config
configure_package_config_file(
  cmake/ToadConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ToadConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Toad
  PATH_VARS CMAKE_INSTALL_BINDIR CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)

write_basic_config_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ToadConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ToadConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/ToadConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Toad)

install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/Toad)
