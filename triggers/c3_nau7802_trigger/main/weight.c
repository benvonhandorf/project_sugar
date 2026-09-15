#include "weight.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_time.h"
#include "batch.h"
#include "board.h"
#include "diag.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include "scale_store.h"
#include "trigger.h"
#include "wifi_manager.h"

static const char *TAG = "weight";

#define TOPIC_TRIGGER   "trigger"
#define TOPIC_AVERAGE   "average"
#define TOPIC_RAW_BATCH "raw_batch"
#define TOPIC_RSSI      "rssi_batch"
#define TOPIC_MESSAGE   "message"

#define TASK_STACK 5120
#define TASK_PRIO  5

/* The largest window the schema allows. Static, so a configuration change
 * cannot fail to allocate at the one moment it matters. */
#define MAX_WINDOW 64

/* ------------------------------------------------------------------ state */

typedef enum {
    OP_NONE = 0,
    OP_TARE,
    OP_CALIBRATE,
    OP_REBASELINE,
} op_kind_t;

static struct {
    op_kind_t kind;
    int samples;
    double known_grams;
    /* Filled by the task. */
    esp_err_t result;
    double out_counts;
    double out_counts_per_gram;
    double out_precision_pct;
} s_op;

static SemaphoreHandle_t s_op_request; /* guards s_op and serialises callers */
static SemaphoreHandle_t s_op_done;
static SemaphoreHandle_t s_state_lock;

static app_config_t s_cfg; /* a private copy: the task outlives app_main's */

static weight_state_t s_state;
static batch_t s_weight_batch;
static batch_t s_rssi_batch;

/* The sliding window of raw conversions, and the trimmed means being averaged. */
static int32_t s_window[MAX_WINDOW];
static uint8_t s_window_count;
static uint8_t s_window_head;

static trigger_state_t s_trigger;
static trigger_config_t s_trigger_cfg;

/* --------------------------------------------------------------- filtering */

static void window_push(int32_t value, uint8_t depth)
{
    s_window[s_window_head] = value;
    s_window_head = (uint8_t)((s_window_head + 1) % depth);
    if (s_window_count < depth) {
        s_window_count++;
    }
}

static int cmp_i32(const void *a, const void *b)
{
    const int32_t x = *(const int32_t *)a;
    const int32_t y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/*
 * The trimmed mean of the window.
 *
 * Discarding the extremes rather than averaging everything is what the
 * predecessor's amplifier driver did, and it is not interchangeable with a
 * plain mean: a load cell's worst noise is the occasional single-conversion
 * outlier, which a mean spreads over the whole window and a trim removes
 * outright.
 */
static bool window_trimmed_mean(uint8_t depth, uint8_t trim_low, uint8_t trim_high,
                                double *out)
{
    if (s_window_count < depth) {
        return false; /* still filling; there is no honest answer yet */
    }
    if ((int)trim_low + (int)trim_high >= (int)depth) {
        return false;
    }

    int32_t sorted[MAX_WINDOW];
    memcpy(sorted, s_window, (size_t)depth * sizeof(sorted[0]));
    qsort(sorted, depth, sizeof(sorted[0]), cmp_i32);

    const int first = trim_low;
    const int last = depth - trim_high; /* exclusive */
    double sum = 0.0;
    for (int i = first; i < last; i++) {
        sum += sorted[i];
    }
    *out = sum / (double)(last - first);
    return true;
}

/* ------------------------------------------------------------- publishing */

static void publish_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void publish_message(const char *fmt, ...)
{
    char text[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    diag_printf("%s\n", text);
    /* Fire and forget: a device that cannot reach the broker must not stall its
     * sampling loop saying so. */
    mqtt_manager_publish(TOPIC_MESSAGE, text, strlen(text), 0, false);
}

static void publish_trigger(bool asserted, double delta, double weight, double baseline)
{
    char payload[192];
    const int64_t ts = app_time_epoch_ms();

    /*
     * `value` is a JSON boolean.
     *
     * The predecessor emitted the integer 1 or 0 inside a JSON object while its
     * own documentation promised true/false and its only consumer compared the
     * whole payload to the string "true" -- three descriptions, no two of which
     * agreed, and a video trigger that could never fire. See
     * docs/message_changes.md; consumers have to be updated with this firmware.
     */
    int n = snprintf(payload, sizeof(payload),
                     "{\"ts\":%lld,\"value\":%s,\"delta_g\":%.2f,"
                     "\"weight_g\":%.3f,\"baseline_g\":%.3f}",
                     (long long)ts, asserted ? "true" : "false", delta, weight,
                     baseline);
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        return;
    }
    mqtt_manager_publish(TOPIC_TRIGGER, payload, (size_t)n, 0, false);
}

static void publish_average(double baseline)
{
    char payload[96];
    const int64_t ts = app_time_epoch_ms();
    int n = snprintf(payload, sizeof(payload),
                     "{\"ts\":%lld,\"baseline_g\":%.3f}", (long long)ts, baseline);
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        return;
    }
    mqtt_manager_publish(TOPIC_AVERAGE, payload, (size_t)n, 0, false);
}

