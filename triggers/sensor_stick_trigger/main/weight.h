#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config_reader.h"
#include "esp_err.h"

/*
 * The weight signal chain: acquisition, baseline, and the trigger state machine.
 *
 * One task owns the load cell. Everything that has to talk to the converter --
 * a tare, a calibration, a re-baseline asked for over MQTT or the console --
 * goes through a request this task services between samples, so there is never
 * a second caller reading conversions out from underneath the filter.
 */

/* A snapshot for the status message and the console. Copied under a lock, so it
 * is internally consistent even though the sampling task is still running. */
typedef struct {
    bool sensor_ok;
    bool initialized;   /* the startup baseline has been acquired */
    bool triggered;
    double weight_g;    /* the most recent sample */
    double baseline_g;
    double delta_g;
    double tare_counts;
    double counts_per_gram;
    bool tare_from_nvs;
    bool factor_from_nvs;

    uint64_t samples;         /* loop iterations since boot */
    uint32_t triggers;        /* assertions since boot */
    uint32_t rebaselines;
    uint32_t triggered_readings; /* within the current event */
    uint32_t sensor_faults;
    double sample_rate_hz;    /* actual, derived from the configured rate */
    int64_t settled_ago_ms;   /* how long since the last settled sample */
} weight_state_t;

esp_err_t weight_start(const app_config_t *cfg);

/* Fill `out` with a consistent snapshot. */
void weight_get_state(weight_state_t *out);

/*
 * Operations that need the converter. Each blocks until the sampling task has
 * run it, or `timeout_ms` elapses.
 *
 * `weight_tare()` measures the unloaded zero and persists it. `weight_calibrate()`
 * derives counts-per-gram from a known mass placed after a tare and persists
 * that; it refuses when the move away from the tare is inside the noise of the
 * two averages, which is what stops a calibration taken with nothing on the pan
 * from being accepted. `weight_rebaseline()` re-acquires the baseline at
 * whatever the load is now -- the way back from a refill without a reboot.
 */
esp_err_t weight_tare(int samples, uint32_t timeout_ms, double *out_counts);
esp_err_t weight_calibrate(double known_grams, int samples, uint32_t timeout_ms,
                           double *out_counts_per_gram, double *out_precision_pct);
esp_err_t weight_rebaseline(uint32_t timeout_ms);

/* Install a factor measured elsewhere, persisting it. */
esp_err_t weight_set_factor(double counts_per_gram);
/* Drop the stored tare and factor; the configured defaults apply on restart. */
esp_err_t weight_forget_calibration(void);
