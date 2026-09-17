#include "publish_queue.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
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
        /* mqtt_manager_enqueue(), not mqtt_manager_publish(): the blocking
         * publish takes the MQTT client's own API lock, and on real hardware
         * that lock can be held by the client's network task indefinitely --
         * not just for one slow reconnect attempt -- which wedged this task,
         * and with it every publish in the system, permanently and
         * silently. enqueue() only copies into the client's outbox and
         * returns; the client's own task sends it when it gets to it. The
         * watchdog stays as a second line of defence -- subscribed only
         * around the call, not the idle wait above, so a legitimately quiet
         * queue never trips it -- in case some other path into the client
         * blocks for reasons this project's code does not control. */
        esp_task_wdt_add(NULL);
        mqtt_manager_enqueue(job.topic, job.data, job.len, job.qos, job.retain);
        esp_task_wdt_delete(NULL);
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
