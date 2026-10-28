#pragma once

#include "esp_err.h"

/* Register this project's HTTP routes. Call before or after http_server_start();
 * routes registered first are served as soon as it listens. */
esp_err_t app_http_register(void);
