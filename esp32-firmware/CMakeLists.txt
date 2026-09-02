cmake_minimum_required(VERSION 3.16)

# project.cmake must be included before project() - it hooks/overrides the
# project() command itself. Reversing this order silently falls back to
# plain CMake (host gcc instead of the xtensa cross-compiler, no IDF project
# machinery), which is what caused the first CI run to fail.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32-surveillance-firmware)

# esp_peer 1.5.4's prebuilt libpeer_default.a is built against IDF v6, so the
# whole project is pinned to ESP-IDF v6 instead of carrying a v5 compatibility
# shim (also enforced via `idf: ">=6.0"` in main/idf_component.yml). Fail at
# configure time with a clear message rather than an obscure link error deep
# inside esp_peer.
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_LESS "6.0")
    message(FATAL_ERROR "ESP-IDF v6 or later is required (found ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}): esp_peer's prebuilt library is built against IDF v6.")
endif()