/* ------------------------------------------------------------ acquisition */

static double counts_to_grams(double counts)
{
    /* A zero factor would make every weight infinite, silently. The schema's
     * minimum does not exclude it, and neither does a hand-edited NVS value. */
    if (s_state.counts_per_gram == 0.0 || !isfinite(s_state.counts_per_gram)) {
        return 0.0;
    }
    return (counts - s_state.tare_counts) / s_state.counts_per_gram;
}

/*
 * One sample: `average_samples` trimmed means, each taken after a new
 * conversion.
 *
 * The values being averaged come from a window that slides by one conversion at
 * a time, so they overlap heavily. That is the whole point and is easy to
 * "simplify" away: one sample is not an independent 90-conversion average, its
 * settling time is about 22 conversions, and a reimplementation that averaged
 * 90 conversions outright would be measurably slower to respond and would miss
 * short landings.
 */
static esp_err_t acquire_sample(double *out_counts)
{
    const app_config_loadcell_t *lc = &s_cfg.loadcell;
    double sum = 0.0;
    int taken = 0;

    while (taken < lc->average_samples) {
        int32_t raw = 0;
        esp_err_t err = nau7802_read_raw(board_loadcell(), &raw);
        if (err != ESP_OK) {
            return err;
        }
        window_push(raw, lc->window_conversions);

        double mean = 0.0;
        if (window_trimmed_mean(lc->window_conversions, lc->trim_low, lc->trim_high,
                                &mean)) {
            sum += mean;
            taken++;
        }
        /* Before the window is full there is nothing to average, so the
         * conversion is consumed and does not count. The first sample after a
         * start therefore costs window_conversions + average_samples - 1. */
    }

    *out_counts = sum / (double)taken;
    return ESP_OK;
}

static esp_err_t acquire_baseline(double *out_grams)
{
    double sum = 0.0;
    for (int i = 0; i < s_cfg.loadcell.baseline_samples; i++) {
        double counts = 0.0;
        esp_err_t err = acquire_sample(&counts);
        if (err != ESP_OK) {
            return err;
        }
        sum += counts_to_grams(counts);
    }
    *out_grams = sum / (double)s_cfg.loadcell.baseline_samples;
    return ESP_OK;
}

/* ----------------------------------------------------------- requested ops */

static void service_request(void)
{
    if (s_op.kind == OP_NONE) {
        return;
    }

    nau7802_handle_t nau = board_loadcell();

    switch (s_op.kind) {
    case OP_TARE: {
        nau7802_stats_t stats;
        s_op.result = nau7802_tare(nau, s_op.samples, &stats);
        if (s_op.result == ESP_OK) {
            const nau7802_scale_t *scale = nau7802_get_scale(nau);
            s_op.out_counts = scale->tare_counts;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.tare_counts = scale->tare_counts;
            s_state.tare_from_nvs = true;
            xSemaphoreGive(s_state_lock);
            s_op.result = scale_store_save_tare(scale->tare_counts);
            /* The window holds conversions taken before the tare moved; they
             * would be reported as a step change that never happened. */
            s_window_count = 0;
            s_window_head = 0;
        }
        break;
    }

    case OP_CALIBRATE: {
        nau7802_calibration_t result;
        s_op.result = nau7802_calibrate(nau, s_op.known_grams, s_op.samples, NULL,
                                        &result);
        if (s_op.result == ESP_OK) {
            s_op.out_counts_per_gram = result.counts_per_unit;
            s_op.out_precision_pct = result.precision_percent;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.counts_per_gram = result.counts_per_unit;
            s_state.factor_from_nvs = true;
            xSemaphoreGive(s_state_lock);
            s_op.result = scale_store_save_factor(result.counts_per_unit);
            s_window_count = 0;
            s_window_head = 0;
        } else {
            /* Filled on refusal too: a caller that was told the move was inside
             * the noise wants to know how big the move actually was. */
            s_op.out_counts_per_gram = result.net_counts;
            s_op.out_precision_pct = result.uncertainty;
        }
        break;
    }

    case OP_REBASELINE: {
        double baseline = 0.0;
        s_op.result = acquire_baseline(&baseline);
        if (s_op.result == ESP_OK) {
            const double weight = s_state.weight_g;
            const bool was_triggered = trigger_set_baseline(
                &s_trigger, baseline, esp_timer_get_time() / 1000);
            s_trigger.rebaselines++;

            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.baseline_g = baseline;
            s_state.initialized = true;
            s_state.triggered = false;
            s_state.triggered_readings = 0;
            s_state.rebaselines = s_trigger.rebaselines;
            xSemaphoreGive(s_state_lock);

            if (was_triggered) {
                publish_trigger(false, 0.0, weight, baseline);
            }
        }
        break;
    }

    case OP_NONE:
        break;
    }

    s_op.kind = OP_NONE;
    xSemaphoreGive(s_op_done);
}

