#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * A batch of telemetry values, published as one MQTT message.
 *
 * Two of these exist -- weight samples and RSSI -- and they are deliberately
 * independent. In the predecessor both counters crossed their threshold on the
 * same iteration and a failure on one desynchronised the other permanently, so
 * the `duration` fields on the two topics stopped being comparable. Nothing
 * here is shared between instances.
 *
 * A batch is flushed when either the entry count is reached or the next value
 * would push the serialised payload past its byte cap, whichever comes first.
 * Sizing by count alone is what made the predecessor's batches fail
 * intermittently: it fixed the count at 257 against a 2048-byte buffer, and a
 * batch landed at 1.8-2.1 kB depending on how many integer digits the readings
 * happened to need.
 */

typedef struct {
    /* Text of the values, comma separated, without the surrounding envelope. */
    char *buf;
    size_t cap;
    size_t len;

    uint16_t count;
    uint16_t max_entries;
    uint16_t drop_entries;

    int64_t started_us;   /* esp_timer time when the first entry was appended */
    int64_t started_wall; /* epoch ms at that moment, or 0 if the clock was unset */

    const char *topic;
    const char *unit;

    /* Held for a retry after a failed publish. */
    bool pending;
    uint16_t dropped_since_flush;

    /* Counters, reported in the status message. */
    uint32_t published;
    uint32_t publish_failures;
    uint32_t discarded_batches;
    uint32_t discarded_entries;
} batch_t;

/*
 * `max_bytes` is the whole message budget: the envelope is measured out of it,
 * so a batch never produces a payload larger than this. It must stay under the
 * MQTT client's outbound buffer or every publish fails at the transport.
 */
esp_err_t batch_init(batch_t *b, const char *topic, const char *unit,
                     uint16_t max_entries, uint16_t max_bytes, uint16_t drop_entries);

void batch_free(batch_t *b);

/*
 * Append one already-formatted value, flushing first if it would not fit.
 *
 * Returns the result of the flush it triggered, or ESP_OK when none was needed.
 * A caller does not have to check it -- the counters record what happened -- but
 * it is there for one that wants to log the first failure.
 */
esp_err_t batch_append(batch_t *b, const char *formatted);

/* Publish whatever is buffered, if anything. */
esp_err_t batch_flush(batch_t *b);

/* Throw the buffer away without publishing, counting it as discarded. */
void batch_discard(batch_t *b);
