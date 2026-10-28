#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_reader.h"
#include "esp_err.h"
#include "ina237.h"
#include "nau7802.h"
#include "sht4x.h"

/*
 * The parts on this board, and the buses they hang off.
 *
 * Everything the drivers refuse to guess lives here: which pins, which address.
 * A driver takes a handle and never goes looking for one, so this is the only
 * file that knows all three parts share one I2C bus -- and the only file that
 * has to notice when two of them have been given the same address.
 *
 * Bring-up is separate from creation on purpose. A handle exists as soon as the
 * configuration is read, whether or not the part answers, so `i2c-nau7802
 * status` can say what is wrong with a board rather than failing to run.
 */

/* What happened when a part was brought up. One per part, kept so the console
 * and the status message can report it without re-probing. */
typedef struct {
    bool present;      /* the part answered and was configured */
    bool attempted;    /* bring-up has been tried at all */
    esp_err_t err;     /* the failure, when present is false */
    const char *stage; /* which step failed, in the driver's own words */
} board_part_status_t;

/*
 * Create the buses and the device handles.
 *
 * A pin set to -1 disables the bus and every part on it; those parts then
 * report `attempted` with ESP_ERR_INVALID_STATE rather than being silently
 * absent. Never fatal: a device with no sensors must still boot far enough to
 * be reconfigured.
 */
esp_err_t board_init(const app_config_board_t *cfg);

/* Reset, configure and start each part. Safe to re-run; that is how a part that
 * was not powered at boot is picked up later. */
esp_err_t board_start_loadcell(const app_config_loadcell_t *cfg);
esp_err_t board_start_environment(void);
esp_err_t board_start_power(const app_config_power_t *cfg);

/*
 * Probe every address on the bus, 0x08-0x77.
 *
 * Every part failing at its own probe looks like three dead parts and is almost
 * always one dead bus. This says which: an address that answers means the bus is
 * wired and an address is wrong; nothing answering means the pins, the pull-ups
 * or the sensor supply, and no amount of driver configuration will help.
 *
 * `found` receives the number of devices, which may exceed `max`; the first
 * `max` of them are written to `addrs`. ESP_ERR_INVALID_STATE if there is no bus.
 */
esp_err_t board_i2c_scan(uint8_t *addrs, size_t max, size_t *found);

/* What board_init() was actually given. */
const app_config_board_t *board_config(void);

/* The handles. NULL only if board_init() has not run. */
nau7802_handle_t board_loadcell(void);
sht4x_handle_t board_environment(void);
ina237_handle_t board_power(void);

const board_part_status_t *board_loadcell_status(void);
const board_part_status_t *board_environment_status(void);
const board_part_status_t *board_power_status(void);

/* The bring-up report for the load cell, for a console command that wants to
 * show the gain and chopper state the device actually came up in rather than
 * the ones it was asked for. */
const nau7802_bringup_report_t *board_loadcell_report(void);
const ina237_config_report_t *board_power_report(void);
