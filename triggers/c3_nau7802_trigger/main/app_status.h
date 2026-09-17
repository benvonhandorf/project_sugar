#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The device's status message.
 *
 * This belongs to the project, not to a component: what a device reports is the
 * one thing no library can decide. mqtt_manager publishes bytes and has no
 * opinion about their shape.
 *
 * Keep it a flat object of scalars. That is the shape that survives being
 * graphed, alerted on and diffed -- and it is why the load cell's state, the
 * two sensors' last readings and the watchdog's countdown all appear here side
 * by side rather than in nested groups.
 *
 * It replaces two retained topics the predecessor published separately: a bare
 * "online"/"offline" string on `status`, and a bare integer reset code on
 * `reset`. Both are represented here; see docs/message_changes.md for what a
 * consumer has to change.
 */

/* Big enough for every field above, with room for a longer firmware name. */
#define APP_STATUS_BUF 1536

/* Serialise the current status as JSON. Returns the length written, or -1 if it
 * would not fit. */
int app_status_serialize(char *buf, size_t buf_size);

/* Publish the status to MQTT, if connected. Called on a timer and whenever
 * something changes. */
esp_err_t app_status_publish(void);

/*
 * Start the periodic publish.
 *
 * `feeder_id` is carried here rather than read back from the configuration
 * because the status message is where an unprovisioned device has to say so:
 * an id of 0 is reported as `provisioned: false` and logged as an error on
 * every interval.
 */
esp_err_t app_status_start(uint32_t interval_ms, uint16_t feeder_id);
