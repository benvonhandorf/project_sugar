#include "app_console.h"

#include <stdio.h>
#include <string.h>

#include "app_control.h"
#include "app_status.h"
#include "board.h"
#include "cli.h"
#include "config_reader.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "mqtt_manager.h"
#include "ota.h"
#include "sensors.h"
#include "weight.h"
#include "wifi_manager.h"

/*
 * Command groups are two tokens: "<group> <command>". A command receives
 * argv[0] == its own name, so `sys info` arrives here as argc=1, argv={"info"}.
 *
 * Never printf() from a command -- diag_printf() reaches the serial port and the
 * web console alike. Report failures with diag_error(), which prefixes "ERR: " so
 * a host script can tell success from failure without parsing prose, and return
 * -1; return 0 on success.
 */

/* Long enough for a calibration of several hundred conversions at 10 SPS. */
#define OP_TIMEOUT_MS 120000

/* ------------------------------------------------------------------- sys */

static int cmd_sys_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_app_desc_t *app = esp_app_get_description();

    diag_printf("firmware:  %s %s\n", app->project_name, app->version);
    diag_printf("built:     %s %s\n", app->date, app->time);
    diag_printf("idf:       %s\n", app->idf_ver);
    diag_printf("heap free: %u (min %u)\n",
                (unsigned)esp_get_free_heap_size(),
                (unsigned)esp_get_minimum_free_heap_size());
    diag_printf("config:    %s\n", config_reader_source());
    return 0;
}

static int cmd_sys_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char buf[APP_STATUS_BUF];
    if (app_status_serialize(buf, sizeof(buf)) < 0) {
        diag_error("status does not fit");
        return -1;
    }
    diag_printf("%s\n", buf);
    return 0;
}

