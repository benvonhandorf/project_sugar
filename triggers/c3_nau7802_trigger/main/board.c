#include "board.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "vin.h"

static const char *TAG = "board";

/* One address probe is one byte on the wire; this only has to be longer than a
 * clock stretch, and short enough that 112 of them are not a visible pause. */
#define I2C_PROBE_TIMEOUT_MS 50

static app_config_board_t s_cfg;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_nau_dev;
static i2c_master_dev_handle_t s_sht_dev;
static i2c_master_dev_handle_t s_ina_dev;

static nau7802_handle_t s_nau;
static sht4x_handle_t s_sht;
static ina237_handle_t s_ina;

static board_part_status_t s_nau_status;
static board_part_status_t s_sht_status;
static board_part_status_t s_ina_status;

static nau7802_bringup_report_t s_nau_report;
static ina237_config_report_t s_ina_report;

/*
 * Stage names.
 *
 * The drivers deliberately return an enumerator and no text -- a component that
 * formats prose can only be reused by projects that want that prose. So the
 * words are here, where the project can choose them.
 */
static const char *nau7802_stage_name(nau7802_stage_t stage)
{
    switch (stage) {
    case NAU7802_STAGE_NONE:          return "none";
    case NAU7802_STAGE_PROBE:         return "probe (reading DEVICE_REV)";
    case NAU7802_STAGE_RESET:         return "reset";
    case NAU7802_STAGE_POWER_DIGITAL: return "powering the digital section";
    case NAU7802_STAGE_POWER_READY:   return "waiting for PU_CTRL.PUR";
    case NAU7802_STAGE_SET_LDO:       return "setting the internal regulator";
    case NAU7802_STAGE_SET_GAIN:      return "setting the PGA gain";
    case NAU7802_STAGE_POWER_ANALOG:  return "powering the analog section";
    case NAU7802_STAGE_ADC_CTRL:      return "configuring the ADC (chopper)";
    case NAU7802_STAGE_CALIBRATE:     return "internal offset calibration";
    case NAU7802_STAGE_START:         return "starting conversions";
    case NAU7802_STAGE_SETTLE:        return "waiting for the filter to settle";
    }
    return "unknown";
}

static const char *ina237_stage_name(ina237_stage_t stage)
{
    switch (stage) {
    case INA237_STAGE_NONE:      return "none";
    case INA237_STAGE_PROBE:     return "probe (reading MANUFACTURER_ID)";
    case INA237_STAGE_IDENTIFY:  return "identify (answered, but not with \"TI\")";
    case INA237_STAGE_SHUNT_CAL: return "writing SHUNT_CAL";
    }
    return "unknown";
}

/* --------------------------------------------------------------- bring-up */

/* One I2C device on the shared bus. A failure here is logged and leaves the
 * handle NULL, which every driver reports as ESP_ERR_INVALID_STATE later --
 * so one part that cannot be added does not take the other two with it. */
static void add_device(uint8_t address, uint32_t hz, const char *what,
                       i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = hz,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c, &dev_cfg, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adding the %s at 0x%02x: %s", what, (unsigned)address,
                 esp_err_to_name(err));
        *out = NULL;
    }
}

