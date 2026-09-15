#include "sensors.h"

#include <stdio.h>
#include <string.h>

#include "app_time.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_manager.h"

static const char *TAG = "sensors";

#define TOPIC_ENVIRONMENT "environment"
#define TOPIC_POWER       "power"

#define TASK_STACK 3584
/* Below the sampling task: a conversion that is ready must not wait behind an
 * SHT4x measurement. */
#define TASK_PRIO 3

static environment_reading_t s_env;
static power_reading_t s_power;
static app_config_t s_cfg;

esp_err_t sensors_read_environment(environment_reading_t *out)
{
    sht4x_handle_t sht = board_environment();
    if (!sht || !board_environment_status()->present) {
        return ESP_ERR_INVALID_STATE;
    }

    sht4x_measurement_t m;
    esp_err_t err = sht4x_measure(sht, SHT4X_REPEATABILITY_HIGH, &m);
    if (err != ESP_OK) {
        s_env.failures++;
        s_env.valid = false;
        if (err == ESP_ERR_SHT4X_CRC) {
            ESP_LOGW(TAG, "SHT4x word %d failed its CRC (%02x, computed %02x)",
                     m.crc_error.word, m.crc_error.received, m.crc_error.computed);
        } else {
            ESP_LOGW(TAG, "SHT4x: %s", esp_err_to_name(err));
        }
        return err;
    }

    s_env.reads++;
    s_env.valid = true;
    s_env.temperature_c = m.temperature_c;
    /*
     * The cropped humidity, not the raw one. The conversion can land just
     * outside 0-100 %RH and the datasheet expects that to be clamped; a
     * consumer graphing 100.4 %RH would be reading a rounding artefact as
     * weather. The uncropped value is still worth having during bring-up, which
     * is why the driver reports both -- it is logged here rather than published.
     */
    s_env.humidity_pct = m.humidity_pct_cropped;
    if (m.humidity_was_cropped) {
        ESP_LOGD(TAG, "humidity cropped from %.2f %%RH", m.humidity_pct);
    }

    if (out) {
        *out = s_env;
    }

    char payload[160];
    int n = snprintf(payload, sizeof(payload),
                     "{\"ts\":%lld,\"temperature_c\":%.2f,\"humidity_pct\":%.2f}",
                     (long long)app_time_epoch_ms(), s_env.temperature_c,
                     s_env.humidity_pct);
    if (n > 0 && (size_t)n < sizeof(payload)) {
        mqtt_manager_publish(TOPIC_ENVIRONMENT, payload, (size_t)n, 0, false);
    }
    return ESP_OK;
}

esp_err_t sensors_read_power(power_reading_t *out)
{
    ina237_handle_t ina = board_power();
    if (!ina || !board_power_status()->present) {
        return ESP_ERR_INVALID_STATE;
    }

    ina237_reading_t r;
    esp_err_t err = ina237_read(ina, &r);
    if (err != ESP_OK) {
        s_power.failures++;
        s_power.valid = false;
        ESP_LOGW(TAG, "INA237: %s", esp_err_to_name(err));
        return err;
    }

    s_power.reads++;
    s_power.valid = true;
    s_power.bus_v = r.bus_v;
    s_power.current_a = r.current_a;
    s_power.power_w = r.power_w;
    s_power.temp_c = r.temp_c;
    /*
     * `shunt_cal_matches` is deliberately not part of this.
     *
     * It cannot mean what it looks like it means on this part: the driver
     * leaves CONFIG and ADC_CONFIG at their reset values and writes only
     * SHUNT_CAL, and the value it writes -- 4096 -- *is* that register's reset
     * value. So the flag reads true whether or not the part has been reset,
     * and a part that browns out comes back configured exactly as this
     * firmware configured it. There is nothing to detect, and nothing to
     * repair; treating the flag as a health signal would only add a condition
     * that is never false.
     */
    s_power.healthy = r.trim_checksum_ok && !r.math_overflow;

    if (!r.trim_checksum_ok) {
        ESP_LOGW(TAG, "INA237 trim memory checksum error; nothing it reports is trustworthy");
    }
    if (r.math_overflow) {
        ESP_LOGW(TAG, "INA237 math overflow; current and power are invalid, "
                      "though the bus voltage is still good");
    }

    if (out) {
        *out = s_power;
    }

    char payload[224];
    int n = snprintf(payload, sizeof(payload),
                     "{\"ts\":%lld,\"bus_v\":%.4f,\"current_a\":%.5f,"
                     "\"power_w\":%.5f,\"temperature_c\":%.2f,\"healthy\":%s}",
                     (long long)app_time_epoch_ms(), s_power.bus_v, s_power.current_a,
                     s_power.power_w, s_power.temp_c,
                     s_power.healthy ? "true" : "false");
    if (n > 0 && (size_t)n < sizeof(payload)) {
        mqtt_manager_publish(TOPIC_POWER, payload, (size_t)n, 0, false);
    }
    return ESP_OK;
}

/*
 * One task, two independent periods.
 *
 * Sleeping to the nearer of the two due times, rather than running a fixed tick,
 * keeps a disabled sensor from costing anything and keeps the two periods from
 * having to be multiples of each other.
 */
static void sensor_task(void *arg)
{
    (void)arg;

    const int64_t env_period_us = (int64_t)s_cfg.telemetry.environment_interval_s * 1000000;
    const int64_t power_period_us = (int64_t)s_cfg.telemetry.power_interval_s * 1000000;

    int64_t env_due = env_period_us ? esp_timer_get_time() : INT64_MAX;
    int64_t power_due = power_period_us ? esp_timer_get_time() : INT64_MAX;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        if (now >= env_due) {
            sensors_read_environment(NULL);
            env_due = now + env_period_us;
        }
        if (now >= power_due) {
            sensors_read_power(NULL);
            power_due = now + power_period_us;
        }

        int64_t next = env_due < power_due ? env_due : power_due;
        if (next == INT64_MAX) {
            /* Both disabled. The task stays alive so the console's `env read`
             * and `power read` still have somewhere to run from. */
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }
        int64_t wait_ms = (next - esp_timer_get_time()) / 1000;
        if (wait_ms < 10) {
            wait_ms = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t sensors_start(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;

    if (cfg->telemetry.environment_interval_s == 0 &&
        cfg->telemetry.power_interval_s == 0) {
        ESP_LOGW(TAG, "both sensor intervals are 0; nothing will be published");
    }

    if (xTaskCreate(sensor_task, "sensors", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const environment_reading_t *sensors_last_environment(void) { return &s_env; }
const power_reading_t *sensors_last_power(void) { return &s_power; }
