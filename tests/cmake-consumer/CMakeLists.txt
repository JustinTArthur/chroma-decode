# Smoke test: a minimal downstream project that links libchromadec exactly as a
# real consumer would, via the installed CMake package config. Build + run this
# against an installed prefix to verify chromadecConfig.cmake resolves and the
# shared library links and loads.
cmake_minimum_required(VERSION 3.16)
project(chromadec_consumer C)

find_package(chromadec CONFIG REQUIRED)

add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE chromadec::chromadec)