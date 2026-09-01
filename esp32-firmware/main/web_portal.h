#pragma once

#include <esp_err.h>

/**
 * Start the config portal httpd on port 80: GET / serves the setup form,
 * POST /save validates and stores the submitted config (valid -> success
 * page + reboot, invalid -> form re-served silently). Runs on the httpd
 * internal task; reboot is the only exit, so there is no stop function.
 */
esp_err_t web_portal_start(void);
