# Commissioning Fabric - the administration CLI.
#
# The CLI is an ordinary consumer of the public surface: it links the library
# target together with the project's warning and sanitizer interface targets,
# and includes only headers that the install step publishes. The installed
# package ships the administration tool next to the library.

add_executable(commissioning-fabric main.cpp)
target_compile_features(commissioning-fabric PRIVATE cxx_std_20)
target_link_libraries(commissioning-fabric PRIVATE commissioning_fabric
                                                     cxf_project_warnings cxf_sanitizers)

include(GNUInstallDirs)
install(TARGETS commissioning-fabric RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
