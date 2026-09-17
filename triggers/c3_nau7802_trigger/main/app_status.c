#include "app_status.h"

#include <stdio.h>
#include <string.h>

#include "app_control.h"
#include "app_time.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_manager.h"
#include "ntp_manager.h"
#include "publish_queue.h"
#include "sensors.h"
#include "weight.h"
#include "wifi_manager.h"

static const char *TAG = "status";

#define STATUS_TOPIC "status"

static esp_timer_handle_t s_timer;
static uint16_t s_feeder_id;
static esp_reset_reason_t s_reset_reason;

/*
 * The reset reason as a word rather than a number.
 *
 * The predecessor published the platform's integer code and nothing else, so
 * reading it needed the vendor's header alongside the dashboard. It also
 * captured the exception cause and then never published it at all.
 */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:   return "unknown";
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:  return "task-watchdog";
    case ESP_RST_WDT:       return "other-watchdog";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "other";
    }
}

static const char *part_state(const board_part_status_t *p)
{
    if (!p->attempted) { return "not-tried"; }
    return p->present ? "ok" : "failed";
}

int app_status_serialize(char *buf, size_t buf_size)
{
    const esp_app_desc_t *app = esp_app_get_description();

    char ip[16] = "";
    wifi_manager_get_address(ip, sizeof(ip));

    int8_t rssi = 0;
    wifi_manager_get_rssi(&rssi);

    weight_state_t w;
    weight_get_state(&w);

    app_control_state_t c;
    app_control_get_state(&c);

    const environment_reading_t *env = sensors_last_environment();
    const power_reading_t *pwr = sensors_last_power();

    /*
     * A flat object of scalars: the shape that survives being graphed, alerted
     * on and diffed. Nested groups here would each need their own dashboard
     * expression, and the one thing a status message is for is being watched by
     * something simple.
     *
     * This replaces two of the predecessor's retained topics -- a bare
     * "online"/"offline" string on `status` and a bare integer on `reset` --
     * with one document. See docs/message_changes.md: `online` still exists as
     * the last-will topic, and the reset reason is a field here.
     */
    int n = snprintf(buf, buf_size,
        "{"
        "\"ts\":%lld,"
        "\"firmware\":\"%s\","
        "\"version\":\"%s\","
        "\"feeder_id\":%u,"
        "\"provisioned\":%s,"
        "\"uptime_s\":%llu,"
        "\"reset_reason\":\"%s\","
        "\"heap_free\":%u,"
        "\"heap_min\":%u,"
        "\"ip\":\"%s\","
        "\"rssi\":%d,"
        "\"mqtt\":%s,"
        "\"time_synced\":%s,"
        "\"loadcell\":\"%s\","
        "\"sht4x\":\"%s\","
        "\"ina237\":\"%s\","
        "\"sensor_ok\":%s,"
        "\"initialized\":%s,"
        "\"triggered\":%s,"
        "\"weight_g\":%.3f,"
        "\"baseline_g\":%.3f,"
        "\"delta_g\":%.3f,"
        "\"counts_per_gram\":%.4f,"
        "\"tare_counts\":%.1f,"
        "\"calibration_source\":\"%s\","
        "\"sample_rate_hz\":%.3f,"
        "\"samples\":%llu,"
        "\"triggers\":%lu,"
        "\"rebaselines\":%lu,"
        "\"sensor_faults\":%lu,"
        "\"settled_ago_ms\":%lld,"
        "\"temperature_c\":%.2f,"
        "\"humidity_pct\":%.2f,"
        "\"bus_v\":%.4f,"
        "\"current_a\":%.5f,"
        "\"power_w\":%.5f,"
        "\"power_healthy\":%s,"
        "\"watchdog_armed\":%s,"
        "\"watchdog_remaining_ms\":%lld,"
        "\"watchdog_pings\":%lu"
        "}",
        (long long)app_time_epoch_ms(),
        app ? app->project_name : "?",
        app ? app->version : "?",
        (unsigned)s_feeder_id,
        s_feeder_id != 0 ? "true" : "false",
        (unsigned long long)(esp_timer_get_time() / 1000000),
        reset_reason_name(s_reset_reason),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        ip,
        rssi,
        mqtt_manager_is_connected() ? "true" : "false",
        ntp_manager_is_synced() ? "true" : "false",
        part_state(board_loadcell_status()),
        part_state(board_environment_status()),
        part_state(board_power_status()),
        w.sensor_ok ? "true" : "false",
        w.initialized ? "true" : "false",
        w.triggered ? "true" : "false",
        w.weight_g, w.baseline_g, w.delta_g,
        w.counts_per_gram, w.tare_counts,
        (w.factor_from_nvs || w.tare_from_nvs) ? "measured" : "configured",
        w.sample_rate_hz,
        (unsigned long long)w.samples,
        (unsigned long)w.triggers,
        (unsigned long)w.rebaselines,
        (unsigned long)w.sensor_faults,
        (long long)w.settled_ago_ms,
        env->valid ? env->temperature_c : 0.0,
        env->valid ? env->humidity_pct : 0.0,
        pwr->valid ? pwr->bus_v : 0.0,
        pwr->valid ? pwr->current_a : 0.0,
        pwr->valid ? pwr->power_w : 0.0,
        (pwr->valid && pwr->healthy) ? "true" : "false",
        c.armed ? "true" : "false",
        (long long)c.remaining_ms,
        (unsigned long)c.pings);

    if (n < 0 || (size_t)n >= buf_size) {
        return -1;
    }
    return n;
}

esp_err_t app_status_publish(void)
{
    static char buf[APP_STATUS_BUF];

    int len = app_status_serialize(buf, sizeof(buf));
    if (len < 0) {
        ESP_LOGW(TAG, "status does not fit in %d bytes", APP_STATUS_BUF);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Retained, so a subscriber that connects later sees the last state rather
     * than nothing until the next interval. Queued, not published directly:
     * this runs on esp_timer's single shared "Tmr Svc" task, and
     * mqtt_manager_publish() blocks on the client's API lock, which its
     * network task can hold for as long as a stalled reconnect -- wedging
     * every other esp_timer callback in the system along with this one. See
     * publish_queue.h. */
    return publish_queue_send(STATUS_TOPIC, buf, (size_t)len, 0, true);
}

static void on_timer(void *arg)
{
    (void)arg;

    if (s_feeder_id == 0) {
        /* Loud, and on every interval. An unprovisioned device is not broken,
         * but it is publishing nothing anyone is watching, and the one thing
         * that must not happen is for it to look fine. */
        ESP_LOGE(TAG, "feeder_id is 0: this device is unprovisioned and publishes "
                      "no measurements. Set it in config.json.");
    }

    /* Not connected is the normal case for most of a boot, so it is not worth
     * logging; publish() reports it and the next tick tries again. */
    app_status_publish();
}

esp_err_t app_status_start(uint32_t interval_ms, uint16_t feeder_id)
{
    if (s_timer) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Idempotent; see publish_queue.h. Called here too since app_status_publish()
     * must never reach mqtt_manager_publish() directly, regardless of start order. */
    esp_err_t queue_err = publish_queue_start();
    if (queue_err != ESP_OK) {
        return queue_err;
    }

    s_feeder_id = feeder_id;
    /* Captured here rather than at the point of use: esp_reset_reason() is
     * stable for the whole run, and reading it once means the status message
     * cannot accidentally report the reason for a reset that has not happened. */
    s_reset_reason = esp_reset_reason();

    const esp_timer_create_args_t args = {
        .callback = on_timer,
        .name = "status",
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_timer, (uint64_t)interval_ms * 1000);
}
