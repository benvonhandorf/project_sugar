#pragma once

#include "esp_err.h"

/* Register this project's command groups with the shell. Call before cli_start()
 * so the banner and tab completion see them. */
esp_err_t app_console_register(void);
