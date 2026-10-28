#include "scale_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "scale_store";

#define NAMESPACE  "loadcell"
#define KEY_TARE   "tare"
#define KEY_FACTOR "factor"

static bool s_ready;

/*
 * Doubles are stored as blobs rather than as a scaled integer.
 *
 * A tare is tens of thousands of counts with a meaningful fractional part -- it
 * is the mean of a few hundred conversions -- and a factor can be negative and
 * carries nine significant figures in the existing fleet. Rounding either to
 * store it would quietly change the calibration it was supposed to preserve.
 */
static esp_err_t get_double(nvs_handle_t h, const char *key, double *out, bool *found)
{
    size_t len = sizeof(double);
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *found = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(double)) {
        /* Written by a different build. Refusing is better than reinterpreting
         * bytes as a calibration. */
        ESP_LOGW(TAG, "%s is %u bytes, not %u; ignoring it", key, (unsigned)len,
                 (unsigned)sizeof(double));
        *found = false;
        return ESP_OK;
    }
    *found = true;
    return ESP_OK;
}

esp_err_t scale_store_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "opening NVS: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(h);
    s_ready = true;
    return ESP_OK;
}

esp_err_t scale_store_load(scale_values_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Nothing has ever been stored. The defaults the caller seeded stand. */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    double value = 0.0;
    bool found = false;
    if (get_double(h, KEY_TARE, &value, &found) == ESP_OK && found) {
        out->tare_counts = value;
        out->tare_stored = true;
    }
    if (get_double(h, KEY_FACTOR, &value, &found) == ESP_OK && found) {
        out->counts_per_gram = value;
        out->factor_stored = true;
    }

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_double(const char *key, double value)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, &value, sizeof(value));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t scale_store_save_tare(double tare_counts)
{
    return save_double(KEY_TARE, tare_counts);
}

esp_err_t scale_store_save_factor(double counts_per_gram)
{
    return save_double(KEY_FACTOR, counts_per_gram);
}

esp_err_t scale_store_forget(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* Not-found is success here: the caller asked for there to be nothing
     * stored, and there is nothing stored. */
    esp_err_t t = nvs_erase_key(h, KEY_TARE);
    esp_err_t f = nvs_erase_key(h, KEY_FACTOR);
    if (t == ESP_ERR_NVS_NOT_FOUND) { t = ESP_OK; }
    if (f == ESP_ERR_NVS_NOT_FOUND) { f = ESP_OK; }

    err = (t != ESP_OK) ? t : f;
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
