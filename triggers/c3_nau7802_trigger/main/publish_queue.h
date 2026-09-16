#ifndef PUBLISH_QUEUE_H
#define PUBLISH_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Hands MQTT publishes to a dedicated task, for a caller that must never
 * block on one.
 *
 * mqtt_manager_publish() takes the MQTT client's own API lock, which the
 * client's network task also holds for as long as whatever it is doing that
 * iteration -- including a stalled reconnect. weight.c's sampling task
 * subscribes to the task watchdog because nothing else in it should ever
 * block for as long as one conversion; calling mqtt_manager_publish()
 * directly from there means a wedged MQTT client reboots a sensor that was
 * working fine. The publisher task below is not watchdog-subscribed, so
 * whatever the MQTT client does while it sorts itself out is its own
 * business.
 */

/* Idempotent: safe to call every time a caller might need the queue. */
esp_err_t publish_queue_start(void);

/*
 * Copies `data` and hands it to the publisher task. Never blocks.
 *
 * Returns ESP_ERR_INVALID_STATE when not connected -- the same signal
 * mqtt_manager_publish() gives synchronously today, so a caller with its own
 * retry-until-connected logic (batch_flush()) keeps working unchanged.
 * Returns ESP_ERR_NO_MEM if the copy or a queue slot could not be had; the
 * queue is bounded; a client that stays wedged sheds new publishes rather
 * than growing without limit.
 *
 * `topic` is stored by reference, not copied -- pass a string literal or
 * other static storage only.
 */
esp_err_t publish_queue_send(const char *topic, const void *data, size_t len,
                             int qos, bool retain);

#endif /* PUBLISH_QUEUE_H */