static int cmd_sys_restart(int argc, char **argv)
{
    (void)argc; (void)argv;
    diag_printf("restarting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

static int cmd_sys_confirm(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!ota_pending_verify()) {
        diag_printf("nothing to confirm; this image is already marked good\n");
        return 0;
    }
    esp_err_t err = ota_mark_valid();
    if (err != ESP_OK) {
        diag_error("confirming this image: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("image confirmed; it will not be rolled back\n");
    return 0;
}

static int cmd_sys_rollback(int argc, char **argv)
{
    (void)argc; (void)argv;
    diag_printf("rolling back to the previous firmware\n");
    fflush(stdout);
    ota_rollback_and_reboot();
    /* Only reached if there is nothing to roll back to. */
    diag_error("no previous firmware to roll back to");
    return -1;
}

static const cli_command_t sys_commands[] = {
    {"info",     NULL, "Firmware, build and heap",            cmd_sys_info},
    {"status",   NULL, "The status message, as published",    cmd_sys_status},
    {"restart",  NULL, "Restart the device",                  cmd_sys_restart},
    {"confirm",  NULL, "Mark this image good, cancelling rollback", cmd_sys_confirm},
    {"rollback", NULL, "Return to the previous firmware",     cmd_sys_rollback},
};

static const cli_group_t sys_group = {
    .name = "sys",
    .help = "Device information and control",
    .commands = sys_commands,
    .command_count = sizeof(sys_commands) / sizeof(sys_commands[0]),
};

/* ------------------------------------------------------------------- net */

static int cmd_net_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    char ip[16] = "";
    int8_t rssi = 0;

    wifi_manager_get_address(ip, sizeof(ip));
    wifi_manager_get_rssi(&rssi);

    diag_printf("wifi:  state %d, ip %s, rssi %d\n",
                (int)wifi_manager_get_state(), ip[0] ? ip : "-", rssi);
    diag_printf("mqtt:  %s", mqtt_manager_is_connected() ? "connected" : "disconnected");
    if (mqtt_manager_topic_prefix()) {
        diag_printf(" under %s", mqtt_manager_topic_prefix());
    }
    diag_printf("\n");

    app_control_state_t c;
    app_control_get_state(&c);
    if (!c.armed) {
        diag_printf("wdog:  disarmed (waiting for the first watchdog message)\n");
    } else {
        diag_printf("wdog:  armed, %lld ms remaining\n", (long long)c.remaining_ms);
    }
    diag_printf("       %lu pings, %lu commands, %lu unrecognised\n",
                (unsigned long)c.pings, (unsigned long)c.commands,
                (unsigned long)c.unknown);
    return 0;
}

static int cmd_net_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = wifi_manager_scan_and_connect();
    if (err != ESP_OK) {
        diag_error("scan: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("scanning; results arrive asynchronously\n");
    return 0;
}

static int cmd_net_ap(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = wifi_manager_start_ap_mode();
    if (err != ESP_OK) {
        diag_error("starting the access point: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static const cli_command_t net_commands[] = {
    {"status", NULL, "WiFi, MQTT and watchdog state", cmd_net_status},
    {"scan",   NULL, "Scan and join the best known network", cmd_net_scan},
    {"ap",     NULL, "Serve the configuration access point", cmd_net_ap},
};

static const cli_group_t net_group = {
    .name = "net",
    .help = "Network state and control",
    .commands = net_commands,
    .command_count = sizeof(net_commands) / sizeof(net_commands[0]),
};

/* ----------------------------------------------------------------- scale */

static int cmd_scale_show(int argc, char **argv)
{
    (void)argc; (void)argv;
    weight_state_t w;
    weight_get_state(&w);

    const board_part_status_t *p = board_loadcell_status();
    if (!p->present) {
        diag_printf("load cell: %s%s%s\n", p->attempted ? "failed" : "not started",
                    p->stage ? " at " : "", p->stage ? p->stage : "");
    } else {
        const nau7802_bringup_report_t *r = board_loadcell_report();
        diag_printf("load cell: NAU7802 rev %u, gain x%d, chopper %s, DRDY %d\n",
                    (unsigned)r->revision, nau7802_gain_value(r->gain),
                    r->chopper_off ? "off" : "ON -- costs about six bits",
                    r->drdy_gpio);
    }

    diag_printf("factor:    %.4f counts/g (%s)\n", w.counts_per_gram,
                w.factor_from_nvs ? "measured, in NVS" : "from config.json");
    diag_printf("tare:      %.1f counts (%s)\n", w.tare_counts,
                w.tare_from_nvs ? "measured, in NVS" : "from config.json");
    diag_printf("weight:    %.3f g, baseline %.3f g, delta %.3f g\n",
                w.weight_g, w.baseline_g, w.delta_g);
    diag_printf("state:     %s, %s, %lu triggers, %lu re-baselines\n",
                w.initialized ? "initialized" : "acquiring the baseline",
                w.triggered ? "TRIGGERED" : "settled",
                (unsigned long)w.triggers, (unsigned long)w.rebaselines);
    diag_printf("rate:      %.3f samples/s, %llu samples, %lu sensor faults\n",
                w.sample_rate_hz, (unsigned long long)w.samples,
                (unsigned long)w.sensor_faults);
    return 0;
}

static int cmd_scale_tare(int argc, char **argv)
{
    int samples = 64;
    if (argc > 1 && cli_parse_int_arg(argv[1], &samples) < 0) {
        diag_error("samples must be a number");
        return -1;
    }
    if (samples < 2) {
        /* One conversion has no spread, so no honest uncertainty can be formed
         * from it -- and the calibration that follows needs one. */
        diag_error("at least 2 samples: a single conversion has no spread");
        return -1;
    }

    diag_printf("taring over %d conversions; leave the cell unloaded\n", samples);
    double counts = 0.0;
    esp_err_t err = weight_tare(samples, OP_TIMEOUT_MS, &counts);
    if (err != ESP_OK) {
        diag_error("tare: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("tare %.1f counts, stored\n", counts);
    return 0;
}

static int cmd_scale_calibrate(int argc, char **argv)
{
    if (argc < 2) {
        diag_error("usage: scale calibrate <grams> [samples]");
        return -1;
    }

    double grams = 0.0;
    if (cli_parse_double_arg(argv[1], &grams) < 0 || grams == 0.0) {
        diag_error("the known mass must be a non-zero number of grams");
        return -1;
    }
    int samples = 64;
    if (argc > 2 && cli_parse_int_arg(argv[2], &samples) < 0) {
        diag_error("samples must be a number");
        return -1;
    }

    diag_printf("calibrating against %.3f g over %d conversions\n", grams, samples);
    double factor = 0.0, precision = 0.0;
    esp_err_t err = weight_calibrate(grams, samples, OP_TIMEOUT_MS, &factor, &precision);

    if (err == ESP_ERR_NAU7802_WITHIN_NOISE) {
        /* On refusal the two outputs carry the move and its uncertainty rather
         * than a factor, which is the number that explains the refusal. */
        diag_error("the move away from the tare was %.1f counts against an "
                   "uncertainty of %.1f; place the mass, or use more samples",
                   factor, precision);
        return -1;
    }
    if (err == ESP_ERR_NAU7802_TOO_FEW_SAMPLES) {
        diag_error("run 'scale tare' with at least 2 samples first");
        return -1;
    }
    if (err != ESP_OK) {
        diag_error("calibrate: %s", esp_err_to_name(err));
        return -1;
    }

    diag_printf("%.6f counts/g to %.3f%%, stored\n", factor, precision);
    return 0;
}

static int cmd_scale_factor(int argc, char **argv)
{
    if (argc < 2) {
        diag_error("usage: scale factor <counts per gram>");
        return -1;
    }
    double factor = 0.0;
    if (cli_parse_double_arg(argv[1], &factor) < 0) {
        diag_error("the factor must be a number");
        return -1;
    }
    /* Negative is legitimate -- a cell wired the other way round calibrates to
     * one -- so only zero is rejected. */
    esp_err_t err = weight_set_factor(factor);
    if (err != ESP_OK) {
        diag_error("storing the factor: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("%.6f counts/g stored\n", factor);
    return 0;
}

static int cmd_scale_forget(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_err_t err = weight_forget_calibration();
    if (err != ESP_OK) {
        diag_error("forgetting the calibration: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("stored tare and factor dropped; config.json values now apply\n");
    return 0;
}

static int cmd_scale_rebaseline(int argc, char **argv)
{
    (void)argc; (void)argv;
    diag_printf("re-acquiring the baseline at the current load\n");
    esp_err_t err = weight_rebaseline(OP_TIMEOUT_MS);
    if (err != ESP_OK) {
        diag_error("re-baseline: %s", esp_err_to_name(err));
        return -1;
    }
    weight_state_t w;
    weight_get_state(&w);
    diag_printf("baseline %.3f g\n", w.baseline_g);
    return 0;
}

static const cli_command_t scale_commands[] = {
    {"show",       NULL,                     "Calibration, weight and trigger state", cmd_scale_show},
    {"tare",       "[samples]",              "Capture the unloaded zero and store it", cmd_scale_tare},
    {"calibrate",  "<grams> [samples]",      "Derive counts/g from a known mass and store it", cmd_scale_calibrate},
    {"factor",     "<counts per gram>",      "Store a factor measured elsewhere", cmd_scale_factor},
    {"forget",     NULL,                     "Drop the stored tare and factor", cmd_scale_forget},
    {"rebaseline", NULL,                     "Re-acquire the baseline at the current load", cmd_scale_rebaseline},
};

static const cli_group_t scale_group = {
    .name = "scale",
    .help = "Load cell calibration and weight",
    .commands = scale_commands,
    .command_count = sizeof(scale_commands) / sizeof(scale_commands[0]),
};

/* --------------------------------------------------------------- sensors */

static int cmd_sensor_env(int argc, char **argv)
{
    (void)argc; (void)argv;
    environment_reading_t r;
    esp_err_t err = sensors_read_environment(&r);
    if (err == ESP_ERR_INVALID_STATE) {
        const board_part_status_t *p = board_environment_status();
        diag_error("SHT4x is not available: %s", p->stage ? p->stage : "not started");
        return -1;
    }
    if (err != ESP_OK) {
        diag_error("SHT4x: %s", esp_err_to_name(err));
        return -1;
    }
    diag_printf("%.2f C, %.2f %%RH (%lu reads, %lu failures)\n", r.temperature_c,
                r.humidity_pct, (unsigned long)r.reads, (unsigned long)r.failures);
    return 0;
}

static int cmd_sensor_power(int argc, char **argv)
{
    (void)argc; (void)argv;
    power_reading_t r;
    esp_err_t err = sensors_read_power(&r);
    if (err == ESP_ERR_INVALID_STATE) {
        const board_part_status_t *p = board_power_status();
        diag_error("INA237 is not available: %s", p->stage ? p->stage : "not started");
        return -1;
    }
    if (err != ESP_OK) {
        diag_error("INA237: %s", esp_err_to_name(err));
        return -1;
    }

    const ina237_config_report_t *cr = board_power_report();
    diag_printf("%.4f V, %.5f A, %.5f W, die %.2f C%s\n", r.bus_v, r.current_a,
                r.power_w, r.temp_c, r.healthy ? "" : "  [UNHEALTHY]");
    diag_printf("%.3f uA/bit, %.3f A full scale, %lu reads, %lu failures\n",
                cr->current_lsb * 1e6, cr->full_scale_amps, (unsigned long)r.reads,
                (unsigned long)r.failures);
    return 0;
}

static const cli_command_t sensor_commands[] = {
    {"env",   NULL, "Read and publish the SHT4x now",  cmd_sensor_env},
    {"power", NULL, "Read and publish the INA237 now", cmd_sensor_power},
};

static const cli_group_t sensor_group = {
    .name = "sensor",
    .help = "The temperature, humidity and power monitors",
    .commands = sensor_commands,
    .command_count = sizeof(sensor_commands) / sizeof(sensor_commands[0]),
};

/* ------------------------------------------------------------------- i2c */

/* What each answering address is, when it is one of ours. Anything else is
 * printed bare: an unexpected device is a finding, not an error. */
static const char *i2c_what(uint8_t addr)
{
    const app_config_board_t *b = board_config();
    if (addr == NAU7802_I2C_ADDRESS) {
        return "NAU7802";
    }
    if (addr == b->sht4x_address) {
        return "SHT4x";
    }
    if (addr == b->ina237_address) {
        return "INA237";
    }
    return "unexpected";
}

static int cmd_i2c_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    const app_config_board_t *b = board_config();

    diag_printf("bus:      sda %d, scl %d, %lu Hz\n", b->i2c_sda, b->i2c_scl,
                (unsigned long)b->i2c_hz);
    diag_printf("expected: 0x%02x NAU7802, 0x%02x SHT4x, 0x%02x INA237\n",
                (unsigned)NAU7802_I2C_ADDRESS, (unsigned)b->sht4x_address,
                (unsigned)b->ina237_address);

    uint8_t addrs[16];
    size_t found = 0;
    esp_err_t err = board_i2c_scan(addrs, sizeof(addrs), &found);
    if (err != ESP_OK) {
        diag_error("scan: %s", esp_err_to_name(err));
        return -1;
    }

    if (found == 0) {
        /* The distinction worth spelling out: the parts were asked and did not
         * answer, so nothing about their configuration is the problem yet. */
        diag_printf("found:    nothing on the bus\n");
        diag_printf("          Nothing answered at any address, so this is the bus\n");
        diag_printf("          rather than any part: check that sda and scl are the\n");
        diag_printf("          pins the parts are actually on, that the bus has\n");
        diag_printf("          external pull-ups (the internal ones are ~45k and are\n");
        diag_printf("          not enough for three devices), and that the sensor\n");
        diag_printf("          supply is up.\n");
        return 0;
    }

    size_t shown = found < sizeof(addrs) ? found : sizeof(addrs);
    diag_printf("found:    %u device%s\n", (unsigned)found, found == 1 ? "" : "s");
    for (size_t i = 0; i < shown; i++) {
        diag_printf("          0x%02x  %s\n", (unsigned)addrs[i], i2c_what(addrs[i]));
    }
    if (found > shown) {
        diag_printf("          ... and %u more\n", (unsigned)(found - shown));
    }
    return 0;
}

static const cli_command_t i2c_commands[] = {
    {"scan", NULL, "Probe every address on the bus", cmd_i2c_scan},
};

static const cli_group_t i2c_group = {
    .name = "i2c",
    .help = "The bus every sensor is on",
    .commands = i2c_commands,
    .command_count = sizeof(i2c_commands) / sizeof(i2c_commands[0]),
};

esp_err_t app_console_register(void)
{
    esp_err_t err = cli_register_group(&sys_group);
    if (err != ESP_OK) {
        return err;
    }
    err = cli_register_group(&net_group);
    if (err != ESP_OK) {
        return err;
    }
    err = cli_register_group(&scale_group);
    if (err != ESP_OK) {
        return err;
    }
    err = cli_register_group(&sensor_group);
    if (err != ESP_OK) {
        return err;
    }
    return cli_register_group(&i2c_group);
}
