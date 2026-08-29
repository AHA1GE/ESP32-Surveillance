idf_component_register(
    SRCS "app_main.c" "wifi.c" "camera.c" "ws_stream.c" "ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp32-camera esp_websocket_client esp_http_client cjson
)

# Derive APP_VERSION from git describe, or fallback to "dev"
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    OUTPUT_VARIABLE GIT_VERSION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT GIT_VERSION)
    set(GIT_VERSION "dev")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DAPP_VERSION=\"${GIT_VERSION}\")
