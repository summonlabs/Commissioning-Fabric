# An independent out-of-tree consumer of the installed Commissioning Fabric
# package. It is deliberately NOT part of the repository build: it only builds
# when it is configured on its own against an installed prefix, which is what
# proves the exported package is usable downstream.

cmake_minimum_required(VERSION 3.25)
project(CommissioningFabricDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CommissioningFabric 1.0 CONFIG REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE summon::commissioning_fabric)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Werror)
endif()
