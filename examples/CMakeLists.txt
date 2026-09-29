# Commissioning Fabric - in-tree usage examples.
#
# The examples are ordinary consumers of the installed public surface: they link
# the namespaced target and use only headers that the install step publishes.

add_executable(commissioning_example commissioning_example.cpp)
target_link_libraries(commissioning_example PRIVATE summon::commissioning_fabric
                                                   cxf_project_warnings cxf_sanitizers)
