#pragma once

#include <stdbool.h>

#include "config_reader.h"
#include "esp_err.h"

/*
 * The two sensors that are not the load cell: an SHT4x for the air the feeder
 * hangs in, and the supply: an INA237 for voltage, current and power where one is
 * fitted, otherwise the input voltage alone through a divider into an ADC pin.
 *
 * They are polled by their own small task, not on the sampling loop and not on
 * an esp_timer callback. Not the sampling loop because anything there costs a
 * fixed fraction of every sample -- an SHT4x high-repeatability measurement
 * blocks for about 9 ms, a fifth of a conversion at 10 SPS -- and neither
 * reading changes fast enough to be worth that. Not a timer callback because
 * those run on the shared esp_timer task, where 9 ms of blocking delays every
 * other timer in the firmware.
 */

typedef struct {
    bool valid;
    double temperature_c;
    double humidity_pct;
    uint32_t reads;
    uint32_t failures;
} environment_reading_t;

typedef struct {
    bool valid;
    double bus_v;
    /* Only meaningful when has_current. A divider measures voltage and nothing
     * else, so without an INA237 these are not zero -- they do not exist, and
     * neither the power topic nor the status message mentions them. */
    bool has_current;
    double current_a;
    double power_w;
    double temp_c;
    bool healthy; /* INA237: trim memory good and no math overflow. Divider: the read worked. */
    uint32_t reads;
    uint32_t failures;
} power_reading_t;

esp_err_t sensors_start(const app_config_t *cfg);

/* Read now and publish, whatever the timer is doing. Returns the sensor's own
 * error so a console command can report it. */
esp_err_t sensors_read_environment(environment_reading_t *out);
esp_err_t sensors_read_power(power_reading_t *out);

/* The most recent readings, for the status message. */
const environment_reading_t *sensors_last_environment(void);
const power_reading_t *sensors_last_power(void);
