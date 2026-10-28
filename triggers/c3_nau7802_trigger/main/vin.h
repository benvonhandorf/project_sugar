#pragma once

#include "config_reader.h"
#include "esp_err.h"

/*
 * The input voltage, measured through a resistor divider into an ADC pin.
 *
 * This is how a board without an INA237 reports bus_v. It measures voltage and
 * nothing else: a divider cannot say what the load is drawing, so no caller
 * should invent a current to go with it.
 */

/* Configure the ADC from the board section. ESP_ERR_NOT_FOUND when no pin is
 * configured, which is not a fault. Safe to call once; not re-entrant. */
esp_err_t vin_init(const app_config_board_t *cfg);

/* True once vin_init() has succeeded. */
bool vin_available(void);

/* The input voltage, in volts, averaged over a burst of conversions. */
esp_err_t vin_read(double *volts);
