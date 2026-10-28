#include "app_http.h"

#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "app_status.h"
#include "config_reader.h"
#include "config_store.h"
#include "esp_log.h"
#include "http_server.h"
#include "weight.h"

static const char *TAG = "app_http";

#define CONFIG_UPLOAD_MAX 4096
/* Long enough for the response to reach the client before the restart cuts the
 * connection. */
#define RESTART_DELAY_MS 750

static esp_err_t handle_status(httpd_req_t *req)
{
    static char buf[APP_STATUS_BUF];
    int len = app_status_serialize(buf, sizeof(buf));
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status too large");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    static char buf[CONFIG_UPLOAD_MAX];
    size_t len = 0;

    esp_err_t err = config_store_read(buf, sizeof(buf), &len, NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{}");
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not read the stored configuration");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > CONFIG_UPLOAD_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Configuration is empty or larger than the buffer");
        return ESP_OK;
    }

    static char buf[CONFIG_UPLOAD_MAX];
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
            return ESP_OK;
        }
        received += n;
    }

    /*
     * Parsed before it is stored, so a configuration that would not load cannot
     * replace one that does. The reason names the section at fault, because
     * "invalid config" over HTTP leaves the user editing blind.
     */
    char err_msg[192];
    esp_err_t err = config_reader_store(buf, (size_t)received, err_msg, sizeof(err_msg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rejected configuration: %s", err_msg);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "Configuration stored. Restarting.\n");

    /* Restarting inside the handler would drop the response, so the client would
     * learn the write succeeded only by never hearing back. */
    ESP_LOGI(TAG, "configuration replaced; restarting");
    config_store_schedule_restart(RESTART_DELAY_MS);
    return ESP_OK;
}

/*
 * The weight state on its own, for a dashboard that polls faster than the
 * status interval. Everything here is in `status` too; this route exists so
 * that polling it every second does not mean parsing forty fields.
 */
static esp_err_t handle_weight(httpd_req_t *req)
{
    weight_state_t w;
    weight_get_state(&w);

    char buf[320];
    int len = snprintf(buf, sizeof(buf),
                       "{\"weight_g\":%.3f,\"baseline_g\":%.3f,\"delta_g\":%.3f,"
                       "\"triggered\":%s,\"initialized\":%s,\"sensor_ok\":%s,"
                       "\"samples\":%llu,\"triggers\":%lu}",
                       w.weight_g, w.baseline_g, w.delta_g,
                       w.triggered ? "true" : "false",
                       w.initialized ? "true" : "false",
                       w.sensor_ok ? "true" : "false",
                       (unsigned long long)w.samples, (unsigned long)w.triggers);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Weight too large");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

static const http_route_t routes[] = {
    /* Open: a dashboard scraping status should not need credentials, and it
     * exposes nothing that is not already on the MQTT status topic. */
    {"/api/status", HTTP_GET,  handle_status,      NULL, false},
    {"/api/weight", HTTP_GET,  handle_weight,      NULL, false},
    /* Protected: the stored configuration contains the WiFi passphrase and the
     * broker password. */
    {"/api/config", HTTP_GET,  handle_config_get,  NULL, true},
    {"/api/config", HTTP_POST, handle_config_post, NULL, true},
};

esp_err_t app_http_register(void)
{
    return http_server_add_routes(routes, sizeof(routes) / sizeof(routes[0]));
}