/*
 * Hand a request to the sampling task and wait for it.
 *
 * The results are copied out before the request mutex is released, not read by
 * the caller afterwards: a second requester that got in between would otherwise
 * overwrite them, and the first caller would report someone else's calibration
 * as its own.
 */
static esp_err_t submit(op_kind_t kind, int samples, double known_grams,
                        uint32_t timeout_ms, double *out_a, double *out_b)
{
    if (!s_op_request) {
        return ESP_ERR_INVALID_STATE;
    }
    /* One requester at a time. The second one waits rather than corrupting the
     * first one's arguments halfway through. */
    if (xSemaphoreTake(s_op_request, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Drain a completion left over from a request that timed out: taking it as
     * this request's answer would report the previous one's result. */
    xSemaphoreTake(s_op_done, 0);

    s_op.samples = samples;
    s_op.known_grams = known_grams;
    s_op.result = ESP_FAIL;
    s_op.out_counts = 0.0;
    s_op.out_counts_per_gram = 0.0;
    s_op.out_precision_pct = 0.0;
    s_op.kind = kind;

    esp_err_t err;
    if (xSemaphoreTake(s_op_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* The task never got to it. Leaving the request set would make it run
         * long after the caller gave up. */
        s_op.kind = OP_NONE;
        err = ESP_ERR_TIMEOUT;
    } else {
        err = s_op.result;
        if (out_a) {
            *out_a = (kind == OP_TARE) ? s_op.out_counts : s_op.out_counts_per_gram;
        }
        if (out_b) {
            *out_b = s_op.out_precision_pct;
        }
    }

    xSemaphoreGive(s_op_request);
    return err;
}

esp_err_t weight_tare(int samples, uint32_t timeout_ms, double *out_counts)
{
    return submit(OP_TARE, samples, 0.0, timeout_ms, out_counts, NULL);
}

esp_err_t weight_calibrate(double known_grams, int samples, uint32_t timeout_ms,
                           double *out_counts_per_gram, double *out_precision_pct)
{
    return submit(OP_CALIBRATE, samples, known_grams, timeout_ms, out_counts_per_gram,
                  out_precision_pct);
}

esp_err_t weight_rebaseline(uint32_t timeout_ms)
{
    return submit(OP_REBASELINE, 0, 0.0, timeout_ms, NULL, NULL);
}

esp_err_t weight_set_factor(double counts_per_gram)
{
    if (counts_per_gram == 0.0 || !isfinite(counts_per_gram)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = scale_store_save_factor(counts_per_gram);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.counts_per_gram = counts_per_gram;
        s_state.factor_from_nvs = true;
        xSemaphoreGive(s_state_lock);
    }
    return err;
}

esp_err_t weight_forget_calibration(void)
{
    esp_err_t err = scale_store_forget();
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.tare_counts = s_cfg.loadcell.tare_counts;
        s_state.counts_per_gram = s_cfg.loadcell.calibration_factor;
        s_state.tare_from_nvs = false;
        s_state.factor_from_nvs = false;
        xSemaphoreGive(s_state_lock);
    }
    return err;
}

void weight_get_state(weight_state_t *out)
{
    if (!out) {
        return;
    }
    if (!s_state_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    *out = s_state;
    out->settled_ago_ms =
        s_trigger.initialized
            ? (esp_timer_get_time() / 1000) - s_trigger.last_settled_ms
            : -1;
    out->samples = s_trigger.iteration;
    xSemaphoreGive(s_state_lock);
}

/* -------------------------------------------------------------- the loop */

/*
 * Apply one step of the trigger state machine.
 *
 * The decisions live in trigger.c, which knows nothing about MQTT or FreeRTOS
 * and is tested off-target against synthetic traces; this is the part that acts
 * on them. Splitting it that way is what makes the acceptance tests in
 * REQUIREMENTS.md runnable at all -- none of them needs a bird, but all of them
 * need the branch structure.
 */
static void evaluate(double sample_g)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const trigger_action_t a =
        trigger_step(&s_trigger, &s_trigger_cfg, sample_g, now_ms);

    /* The published state, kept in step under the lock so a status reader never
     * sees a weight from one sample beside a baseline from another. */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.weight_g = sample_g;
    s_state.delta_g = a.delta_g;
    s_state.baseline_g = a.updated_baseline_g;
    s_state.triggered = s_trigger.triggered;
    s_state.triggered_readings = s_trigger.triggered_readings;
    s_state.triggers = s_trigger.triggers;
    s_state.rebaselines = s_trigger.rebaselines;
    xSemaphoreGive(s_state_lock);

    if (a.publish_assert) {
        publish_trigger(true, a.delta_g, sample_g, a.baseline_g);
    }
    if (a.publish_clear) {
        /* The settling delta, not the peak: the peak belongs to raw_batch, and
         * reporting it here would make two topics disagree about one event. */
        publish_trigger(false, a.delta_g, sample_g, a.baseline_g);
    }
    if (a.publish_average) {
        /* The updated baseline: `average` is the current estimate, not the one
         * this sample was compared against. */
        publish_average(a.updated_baseline_g);
    }

    if (!a.rebaseline) {
        return;
    }

    publish_message("re-baselining after %lu s outside the settled band "
                    "(weight %.3f g, baseline %.3f g)",
                    (unsigned long)(s_cfg.trigger.rebaseline_after_s),
                    sample_g, a.updated_baseline_g);

    double baseline = 0.0;
    if (acquire_baseline(&baseline) != ESP_OK) {
        /* Leave last_settled_ms where it is, so the next sample asks again
         * rather than waiting out another whole interval. */
        return;
    }

    const bool was_triggered =
        trigger_set_baseline(&s_trigger, baseline, esp_timer_get_time() / 1000);
    s_trigger.rebaselines++;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.baseline_g = baseline;
    s_state.triggered = false;
    s_state.triggered_readings = 0;
    s_state.initialized = true;
    s_state.rebaselines = s_trigger.rebaselines;
    xSemaphoreGive(s_state_lock);

    if (was_triggered) {
        publish_trigger(false, 0.0, sample_g, baseline);
    }
}

static void append_telemetry(double sample_g)
{
    char formatted[16];

    snprintf(formatted, sizeof(formatted), "%.3f", sample_g);
    batch_append(&s_weight_batch, formatted);

    /* One RSSI reading per iteration, at the same cadence as the weight, so the
     * two batches describe the same stretch of time. */
    int8_t rssi = 0;
    wifi_manager_get_rssi(&rssi);
    snprintf(formatted, sizeof(formatted), "%d", (int)rssi);
    batch_append(&s_rssi_batch, formatted);
}

static void sample_task(void *arg)
{
    (void)arg;

    if (s_cfg.watchdog.hardware_s > 0) {
        /* Subscribing rather than assuming: the task watchdog exists to catch a
         * sampling loop that has stopped, and nothing else in this firmware
         * blocks for as long as one conversion. */
        esp_task_wdt_add(NULL);
    }

    /*
     * Wait for the converter to produce anything at all.
     *
     * A bounded wait and then a report, not an infinite delay loop. The
     * predecessor called halt() here -- an unbounded delay that published
     * nothing and never recovered, so a device with a broken cell looked
     * exactly like a device with a flat battery.
     */
    const int64_t deadline =
        esp_timer_get_time() + (int64_t)s_cfg.loadcell.startup_timeout_ms * 1000;
    bool alive = false;
    while (esp_timer_get_time() < deadline) {
        int32_t raw = 0;
        if (nau7802_read_raw(board_loadcell(), &raw) == ESP_OK) {
            alive = true;
            break;
        }
        if (s_cfg.watchdog.hardware_s > 0) {
            esp_task_wdt_reset();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!alive) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.sensor_ok = false;
        s_state.sensor_faults++;
        xSemaphoreGive(s_state_lock);
        publish_message("load cell produced no conversions in %lu ms; restarting",
                        (unsigned long)s_cfg.loadcell.startup_timeout_ms);
        /* Long enough for the message and the log to leave. Restarting beats
         * sitting here: a device that reboots is one an operator can see
         * failing, and OTA still has a window on each boot. */
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.sensor_ok = true;
    xSemaphoreGive(s_state_lock);

    /* The startup baseline. Until this lands the trigger branch is disabled, so
     * the first samples -- taken while the filter is still settling -- cannot
     * manufacture an event. */
    double baseline = 0.0;
    if (acquire_baseline(&baseline) == ESP_OK) {
        trigger_set_baseline(&s_trigger, baseline, esp_timer_get_time() / 1000);
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.baseline_g = baseline;
        s_state.initialized = true;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG, "startup baseline %.3f g", baseline);
    }

    /* An initial cleared trigger, so a consumer that connects later and finds
     * nothing retained still knows the device is not mid-event. */
    publish_trigger(false, 0.0, baseline, baseline);

    for (;;) {
        if (s_cfg.watchdog.hardware_s > 0) {
            esp_task_wdt_reset();
        }

        /* Between samples, never during one: a tare taken while the filter is
         * mid-average would mix conversions from two different zeros. */
        service_request();

        double counts = 0.0;
        esp_err_t err = acquire_sample(&counts);
        if (err != ESP_OK) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_state.sensor_ok = false;
            s_state.sensor_faults++;
            xSemaphoreGive(s_state_lock);
            ESP_LOGW(TAG, "conversion failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.sensor_ok = true;
        xSemaphoreGive(s_state_lock);

        const double sample_g = counts_to_grams(counts);
        evaluate(sample_g);
        append_telemetry(sample_g);
    }
}

/* ---------------------------------------------------------------- startup */

esp_err_t weight_start(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->loadcell.window_conversions > MAX_WINDOW) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;

    s_state_lock = xSemaphoreCreateMutex();
    s_op_request = xSemaphoreCreateMutex();
    s_op_done = xSemaphoreCreateBinary();
    if (!s_state_lock || !s_op_request || !s_op_done) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The configured values are the factory defaults; anything this unit
     * measured for itself lives in NVS and wins. Seeding first and merging is
     * what makes `scale forget` a complete way back.
     */
    s_state.tare_counts = cfg->loadcell.tare_counts;
    s_state.counts_per_gram = cfg->loadcell.calibration_factor;
    scale_values_t stored = {
        .tare_counts = s_state.tare_counts,
        .counts_per_gram = s_state.counts_per_gram,
    };
    scale_store_load(&stored);
    s_state.tare_counts = stored.tare_counts;
    s_state.counts_per_gram = stored.counts_per_gram;
    s_state.tare_from_nvs = stored.tare_stored;
    s_state.factor_from_nvs = stored.factor_stored;

    s_trigger_cfg = (trigger_config_t){
        .min_g = cfg->trigger.min_g,
        .max_g = cfg->trigger.max_g,
        .hysteresis_g = cfg->trigger.hysteresis_g,
        .baseline_alpha = cfg->trigger.baseline_alpha,
        .reset_interval_samples = cfg->trigger.reset_interval_samples,
        .average_every = cfg->trigger.average_every,
        .rebaseline_after_ms = cfg->trigger.rebaseline_after_s * 1000u,
    };

    s_state.sample_rate_hz =
        (double)cfg->loadcell.rate_sps / (double)cfg->loadcell.average_samples;

    ESP_LOGI(TAG, "scale: %.4f counts/g (%s), tare %.1f counts (%s), %.2f samples/s",
             s_state.counts_per_gram, stored.factor_stored ? "measured" : "configured",
             s_state.tare_counts, stored.tare_stored ? "measured" : "configured",
             s_state.sample_rate_hz);

    esp_err_t err = batch_init(&s_weight_batch, TOPIC_RAW_BATCH, "g",
                               cfg->telemetry.batch_entries,
                               cfg->telemetry.batch_max_bytes,
                               cfg->telemetry.batch_drop_entries);
    if (err != ESP_OK) {
        return err;
    }
    err = batch_init(&s_rssi_batch, TOPIC_RSSI, "dBm", cfg->telemetry.batch_entries,
                     cfg->telemetry.batch_max_bytes, cfg->telemetry.batch_drop_entries);
    if (err != ESP_OK) {
        batch_free(&s_weight_batch);
        return err;
    }

    if (xTaskCreate(sample_task, "weight", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        batch_free(&s_weight_batch);
        batch_free(&s_rssi_batch);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
