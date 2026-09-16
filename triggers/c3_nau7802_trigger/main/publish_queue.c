#include "publish_queue.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_manager.h"

static const char *TAG = "publish_queue";

#define QUEUE_DEPTH 32
#define TASK_STACK  3072
#define TASK_PRIO   4

typedef struct {
    const char *topic;
    void *data;
    size_t len;
    int qos;
    bool retain;
} publish_job_t;

static QueueHandle_t s_queue;
static uint32_t s_dropped;

static void publisher_task(void *arg)
{
    (void)arg;
    publish_job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* However long this takes -- a client that is mid-reconnect is not
         * this task's problem to escalate, and nothing here feeds a task
         * watchdog. */
        mqtt_manager_publish(job.topic, job.data, job.len, job.qos, job.retain);
        free(job.data);
    }
}

esp_err_t publish_queue_start(void)
{
    if (s_queue) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(publish_job_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(publisher_task, "mqtt_pub", TASK_STACK, NULL, TASK_PRIO, NULL) !=
        pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t publish_queue_send(const char *topic, const void *data, size_t len,
                             int qos, bool retain)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Checked here, synchronously, rather than left for the publisher task to
     * discover: a caller like batch_flush() decides whether to keep its
     * buffer for a retry based on this return value. */
    if (!mqtt_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    void *copy = NULL;
    if (len > 0) {
        copy = malloc(len);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, data, len);
    }

    publish_job_t job = {
        .topic = topic, .data = copy, .len = len, .qos = qos, .retain = retain,
    };
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        free(copy);
        s_dropped++;
        ESP_LOGW(TAG, "queue full; dropped a publish to %s (%lu dropped total)",
                 topic, (unsigned long)s_dropped);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