esp_err_t board_init(const app_config_board_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Kept so a diagnostic can report the bus that failed. Reading the pins back
     * out of the configuration file would report the bus the file describes,
     * which is the same thing only when nothing has gone wrong. */
    s_cfg = *cfg;

    /*
     * The SHT4x and the INA237 have overlapping legal address ranges: 0x44-0x46
     * belongs to both. Two devices on one address is not a subtle fault -- the
     * bus is contended and both parts return nonsense -- but it is a very quiet
     * one, because each driver's own probe still sees something answering. Say
     * so here, once, in terms of the two configuration values that collide.
     *
     * An ina237_address of -1 means the part is not fitted, and cannot collide.
     */
    const bool ina_fitted = cfg->ina237_address >= 0;
    const bool address_clash = ina_fitted && cfg->sht4x_address == cfg->ina237_address;
    if (address_clash) {
        ESP_LOGE(TAG, "sht4x_address and ina237_address are both 0x%02x. "
                      "Two parts cannot share an address; readings from both "
                      "would be nonsense. Neither is started.",
                 (unsigned)cfg->sht4x_address);
    }

    /*
     * The I2C bus. A pin of -1 means the board does not have one, which is not
     * an error: the device still boots, still answers the console, and reports
     * all three parts as absent rather than pretending they might appear.
     */
    if (cfg->i2c_sda >= 0 && cfg->i2c_scl >= 0) {
        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = cfg->i2c_sda,
            .scl_io_num = cfg->i2c_scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus on sda %d scl %d: %s", cfg->i2c_sda,
                     cfg->i2c_scl, esp_err_to_name(err));
            s_i2c = NULL;
        }
    } else {
        ESP_LOGW(TAG, "no I2C pins configured; every sensor on this board is disabled");
    }

    if (s_i2c) {
        add_device(NAU7802_I2C_ADDRESS, cfg->i2c_hz, "NAU7802", &s_nau_dev);
        /* Both of the clashing parts are left without a device handle, rather
         * than arbitrarily starting the first one: which of the two is
         * misconfigured is not something the firmware can know. */
        if (!address_clash) {
            add_device(cfg->sht4x_address, cfg->i2c_hz, "SHT4x", &s_sht_dev);
            if (ina_fitted) {
                add_device((uint8_t)cfg->ina237_address, cfg->i2c_hz, "INA237", &s_ina_dev);
            }
        }
    }

    /*
     * The handles exist whether or not there is a bus. Every driver accepts a
     * NULL device and refuses with ESP_ERR_INVALID_STATE until one arrives,
     * which is what lets a status command describe a board that is not working.
     */
    const nau7802_config_t nau_cfg = {
        .dev = s_nau_dev,
        .drdy_gpio = cfg->nau7802_drdy,
    };
    esp_err_t err = nau7802_create(&nau_cfg, &s_nau);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "creating the NAU7802 handle: %s", esp_err_to_name(err));
    }

    const sht4x_config_t sht_cfg = {.dev = s_sht_dev};
    err = sht4x_create(&sht_cfg, &s_sht);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "creating the SHT4x handle: %s", esp_err_to_name(err));
    }

    /* The shunt is supplied to ina237_configure(), not here: nothing is
     * calibrated until the part has proved it is really an INA237. */
    const ina237_config_t ina_cfg = {.dev = s_ina_dev, .shunt_ohms = 0.0};
    err = ina237_create(&ina_cfg, &s_ina);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "creating the INA237 handle: %s", esp_err_to_name(err));
    }

    /* The input-voltage divider is not on the I2C bus, so it is brought up
     * whether or not there is one. A board with an INA237 has no use for it. */
    if (!ina_fitted) {
        vin_init(cfg);
    }

    return ESP_OK;
}

