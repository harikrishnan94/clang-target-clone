cmake_minimum_required(VERSION 3.15)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(
  cmake-target-clones
  VERSION 0.1.0
  DESCRIPTION "Clang Target Clone implementation similar to GCC"
  LANGUAGES CXX)

enable_testing()

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

add_subdirectory(src)
