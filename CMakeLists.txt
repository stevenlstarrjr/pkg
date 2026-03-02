cmake_minimum_required(VERSION 3.20)
project(npkg_pkg LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pkg_core
  src/tomlc17.c
  src/config.cpp
  src/group.cpp
  src/port.cpp
  src/lockfile.cpp
  src/resolver.cpp
  src/commands.cpp
)

target_include_directories(pkg_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(pkg src/apps/main.cpp)
target_link_libraries(pkg PRIVATE pkg_core)

install(TARGETS pkg RUNTIME DESTINATION /usr/bin)