esp_err_t board_start_loadcell(const app_config_loadcell_t *cfg)
{
    s_nau_status.attempted = true;
    s_nau_status.present = false;
    s_nau_status.stage = NULL;

    if (!s_nau || !s_nau_dev) {
        s_nau_status.err = ESP_ERR_INVALID_STATE;
        s_nau_status.stage = "no I2C bus";
        return s_nau_status.err;
    }

    nau7802_gain_t gain = NAU7802_GAIN_128;
    if (nau7802_gain_from_value(cfg->gain, &gain) != ESP_OK) {
        s_nau_status.err = ESP_ERR_INVALID_ARG;
        s_nau_status.stage = "gain is not one the part implements";
        return s_nau_status.err;
    }
    nau7802_ldo_t ldo = NAU7802_LDO_3V3;
    if (nau7802_ldo_from_millivolts(cfg->ldo_mv, &ldo) != ESP_OK) {
        s_nau_status.err = ESP_ERR_INVALID_ARG;
        s_nau_status.stage = "ldo_mv is not one the part implements";
        return s_nau_status.err;
    }
    nau7802_rate_t rate = NAU7802_RATE_10SPS;
    if (nau7802_rate_from_sps(cfg->rate_sps, &rate) != ESP_OK) {
        s_nau_status.err = ESP_ERR_INVALID_ARG;
        s_nau_status.stage = "rate_sps is not one the part implements";
        return s_nau_status.err;
    }

    /*
     * No scale is installed here, and that is deliberate. This project keeps
     * the tare and the calibration factor itself, in NVS, because it has to
     * restore a *stored* zero at boot and the driver only offers a measured
     * one. Two copies of a calibration is one too many, so the driver's copy is
     * left empty and every conversion to grams happens in weight.c.
     */
    const nau7802_bringup_opts_t opts = {
        .use_internal_ldo = cfg->internal_ldo,
        .ldo = ldo,
        .set_gain = true,
        .gain = gain,
        .set_scale = false,
    };

    esp_err_t err = nau7802_bring_up(s_nau, &opts, &s_nau_report);
    if (err != ESP_OK) {
        s_nau_status.err = err;
        s_nau_status.stage = nau7802_stage_name(s_nau_report.failed_stage);
        ESP_LOGE(TAG, "NAU7802 bring-up failed at %s: %s", s_nau_status.stage,
                 esp_err_to_name(err));
        return err;
    }

    /* After bring-up, not as part of it: a rate change re-runs the device's own
     * calibration and discards the unsettled conversions, which is exactly what
     * bring-up just did, and doing it in the other order would leave the filter
     * settled at the wrong rate. */
    nau7802_change_report_t change;
    err = nau7802_set_rate(s_nau, rate, &change);
    if (err != ESP_OK) {
        s_nau_status.err = err;
        s_nau_status.stage = nau7802_stage_name(change.failed_stage);
        ESP_LOGE(TAG, "NAU7802 rate: %s", esp_err_to_name(err));
        return err;
    }

    s_nau_status.present = true;
    s_nau_status.err = ESP_OK;
    s_nau_status.stage = NULL;
    ESP_LOGI(TAG, "NAU7802 rev %u up: gain x%d, %d SPS, chopper %s, DRDY %d",
             (unsigned)s_nau_report.revision, nau7802_gain_value(s_nau_report.gain),
             nau7802_rate_sps(rate), s_nau_report.chopper_off ? "off" : "ON (bad)",
             s_nau_report.drdy_gpio);
    return ESP_OK;
}

esp_err_t board_start_environment(void)
{
    s_sht_status.attempted = true;
    s_sht_status.present = false;
    s_sht_status.stage = NULL;

    if (!s_sht || !s_sht_dev) {
        s_sht_status.err = ESP_ERR_INVALID_STATE;
        s_sht_status.stage = "no I2C bus";
        return s_sht_status.err;
    }

    /*
     * The SHT4x has no identity register, so a serial number that reads back
     * with valid CRCs is the available evidence that the part is really there
     * -- and it is good evidence, because the CRC would not survive an address
     * that answered by accident.
     */
    uint32_t serial = 0;
    sht4x_crc_error_t crc;
    esp_err_t err = sht4x_read_serial(s_sht, &serial, &crc);
    if (err != ESP_OK) {
        s_sht_status.err = err;
        s_sht_status.stage = (err == ESP_ERR_SHT4X_CRC) ? "serial number failed its CRC"
                                                        : "reading the serial number";
        ESP_LOGE(TAG, "SHT4x: %s", esp_err_to_name(err));
        return err;
    }

    s_sht_status.present = true;
    s_sht_status.err = ESP_OK;
    ESP_LOGI(TAG, "SHT4x up, serial %08lx", (unsigned long)serial);
    return ESP_OK;
}

