#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * The load cell's tare and calibration factor, in NVS.
 *
 * These are the two numbers that cannot come from a build. The factor is a
 * property of the cell and the gain, measured once on a bench; the tare is the
 * bridge's own zero and moves with temperature, mounting and whatever is bolted
 * to the cell. Neither belongs in a firmware image, and neither survives being
 * kept only in config.json -- rewriting the whole configuration file to record
 * a tare would risk the file that also holds the WiFi credentials.
 *
 * So: config.json carries the factory defaults, NVS carries what this unit
 * measured, and NVS wins. `scale forget` drops the stored pair and the device
 * falls back to the configured one, which is the way back from a bad
 * calibration without a cable.
 */

typedef struct {
    double tare_counts;
    double counts_per_gram;
    bool tare_stored;   /* a tare has been measured and persisted */
    bool factor_stored; /* a factor has been measured and persisted */
} scale_values_t;

/* Open the namespace. Never fatal: a device with unreadable NVS still weighs,
 * using the configured defaults. */
esp_err_t scale_store_init(void);

/* Fill `out` from NVS, leaving any field that is not stored untouched -- so the
 * caller seeds it with the configured defaults first and gets a merge. */
esp_err_t scale_store_load(scale_values_t *out);

esp_err_t scale_store_save_tare(double tare_counts);
esp_err_t scale_store_save_factor(double counts_per_gram);

/* Drop both, so the configured defaults apply again after a restart. */
esp_err_t scale_store_forget(void);
