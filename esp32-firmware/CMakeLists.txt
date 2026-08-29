cmake_minimum_required(VERSION 3.16)

# project.cmake must be included before project() - it hooks/overrides the
# project() command itself. Reversing this order silently falls back to
# plain CMake (host gcc instead of the xtensa cross-compiler, no IDF project
# machinery), which is what caused the first CI run to fail.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32-surveillance-firmware)