esp_err_t board_start_power(const app_config_power_t *cfg)
{
    s_ina_status.attempted = true;
    s_ina_status.present = false;
    s_ina_status.stage = NULL;

    /* Not fitted is a fact about the board, not a fault: no error, no log. */
    if (s_cfg.ina237_address < 0) {
        s_ina_status.err = ESP_ERR_NOT_FOUND;
        s_ina_status.stage = "not fitted";
        return s_ina_status.err;
    }

    if (!s_ina || !s_ina_dev) {
        s_ina_status.err = ESP_ERR_INVALID_STATE;
        s_ina_status.stage = "no I2C device (no bus, or an address clash)";
        return s_ina_status.err;
    }

    /*
     * The shunt is the only thing to configure.
     *
     * The driver writes SHUNT_CAL and nothing else, leaving CONFIG and
     * ADC_CONFIG at their reset values -- which is what section 8.2.2.2 of the
     * datasheet prescribes, and has a useful side effect: the value it writes,
     * 4096, *is* SHUNT_CAL's reset value, so a part that browns out and resets
     * comes back configured exactly as this firmware configured it. There is
     * nothing to detect and nothing to repair.
     */
    esp_err_t err = ina237_configure(s_ina, cfg->shunt_ohms, &s_ina_report);
    if (err != ESP_OK) {
        s_ina_status.err = err;
        s_ina_status.stage = ina237_stage_name(s_ina_report.failed_stage);
        if (err == ESP_ERR_INA237_WRONG_PART) {
            ESP_LOGE(TAG, "INA237: something answered but MANUFACTURER_ID read "
                          "%04x, not TI. Check ina237_address.",
                     (unsigned)s_ina_report.manufacturer_id);
        } else {
            ESP_LOGE(TAG, "INA237 configure failed at %s: %s", s_ina_status.stage,
                     esp_err_to_name(err));
        }
        return err;
    }

    s_ina_status.present = true;
    s_ina_status.err = ESP_OK;
    ESP_LOGI(TAG, "INA237 up: %.4f ohm shunt, %.3f uA/bit, %.3f A full scale",
             cfg->shunt_ohms, s_ina_report.current_lsb * 1e6,
             s_ina_report.full_scale_amps);
    return ESP_OK;
}

/* ------------------------------------------------------------------- scan */

/*
 * Every part on this board NACKing is not three faults, it is one.
 *
 * A driver can only report what its own address did, so three failed bring-ups
 * read as three broken parts when the truth is that the bus is not there --
 * wrong pins, no pull-ups, or the sensor rail unpowered. A scan is the
 * measurement that separates those: an address that answers proves the bus
 * works and the configured address is wrong, and an empty scan proves the
 * opposite. Both are worth more than any amount of re-probing.
 */
esp_err_t board_i2c_scan(uint8_t *addrs, size_t max, size_t *found)
{
    if (!addrs || !found) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = 0;
    if (!s_i2c) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 0x08-0x77: the reserved addresses at either end are not probed, because a
     * general call or a 10-bit prefix is answered by parts that are not at that
     * address and would be reported as devices that do not exist. */
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_i2c, addr, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
            if (*found < max) {
                addrs[*found] = addr;
            }
            (*found)++;
        }
    }
    return ESP_OK;
}

/* -------------------------------------------------------------- accessors */

const app_config_board_t *board_config(void) { return &s_cfg; }


nau7802_handle_t board_loadcell(void) { return s_nau; }
sht4x_handle_t board_environment(void) { return s_sht; }
ina237_handle_t board_power(void) { return s_ina; }

const board_part_status_t *board_loadcell_status(void) { return &s_nau_status; }
const board_part_status_t *board_environment_status(void) { return &s_sht_status; }
const board_part_status_t *board_power_status(void) { return &s_ina_status; }

const nau7802_bringup_report_t *board_loadcell_report(void) { return &s_nau_report; }
const ina237_config_report_t *board_power_report(void) { return &s_ina_report; }
