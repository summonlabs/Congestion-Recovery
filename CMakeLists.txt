cmake_minimum_required(VERSION 3.20)
project(congestion_recovery VERSION 1.0.0 LANGUAGES CXX)

option(CR_BUILD_TESTS "Build Congestion Recovery tests" ON)
option(CR_BUILD_EXAMPLES "Build Congestion Recovery examples" ON)
option(CR_BUILD_TOOLS "Build Congestion Recovery tools" ON)
option(CR_BUILD_BENCHMARKS "Build Congestion Recovery benchmarks" ON)
option(CR_ENABLE_ASAN "Enable AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(congestion_recovery STATIC
  src/adjacent.cpp
  src/authority.cpp
  src/coordinator.cpp
  src/engine.cpp
  src/explain.cpp
  src/journal.cpp
  src/obligations.cpp
  src/plan.cpp
  src/policy.cpp
  src/protocol.cpp
  src/restoration.cpp
)
add_library(CongestionRecovery::CongestionRecovery ALIAS congestion_recovery)
set_target_properties(congestion_recovery PROPERTIES EXPORT_NAME CongestionRecovery)
target_include_directories(congestion_recovery
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_compile_features(congestion_recovery PUBLIC cxx_std_20)

if(WIN32)
  target_link_libraries(congestion_recovery PUBLIC ws2_32)
endif()

if(MSVC)
  target_compile_options(congestion_recovery PRIVATE /W4 /WX /MP)
  if(CR_ENABLE_ASAN)
    target_compile_options(congestion_recovery PRIVATE /fsanitize=address /Zi)
    target_link_options(congestion_recovery PRIVATE /fsanitize=address)
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(congestion_recovery PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(CR_ENABLE_ASAN)
    target_compile_options(congestion_recovery PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(congestion_recovery PRIVATE -fsanitize=address)
  endif()
endif()

if(CR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(CR_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(CR_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(CR_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS congestion_recovery
  EXPORT CongestionRecoveryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT CongestionRecoveryTargets
  FILE CongestionRecoveryTargets.cmake
  NAMESPACE CongestionRecovery::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CongestionRecovery)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CongestionRecoveryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionRecoveryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CongestionRecovery)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionRecoveryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionRecoveryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CongestionRecoveryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CongestionRecovery)
