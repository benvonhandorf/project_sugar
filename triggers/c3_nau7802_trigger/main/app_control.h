#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config_reader.h"
#include "esp_err.h"

/*
 * Remote control and liveness: the `control` and `watchdog` topics, the task
 * watchdog, and the external countdown.
 *
 * Two watchdogs, with different jobs. The task watchdog catches firmware that
 * has stopped running -- the sampling task subscribes to it, because nothing
 * else here blocks for as long as one conversion. The external countdown
 * catches firmware that is running and useless: it can only be reset by a
 * message from outside, so a device that is connected, publishing, and
 * measuring nothing still gets restarted.
 *
 * The countdown does not start until the first watchdog message arrives, unless
 * that is turned off. The predecessor started it at boot and then subscribed
 * only to `control`, so the message that would have reset it was never
 * delivered and the device rebooted every two minutes for its whole service
 * life -- which also meant its 257-sample raw batches, needing 128 s of wall
 * time, never once published.
 */

typedef struct {
    bool armed;
    int64_t remaining_ms; /* -1 when disarmed or disabled */
    uint32_t pings;       /* watchdog messages received */
    uint32_t commands;    /* control messages accepted */
    uint32_t unknown;     /* control messages not understood */
} app_control_state_t;

esp_err_t app_control_start(const app_config_t *cfg);

void app_control_get_state(app_control_state_t *out);
