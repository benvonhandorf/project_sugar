#include "app_control.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include "weight.h"

static const char *TAG = "control";

#define TOPIC_CONTROL  "control"
#define TOPIC_WATCHDOG "watchdog"

#define TICK_MS 1000
#define TASK_STACK 3072
#define TASK_PRIO 2

/* Long enough for a reply to leave before the reset cuts the connection. */
#define RESTART_DELAY_MS 500

/* A baseline is baseline_samples samples, each average_samples conversions: at
 * the default 10 SPS that is 2.5 s, and a slower rate has to fit too. */
#define REQUEST_TIMEOUT_MS 30000

static app_config_t s_cfg;
static volatile int64_t s_deadline_us; /* 0 = not armed */
static volatile bool s_armed;
static volatile uint32_t s_pings;
static volatile uint32_t s_commands;
static volatile uint32_t s_unknown;
static volatile bool s_restart_requested;
static volatile bool s_rebaseline_requested;

static void arm(void)
{
    s_deadline_us = esp_timer_get_time() + (int64_t)s_cfg.watchdog.external_ms * 1000;
    s_armed = true;
}

/*
 * Both handlers run on the MQTT client's task, which is also what services
 * keepalives, so neither does anything slow. A restart is a flag the watchdog
 * task acts on: calling esp_restart() from here would tear the client down
 * from inside its own callback.
 */
static void on_watchdog(const char *topic, size_t topic_len, const char *data,
                        size_t data_len, void *ctx)
{
    (void)topic; (void)topic_len; (void)data; (void)data_len; (void)ctx;
    s_pings++;
    if (s_cfg.watchdog.external_ms > 0) {
        arm();
    }
}

static void on_control(const char *topic, size_t topic_len, const char *data,
                       size_t data_len, void *ctx)
{
    (void)topic; (void)topic_len; (void)ctx;

    /* esp-mqtt delivers neither topic nor payload NUL-terminated. */
    char command[32];
    const size_t n = data_len < sizeof(command) - 1 ? data_len : sizeof(command) - 1;
    memcpy(command, data, n);
    command[n] = '\0';

    if (strcmp(command, "reset") == 0) {
        s_commands++;
        ESP_LOGW(TAG, "restart requested over MQTT");
        s_restart_requested = true;
        return;
    }
    if (strcmp(command, "rebaseline") == 0) {
        s_commands++;
        ESP_LOGI(TAG, "re-baseline requested over MQTT");
        /*
         * A flag, acted on by the watchdog task. A baseline takes several
         * seconds to acquire, and this handler runs on the MQTT client task --
         * the same task that sends keepalives, so blocking here would drop the
         * broker connection. That is a smaller version of exactly what the
         * predecessor did by servicing the client once per batch rather than
         * once per sample.
         */
        s_rebaseline_requested = true;
        return;
    }

    s_unknown++;
    ESP_LOGW(TAG, "unrecognised control command '%s'", command);
}

static void watchdog_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        if (s_restart_requested) {
            vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
            esp_restart();
        }

        if (s_rebaseline_requested) {
            s_rebaseline_requested = false;
            esp_err_t err = weight_rebaseline(REQUEST_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "re-baseline: %s", esp_err_to_name(err));
            }
        }

        if (!s_armed || s_cfg.watchdog.external_ms == 0) {
            continue;
        }
        if (esp_timer_get_time() < s_deadline_us) {
            continue;
        }

        ESP_LOGE(TAG, "no watchdog message for %lu ms; restarting",
                 (unsigned long)s_cfg.watchdog.external_ms);
        vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
        esp_restart();
    }
}

esp_err_t app_control_start(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;

    /*
     * The task watchdog. ESP-IDF may already have initialised it for the idle
     * tasks, in which case reconfiguring is the only way to change the timeout;
     * init would return ESP_ERR_INVALID_STATE and leave the wrong one in force.
     */
    if (s_cfg.watchdog.hardware_s > 0) {
        const esp_task_wdt_config_t wdt = {
            .timeout_ms = (uint32_t)s_cfg.watchdog.hardware_s * 1000,
            .idle_core_mask = 0,
            .trigger_panic = true,
        };
        esp_err_t err = esp_task_wdt_reconfigure(&wdt);
        if (err == ESP_ERR_INVALID_STATE) {
            err = esp_task_wdt_init(&wdt);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "task watchdog: %s", esp_err_to_name(err));
        }
    }

    /*
     * Both topics, on every connect. Subscribing to `control` alone -- the
     * predecessor's mistake -- makes the external watchdog un-resettable and
     * therefore a two-minute reboot timer. mqtt_manager re-applies the table on
     * each reconnect, because a broker keeps no subscription state for a client
     * that dropped.
     */
    esp_err_t err = mqtt_manager_subscribe(TOPIC_CONTROL, 1, on_control, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = mqtt_manager_subscribe(TOPIC_WATCHDOG, 1, on_watchdog, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (s_cfg.watchdog.external_ms > 0 && !s_cfg.watchdog.arm_on_first_message) {
        arm();
        ESP_LOGW(TAG, "external watchdog armed at boot: without a supervisor "
                      "publishing to %s this device will restart every %lu ms",
                 TOPIC_WATCHDOG, (unsigned long)s_cfg.watchdog.external_ms);
    }

    if (xTaskCreate(watchdog_task, "watchdog", TASK_STACK, NULL, TASK_PRIO, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_control_get_state(app_control_state_t *out)
{
    if (!out) {
        return;
    }
    out->armed = s_armed;
    out->remaining_ms =
        s_armed ? (s_deadline_us - esp_timer_get_time()) / 1000 : -1;
    out->pings = s_pings;
    out->commands = s_commands;
    out->unknown = s_unknown;
}
