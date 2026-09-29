cmake_minimum_required(VERSION 3.25)

# Commissioning Fabric - DCCP Tranche 5: Physical Fleet Lifecycle.
#
# Top-level project file. Builds the library, an administration CLI, tests,
# benchmarks and out-of-tree examples, and installs a namespaced imported
# target (summon::commissioning_fabric) for downstream consumption.

project(
  CommissioningFabric
  VERSION 1.0.0
  DESCRIPTION "DCCP governed commissioning of physical facility assets"
  HOMEPAGE_URL "https://github.com/summonlabs/Commissioning-Fabric"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

option(CXF_BUILD_TESTS "Build the Commissioning Fabric test suite" ON)
option(CXF_BUILD_BENCHMARKS "Build the Commissioning Fabric benchmarks" ON)
option(CXF_BUILD_EXAMPLES "Build the Commissioning Fabric examples" ON)
option(CXF_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(CXF_ENABLE_ASAN "Enable AddressSanitizer for first-party targets" OFF)
option(CXF_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (GCC/Clang only)" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Build type" FORCE
  )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # Portable _CRT_SECURE_NO_WARNINGS: the library avoids the CRT functions that
  # would need it, but tests and CLI use std::getenv / std::FILE deliberately.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------------------
# First-party warning policy: strict in every configuration.
# ---------------------------------------------------------------------------
add_library(cxf_project_warnings INTERFACE)
if(MSVC)
  target_compile_options(
    cxf_project_warnings
    INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc
              /guard:cf
  )
  if(CXF_WARNINGS_AS_ERRORS)
    target_compile_options(cxf_project_warnings INTERFACE /WX)
  endif()
  target_compile_options(
    cxf_project_warnings
    INTERFACE "$<$<CONFIG:Release>:/O2>"
              "$<$<CONFIG:RelWithDebInfo>:/O2>"
  )
else()
  target_compile_options(
    cxf_project_warnings
    INTERFACE -Wall
              -Wextra
              -Wpedantic
              -Wshadow
              -Wnon-virtual-dtor
              -Wold-style-cast
              -Wcast-align
              -Wunused
              -Woverloaded-virtual
              -Wconversion
              -Wsign-conversion
              -Wnull-dereference
              -Wdouble-promotion
              -Wformat=2
  )
  if(CXF_WARNINGS_AS_ERRORS)
    target_compile_options(cxf_project_warnings INTERFACE -Werror)
  endif()
endif()

add_library(cxf_sanitizers INTERFACE)
if(CXF_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(cxf_sanitizers INTERFACE /fsanitize=address)
    target_link_options(cxf_sanitizers INTERFACE /INCREMENTAL:NO)
  else()
    target_compile_options(cxf_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(cxf_sanitizers INTERFACE -fsanitize=address)
  endif()
endif()
if(CXF_ENABLE_UBSAN AND NOT MSVC)
  target_compile_options(cxf_sanitizers INTERFACE -fsanitize=undefined -fno-sanitize-recover=undefined)
  target_link_options(cxf_sanitizers INTERFACE -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Generated version header.
# ---------------------------------------------------------------------------
set(CXF_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CXF_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CXF_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CXF_VERSION_STRING "${PROJECT_VERSION}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/cxf/version.hpp"
  @ONLY
)

# ---------------------------------------------------------------------------
# Library.
# ---------------------------------------------------------------------------
set(CXF_PUBLIC_HEADERS
    include/cxf/cxf.hpp
    include/cxf/codec/archive.hpp
    include/cxf/support/crc32c.hpp
    include/cxf/support/digest.hpp
    include/cxf/support/serial.hpp
    include/cxf/support/text.hpp
    include/cxf/support/clock.hpp
    include/cxf/support/status.hpp
    include/cxf/support/fs.hpp
    include/cxf/support/lock_file.hpp
    include/cxf/types/ids.hpp
    include/cxf/types/generations.hpp
    include/cxf/types/enums.hpp
    include/cxf/types/timepoint.hpp
    include/cxf/model/candidate.hpp
    include/cxf/model/lifecycle.hpp
    include/cxf/model/evidence.hpp
    include/cxf/model/readiness.hpp
    include/cxf/model/dependency.hpp
    include/cxf/model/record.hpp
    include/cxf/persist/record_io.hpp
    include/cxf/persist/journal.hpp
    include/cxf/persist/snapshot.hpp
    include/cxf/runtime/authority.hpp
    include/cxf/runtime/event.hpp
    include/cxf/runtime/plan.hpp
    include/cxf/runtime/fabric.hpp
    include/cxf/runtime/explain.hpp
    include/cxf/tools/registry.hpp
    include/cxf/tools/textlog.hpp
    include/cxf/codec/text_codec.hpp
)

add_library(commissioning_fabric
  src/support/status.cpp
  src/support/serial.cpp
  src/support/crc32c.cpp
  src/support/digest.cpp
  src/support/text.cpp
  src/support/clock.cpp
  src/support/fs.cpp
  src/support/lock_file.cpp
  src/types/enums.cpp
  src/types/generations.cpp
  src/types/timepoint.cpp
  src/model/candidate.cpp
  src/model/lifecycle.cpp
  src/model/evidence.cpp
  src/model/readiness.cpp
  src/model/dependency.cpp
  src/model/record.cpp
  src/persist/record_io.cpp
  src/persist/journal.cpp
  src/persist/snapshot.cpp
  src/runtime/authority.cpp
  src/runtime/plan.cpp
  src/runtime/event.cpp
  src/runtime/fabric.cpp
  src/runtime/explain.cpp
  src/tools/registry.cpp
  src/tools/textlog.cpp
  src/codec/text_codec.cpp
)
add_library(summon::commissioning_fabric ALIAS commissioning_fabric)

target_include_directories(
  commissioning_fabric
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features(commissioning_fabric PUBLIC cxx_std_20)
# The warning and sanitizer interfaces are build-time policy, not part of the
# installed package: wrapping them in BUILD_INTERFACE keeps them out of the
# exported target set while still applying to this build.
target_link_libraries(
  commissioning_fabric
  PRIVATE "$<BUILD_INTERFACE:cxf_project_warnings>" "$<BUILD_INTERFACE:cxf_sanitizers>"
)

# Default symbol visibility is deliberately left alone: this project ships no
# export macros, so hiding symbols would silently break a shared-library build on
# toolchains that honour the visibility preset.
set_target_properties(
  commissioning_fabric
  PROPERTIES VERSION "${PROJECT_VERSION}"
             SOVERSION "${PROJECT_VERSION_MAJOR}"
             POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_options(commissioning_fabric PRIVATE /W4)
else()
  target_compile_definitions(commissioning_fabric PRIVATE CXF_BUILDING_LIBRARY=1)
endif()

# ---------------------------------------------------------------------------
# CLI, tests, benchmarks, examples.
# ---------------------------------------------------------------------------
add_subdirectory(apps/commissioning-fabric)

if(CXF_BUILD_TESTS AND BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CXF_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(CXF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Install / export / package.
# ---------------------------------------------------------------------------
install(
  TARGETS commissioning_fabric
  EXPORT CommissioningFabricTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/cxf"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/cxf/version.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxf"
)

install(
  EXPORT CommissioningFabricTargets
  FILE CommissioningFabricTargets.cmake
  NAMESPACE summon::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CommissioningFabric"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommissioningFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/CommissioningFabricConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CommissioningFabric"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CommissioningFabricConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CommissioningFabricConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/CommissioningFabricConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CommissioningFabric"
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
              "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/CommissioningFabric"
        OPTIONAL
)

# The supported downstream contract is the installed package produced by
# install(EXPORT ...) above. No build-tree export file is written: the warning
# and sanitizer interfaces are build-time policy targets that are deliberately
# absent from the installed target set.

set(CPACK_PACKAGE_NAME "CommissioningFabric")
set(CPACK_PACKAGE_VENDOR "Summon Software Labs")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
