# Commissioning Fabric - benchmarks.
#
# Benchmarks measure completed operations against the real durable store on this
# host. Workload generation is synthetic; the storage under measurement is not.

add_executable(cxf_benchmark fabric_benchmark.cpp)
target_link_libraries(cxf_benchmark PRIVATE commissioning_fabric cxf_project_warnings
                                            cxf_sanitizers)
