#include "vin.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "vin";

/* The divider is high-impedance, so one conversion is noisy; the mean of a burst
 * is cheap (a few hundred microseconds) and read once a minute. */
#define VIN_SAMPLES 32

/* 12 dB reaches about 3.1 V at the pin, which a 6:1 divider turns into ~18.6 V. */
#define VIN_ATTEN ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static double s_ratio; /* (top + bottom) / bottom */

static adc_cali_handle_t make_cali(adc_unit_t unit, adc_channel_t chan)
{
    adc_cali_handle_t h = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit,
        .chan = chan,
        .atten = VIN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, &h) == ESP_OK) {
        return h;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t cfg = {
        .unit_id = unit,
        .atten = VIN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cfg, &h) == ESP_OK) {
        return h;
    }
#else
    (void)unit;
    (void)chan;
#endif
    return NULL;
}

esp_err_t vin_init(const app_config_board_t *cfg)
{
    if (!cfg || cfg->vin_adc_gpio < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (cfg->vin_divider_bottom_ohms <= 0 || cfg->vin_divider_top_ohms < 0) {
        ESP_LOGE(TAG, "divider resistors %.0f / %.0f ohms are not usable",
                 cfg->vin_divider_top_ohms, cfg->vin_divider_bottom_ohms);
        return ESP_ERR_INVALID_ARG;
    }

    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(cfg->vin_adc_gpio, &unit, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d is not an ADC pin: %s", cfg->vin_adc_gpio,
                 esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = unit};
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = VIN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }

    s_cali = make_cali(unit, s_chan);
    if (!s_cali) {
        ESP_LOGW(TAG, "no ADC calibration available; readings are uncorrected");
    }

    s_ratio = (cfg->vin_divider_top_ohms + cfg->vin_divider_bottom_ohms) /
              cfg->vin_divider_bottom_ohms;
    ESP_LOGI(TAG, "input voltage on GPIO %d, divider %.0f / %.0f ohms (x%.2f)",
             cfg->vin_adc_gpio, cfg->vin_divider_top_ohms,
             cfg->vin_divider_bottom_ohms, s_ratio);
    return ESP_OK;
}

bool vin_available(void) { return s_adc != NULL; }

esp_err_t vin_read(double *volts)
{
    if (!s_adc) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!volts) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t sum = 0;
    int good = 0;
    for (int i = 0; i < VIN_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) {
            sum += raw;
            good++;
        }
    }
    if (good == 0) {
        return ESP_FAIL;
    }

    const int mean = (int)(sum / good);
    int mv;
    if (s_cali) {
        esp_err_t err = adc_cali_raw_to_voltage(s_cali, mean, &mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        /* Nominal full scale at 12 dB, 12-bit. Good to a few percent at best. */
        mv = (mean * 3100) / 4095;
    }

    *volts = (mv / 1000.0) * s_ratio;
    return ESP_OK;
}
