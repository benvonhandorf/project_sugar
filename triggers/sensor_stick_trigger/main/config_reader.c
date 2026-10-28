#include "config_reader.h"

#include <string.h>

#include "config_store.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "js2c_error_capture.h"

static const char *TAG = "config";

#define CONFIG_BUF_SIZE 4096

/* The LittleFS partition named in the partition table, and its mount point. */
#define FS_PARTITION "res"
#define FS_MOUNT     "/res"

static char s_source[8] = "none";

/*
 * One line per section, dispatched by the X-macro the schema generated.
 *
 * Adding a section to config_schema.json without wiring it here does not
 * compile: APP_CONFIG_SECTIONS expands to an X() naming a parser and a struct
 * member that do not exist yet. That is the whole reason the walker is generated
 * rather than hand-written -- the two cannot drift apart.
 */
#define SECTION_PARSER_wifi json_parse_wifi_manager_config_with_len
#define SECTION_PARSER_mqtt json_parse_mqtt_manager_config_with_len
#define SECTION_PARSER_ntp  json_parse_ntp_manager_config_with_len
#define SECTION_PARSER_http json_parse_http_server_config_with_len

#define PARSE_SECTION(name)                                                   \
    do {                                                                      \
        const app_config_json_ref_t *slice = &out->top.name;                  \
        js2c_error_capture_reset();                                           \
        /* In place: the slice indexes the buffer; no section is copied. */  \
        if (SECTION_PARSER_##name(json + slice->index, slice->length,         \
                                  &out->name)) {                              \
            snprintf(err, err_len, "%s: %s", #name, js2c_error_capture_get()); \
            return ESP_ERR_INVALID_ARG;                                       \
        }                                                                     \
    } while (0);

static esp_err_t parse_document(app_config_full_t *out, const char *json, size_t len,
                                char *err, size_t err_len)
{
    memset(out, 0, sizeof(*out));
    err[0] = '\0';

    js2c_error_capture_reset();
    if (json_parse_app_config_with_len(json, len, &out->top)) {
        snprintf(err, err_len, "%s", js2c_error_capture_get());
        return ESP_ERR_INVALID_ARG;
    }

    APP_CONFIG_SECTIONS(PARSE_SECTION)
    return ESP_OK;
}

/*
 * js2c puts its jsmn token buffer on the caller's stack, sized for the worst case
 * the schema permits: 298 tokens here, so json_parse_app_config_with_len opens with
 * a 4832-byte frame before it has looked at a single field. Neither caller has room
 * for that -- app_main runs on 5 kB, and the esp_http_server task that serves
 * POST /config on 4 kB -- and the overrun surfaces as a stack protection fault
 * wherever the parser happened to be (strtod, for a config with a float in it),
 * naming nothing that points back here.
 *
 * Widening both tasks for the life of the program would reserve 5 kB for a buffer
 * used twice, so the parse runs instead on a task sized for it that exits at once.
 * The caller blocks until it finishes, which keeps `job` on the caller's stack
 * valid and keeps the single static js2c error buffer to one parse at a time.
 *
 * Raise this if the schema grows: the requirement is the token_buffer in
 * build/js2c_gen/main/app_config.c plus the parser recursion beneath it.
 */
#define PARSE_TASK_STACK 8192

typedef struct {
    app_config_full_t *out;
    const char *json;
    size_t len;
    char *err;
    size_t err_len;
    esp_err_t result;
    SemaphoreHandle_t done;
} parse_job_t;

static void parse_task(void *arg)
{
    parse_job_t *job = (parse_job_t *)arg;

    job->result = parse_document(job->out, job->json, job->len, job->err, job->err_len);

    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static esp_err_t parse(app_config_full_t *out, const char *json, size_t len,
                       char *err, size_t err_len)
{
    StaticSemaphore_t done_storage;
    parse_job_t job = {
        .out = out,
        .json = json,
        .len = len,
        .err = err,
        .err_len = err_len,
        .result = ESP_FAIL,
        .done = xSemaphoreCreateBinaryStatic(&done_storage),
    };

    /* At the caller's own priority: this is the caller's work on a bigger stack,
     * and nothing else should be able to overtake it while the caller blocks. */
    if (xTaskCreate(parse_task, "cfgparse", PARSE_TASK_STACK, &job,
                    uxTaskPriorityGet(NULL), NULL) != pdPASS) {
        vSemaphoreDelete(job.done);
        snprintf(err, err_len, "no memory for the config parser task");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.result;
}

static esp_err_t mount_filesystem(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_MOUNT,
        .partition_label = FS_PARTITION,
        /* A first boot, or a partition resized by a new table, arrives here with
         * nothing readable. Formatting beats refusing to start: the defaults are
         * enough to bring the console up and be reconfigured. */
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mounting %s: %s", FS_PARTITION, esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_reader_load(app_config_full_t *out, char *err, size_t err_len)
{
    mount_filesystem();

    /*
     * An SD card overrides the built-in copy, so a unit can be reconfigured
     * without reflashing. Writes still go to the internal filesystem: a card can
     * be pulled at any moment, and the device has to boot without it.
     */
    static const config_store_config_t store = {
        .path = FS_MOUNT "/config.json",
        .override_path = "/sdcard/config.json",
        .max_size = CONFIG_BUF_SIZE,
    };
    esp_err_t cfg_err = config_store_init(&store);
    if (cfg_err != ESP_OK) {
        snprintf(err, err_len, "config_store: %s", esp_err_to_name(cfg_err));
        return cfg_err;
    }

    /* Static rather than on the stack: 4 kB would overflow most task stacks, and
     * this runs once at boot on app_main's. */
    static char json[CONFIG_BUF_SIZE];
    size_t len = 0;
    config_store_source_t source = CONFIG_STORE_SOURCE_NONE;

    esp_err_t read_err = config_store_read(json, sizeof(json), &len, &source);
    if (read_err == ESP_ERR_NOT_FOUND) {
        /*
         * Not an error. Every optional field has a default in its owning
         * component's schema, so an empty document yields a usable
         * configuration -- the device comes up on its own access point with the
         * console running, which is exactly what a freshly flashed unit needs.
         */
        ESP_LOGW(TAG, "no config.json; using schema defaults");
        snprintf(s_source, sizeof(s_source), "default");
        static const char EMPTY[] = "{\"config_version\":1,\"wifi\":{},\"mqtt\":{},"
                                    "\"ntp\":{},\"http\":{}}";
        return parse(out, EMPTY, strlen(EMPTY), err, err_len);
    }
    if (read_err != ESP_OK) {
        snprintf(err, err_len, "reading config: %s", esp_err_to_name(read_err));
        return read_err;
    }

    snprintf(s_source, sizeof(s_source), "%s",
             source == CONFIG_STORE_SOURCE_OVERRIDE ? "sdcard" : "flash");

    esp_err_t parse_err = parse(out, json, len, err, err_len);
    if (parse_err != ESP_OK) {
        ESP_LOGE(TAG, "config from %s is invalid: %s", s_source, err);
    } else {
        ESP_LOGI(TAG, "config loaded from %s", s_source);
    }
    return parse_err;
}

esp_err_t config_reader_store(const char *json, size_t len, char *err, size_t err_len)
{
    /* Parse before storing. A configuration that will not load must not be
     * allowed to replace one that does, because the way back is a cable. */
    static app_config_full_t scratch;
    esp_err_t parse_err = parse(&scratch, json, len, err, err_len);
    if (parse_err != ESP_OK) {
        return parse_err;
    }

    return config_store_write(json, len);
}

const char *config_reader_source(void)
{
    return s_source;
}
