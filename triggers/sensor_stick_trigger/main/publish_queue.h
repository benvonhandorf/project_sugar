#ifndef PUBLISH_QUEUE_H
#define PUBLISH_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Hands MQTT publishes to a dedicated task, for a caller that must never
 * block on one.
 *
 * The publisher task below calls mqtt_manager_enqueue(), not
 * mqtt_manager_publish(): the blocking publish takes the MQTT client's own
 * API lock, and on real hardware that lock has been observed held by the
 * client's network task indefinitely -- not just for one slow reconnect
 * attempt -- which wedges whatever called it, permanently and silently.
 * enqueue() only copies into the client's outbox and returns.
 *
 * weight.c's sampling task subscribes to the task watchdog because nothing
 * else in it should ever block for as long as one conversion; calling
 * mqtt_manager_publish() directly from there means a wedged MQTT client
 * reboots a sensor that was working fine. The publisher task below is its
 * own task precisely so a wedged client doesn't take anything else down with
 * it -- and it also subscribes to the task watchdog around the enqueue call
 * itself (see publisher_task()), as a second line of defence in case some
 * other path into the client blocks for reasons this project's code does
 * not control.
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
