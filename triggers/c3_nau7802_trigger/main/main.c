#include <stdio.h>
#include <string.h>

#include "app_console.h"
#include "app_control.h"
#include "app_http.h"
#include "app_status.h"
#include "board.h"
#include "cli.h"
#include "cli_web.h"
#include "config_reader.h"
#include "diag.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "http_server.h"
#include "mdns_manager.h"
#include "mqtt_log_sink.h"
#include "mqtt_manager.h"
#include "net_events.h"
#include "nvs_flash.h"
#include "ntp_manager.h"
#include "ota.h"
#include "ota_http.h"
#include "scale_store.h"
#include "sensors.h"
#include "weight.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static app_config_full_t s_config;

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /*
         * A partition table change or a new NVS format. Erasing loses the
         * stored tare and calibration factor, which is a real cost -- but the
         * alternative is a device that will not boot, and the factory defaults
         * in config.json still produce a working, if uncalibrated, scale.
         */
        ESP_LOGW(TAG, "NVS is unusable; erasing. The stored calibration is lost.");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    /*
     * Bring-up order, and why.
     *
     * Nothing below waits for the network. Every service that needs
     * connectivity subscribes to NET_EVENT and starts itself when there is a
     * link, so this function is a list of "set this up", not a sequence of
     * "wait for that".
     *
     * What *is* ordered: output first, so failures are visible; configuration
     * next, because everything else is configured from it; the hardware before
     * the sampling task that uses it; and the MQTT subscriptions before the
     * broker connection can happen, so a `watchdog` message that arrives in the
     * first second is not missed.
     */

    /* 1. Output. First, so anything that fails after this says so on the console
     *    and, later, on the web console and MQTT. */
    ESP_ERROR_CHECK(diag_init(NULL));

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. Configuration. A missing or unreadable file is not fatal: schema
     *    defaults leave the device on its own access point with the console
     *    running, which is what a freshly flashed unit needs to be reconfigured. */
    char err_msg[192] = "";
    esp_err_t cfg_err = config_reader_load(&s_config, err_msg, sizeof(err_msg));
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "configuration: %s", err_msg);
        ESP_LOGE(TAG, "continuing with defaults; fix it over the console or /api/config");
    }

    if (s_config.top.feeder_id == 0) {
        /*
         * Loud, and repeated on every status interval. The predecessor fell back
         * to feeder 999 with a compile warning, which produced a device that
         * looked healthy and published to a topic nobody was watching.
         */
        ESP_LOGE(TAG, "feeder_id is 0: this device is unprovisioned. It will join "
                      "the network and answer the console, but its measurements "
                      "belong to no feeder. Set feeder_id in config.json.");
    }

    /* 3. The shell, before the network. A device that never joins anything must
     *    still be reachable over the serial port. */
    ESP_ERROR_CHECK(app_console_register());
    ESP_ERROR_CHECK(cli_start(NULL));

    /*
     * 4. The hardware, before anything that reads it.
     *
     * None of this is fatal. A board with a dead sensor still has to come up far
     * enough to be diagnosed and updated over the air -- a device that refuses
     * to boot because a part did not answer is a device that has to be taken
     * down from a tree.
     */
    ESP_ERROR_CHECK(scale_store_init());
    board_init(&s_config.top.board);
    board_start_loadcell(&s_config.top.loadcell);
    board_start_environment();
    board_start_power(&s_config.top.power);

    /*
     * Three parts that all failed are one bus that failed. Say which, once, in
     * the boot log -- the alternative is a log that reads as three broken
     * sensors and a console session to discover it was the pins all along.
     */
    if (!board_loadcell_status()->present && !board_environment_status()->present &&
        !board_power_status()->present) {
        uint8_t addrs[16];
        size_t found = 0;
        if (board_i2c_scan(addrs, sizeof(addrs), &found) == ESP_OK) {
            if (found == 0) {
                ESP_LOGE(TAG, "no part answered and no address answers at all: "
                              "the I2C bus (sda %d, scl %d) is the fault, not the "
                              "parts. Check the pins, the pull-ups and the sensor "
                              "supply; `i2c scan` re-runs this.",
                         s_config.top.board.i2c_sda, s_config.top.board.i2c_scl);
            } else {
                ESP_LOGE(TAG, "no part answered at its configured address, but %u "
                              "device(s) answer on the bus (first: 0x%02x). The bus "
                              "works; the addresses are wrong. See `i2c scan`.",
                         (unsigned)found, (unsigned)addrs[0]);
            }
        }
    }

    /* 5. Services. Each subscribes to NET_EVENT and does nothing until there is
     *    a link -- registration order does not matter and none of them blocks. */
    const mdns_manager_config_t mdns_cfg = {
        .hostname = s_config.top.device_name,
        .instance_name = s_config.top.instance_name,
    };
    ESP_ERROR_CHECK(mdns_manager_start(&mdns_cfg));
    ESP_ERROR_CHECK(ntp_manager_start(&s_config.ntp));

    if (s_config.mqtt.uri[0] != '\0') {
        ESP_ERROR_CHECK(mqtt_manager_start(&s_config.mqtt, s_config.top.device_name));
        /* Send the log to MQTT as well as the console. Dropped lines are counted,
         * not lost silently -- see mqtt_log_sink_dropped(). */
        mqtt_log_sink_start("log", 0);
    } else {
        ESP_LOGW(TAG, "no MQTT broker configured");
    }

    /* 6. The subscriptions and the watchdogs, before the link can come up. The
     *    table is re-applied on every reconnect, so registering early costs
     *    nothing and registering late risks missing the first message. */
    ESP_ERROR_CHECK(app_control_start(&s_config.top));

    /* 7. HTTP, its routes, and the OTA endpoint. Starting the server before
     *    registering routes would work too -- routes added later are registered
     *    as they arrive -- but registering first means they are all live the
     *    instant it listens. */
    ESP_ERROR_CHECK(app_http_register());
    ESP_ERROR_CHECK(ota_http_register(NULL));
    esp_err_t http_err = http_server_start(&s_config.http);
    if (http_err != ESP_OK) {
        /* Most likely HTTP_SERVER_ERR_NO_PASSWORD. Not fatal: the console still
         * works, and refusing to serve is the correct response to being asked to
         * serve an unauthenticated management interface. */
        ESP_LOGE(TAG, "HTTP server not started: %s", esp_err_to_name(http_err));
    } else {
        /* One server, not two: the web console shares it. Since
         * http_server-v0.2.0 the listener is open by the time start() returns --
         * httpd binds INADDR_ANY, which needs no address and no link -- so the
         * handle is valid here. It was NULL until the link came up before that,
         * and starting cli_web with a NULL handle makes it open its own httpd on
         * port 80, whose listener then made http_server's own fail with "address
         * in use". Hence the check rather than passing it blind. */
        httpd_handle_t server = http_server_handle();
        if (server) {
            const cli_web_config_t web = {
                .server = server,
                .page_uri = "/",
                .ws_uri = "/ws",
                .serve_page = true,
            };
            ESP_ERROR_CHECK(cli_web_start(&web));
        } else {
            ESP_LOGE(TAG, "HTTP server has no handle; the web console stays off "
                          "rather than opening a second listener on the same port.");
        }

        static const mdns_manager_service_t http_service = {
            .type = "_http", .proto = "_tcp", .port = 80,
        };
        mdns_manager_add_service(&http_service);
    }

    /*
     * 8. The measuring tasks.
     *
     * Started even when their parts failed to come up: the sampling task's own
     * timeout is what reports a dead load cell, and reporting it is more useful
     * than never starting. Both refuse quietly if their part is absent.
     */
    ESP_ERROR_CHECK(weight_start(&s_config.top));
    ESP_ERROR_CHECK(sensors_start(&s_config.top));

    /* 9. WiFi last. It is what makes NET_EVENT_LINK_UP happen, so everything
     *    that reacts to it is already listening by the time it can fire. */
    ESP_ERROR_CHECK(wifi_manager_init(&s_config.wifi));
    ESP_ERROR_CHECK(wifi_manager_start_station_mode());
    wifi_manager_scan_and_connect();

    ESP_ERROR_CHECK(app_status_start(s_config.top.telemetry.status_interval_s * 1000,
                                     s_config.top.feeder_id));

    /*
     * 10. Confirm the image, if this boot is an update on trial.
     *
     * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, firmware that boots and never
     * confirms itself is rolled back on the next reset. Doing it here, after
     * everything has been brought up, is the point: an image that crashes during
     * start-up never reaches this line and undoes itself. Move it earlier and the
     * rollback mechanism stops meaning anything.
     *
     * Deliberately not conditioned on the load cell answering. Rollback is for
     * firmware that does not run; a sensor that is unplugged is not a bad image,
     * and rolling back would take the fix away from whoever is holding the
     * board.
     */
    if (ota_pending_verify()) {
        ESP_LOGW(TAG, "this image is on trial; confirming it started cleanly");
        ota_mark_valid();
    }

    ESP_LOGI(TAG, "ready");
}
