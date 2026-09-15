#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Generated from config/config_schema.json by js2c_generate_sections(): the
 * top-level walker (app_config_t, json_parse_app_config_with_len) and an X-macro
 * over its sections. */
#include "app_config.h"
#include "app_config_sections.h"

/* Each component's own generated config type. There is exactly one definition of
 * each config concept, owned by the component that consumes it. */
#include "http_server_config.h"
#include "mqtt_manager_config.h"
#include "ntp_manager_config.h"
#include "wifi_manager_config.h"

/*
 * The whole device configuration.
 *
 * `top` holds the scalars and, for each section, a slice of the source JSON. The
 * sections themselves are parsed in place by the component that owns them, so
 * nothing is copied and no section's shape is described twice.
 */
typedef struct {
    app_config_t           top;
    wifi_manager_config_t  wifi;
    mqtt_manager_config_t  mqtt;
    ntp_manager_config_t   ntp;
    http_server_config_t   http;
} app_config_full_t;

/*
 * Read and parse the configuration.
 *
 * Tries /sdcard/config.json, then /res/config.json. A missing file is not an
 * error: every optional field carries a default from its owning component's
 * schema, so the device boots configurable rather than not at all.
 *
 * `err` receives a reason when parsing fails, naming the section at fault.
 */
esp_err_t config_reader_load(app_config_full_t *out, char *err, size_t err_len);

/*
 * Replace the stored configuration, keeping the previous copy.
 *
 * The bytes are parsed before being written, so a configuration that would not
 * load cannot replace one that does -- which matters when the only way back is a
 * cable.
 */
esp_err_t config_reader_store(const char *json, size_t len, char *err, size_t err_len);

/* Where the loaded configuration came from, for reporting. */
const char *config_reader_source(void);
