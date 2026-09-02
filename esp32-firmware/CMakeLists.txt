cmake_minimum_required(VERSION 3.16)

# project.cmake must be included before project() - it hooks/overrides the
# project() command itself. Reversing this order silently falls back to
# plain CMake (host gcc instead of the xtensa cross-compiler, no IDF project
# machinery), which is what caused the first CI run to fail.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32-surveillance-firmware)

# esp_peer's prebuilt libpeer_default.a is built against IDF v6's log API and
# references esp_log(), which IDF v5.x does not define. The shim in
# main/esp_log_compat.c provides it, but nothing in the app calls esp_log
# directly, so the linker never extracts that object from libmain.a on its
# own (libmain.a precedes libpeer_default.a in link order). -u forces it.
target_link_options(${project_elf} PUBLIC "-Wl,-u,esp_log")
