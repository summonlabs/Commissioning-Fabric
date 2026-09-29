# Commissioning Fabric - proof obligations.
#
# Every test is a real program that drives the built library or the built CLI as
# an independent process. No test declares a timeout: a test that hangs is a
# defect to diagnose, not a test to kill.

find_package(Threads REQUIRED)

add_executable(cxf_tests
  support/test_harness.cpp
  support/child_process.cpp
  unit_support.cpp
  unit_types.cpp
  unit_model.cpp
  unit_persist.cpp
  property_roundtrip.cpp
  randomized_state_machine.cpp
  adversarial_evidence.cpp
  adversarial_persistence.cpp
  adversarial_paths.cpp
  concurrency_lock.cpp
  crash_consistency.cpp
  multiprocess_store.cpp
)
target_link_libraries(cxf_tests PRIVATE commissioning_fabric cxf_project_warnings cxf_sanitizers
                                        Threads::Threads)
target_include_directories(cxf_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The command-line interface is exercised as a real child process.
add_executable(cxf_cli_tests e2e_cli.cpp support/test_harness.cpp support/child_process.cpp)
target_link_libraries(cxf_cli_tests PRIVATE commissioning_fabric cxf_project_warnings
                                            cxf_sanitizers Threads::Threads)
target_include_directories(cxf_cli_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(
  cxf_cli_tests PRIVATE CXF_CLI_PATH="$<TARGET_FILE:commissioning-fabric>"
)

add_test(NAME cxf.unit COMMAND cxf_tests --suite unit)
add_test(NAME cxf.property COMMAND cxf_tests --suite property)
add_test(NAME cxf.randomized COMMAND cxf_tests --suite randomized)
add_test(NAME cxf.adversarial COMMAND cxf_tests --suite adversarial)
add_test(NAME cxf.concurrency COMMAND cxf_tests --suite concurrency)
add_test(NAME cxf.crash COMMAND cxf_tests --suite crash)
add_test(NAME cxf.multiprocess COMMAND cxf_tests --suite multiprocess)
add_test(NAME cxf.e2e_cli COMMAND cxf_cli_tests)

# The installed-artifact proof: a script that installs this build into a staging
# prefix and then configures, builds and runs an independent out-of-tree consumer
# against it with find_package. It needs the CMake executable and the build tree,
# so it is a script test rather than an in-process one.
add_test(
  NAME cxf.package
  COMMAND
    "${CMAKE_COMMAND}" -E env "CXF_REPO=${CMAKE_SOURCE_DIR}"
    "CXF_CLI=$<TARGET_FILE:commissioning-fabric>" pwsh -NoProfile -File
    "${CMAKE_SOURCE_DIR}/tools/validate-package.ps1" -RepoRoot "${CMAKE_SOURCE_DIR}"
    -BuildDir "${CMAKE_BINARY_DIR}" -Config "$<CONFIG>" -StageDir
    "${CMAKE_BINARY_DIR}/package-validation"
)
