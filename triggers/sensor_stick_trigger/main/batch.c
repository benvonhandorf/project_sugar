#include "batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_time.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "publish_queue.h"

static const char *TAG = "batch";

/*
 * Room reserved at the front of the allocation for the envelope, which is
 * written backwards into it at flush time so the whole message is one
 * contiguous buffer and no second copy of the data is ever made.
 *
 *   {"ts":1757193600000,"duration_ms":4294967295,"count":65535,"unit":"dBm","data":[
 *
 * is 80 characters at its longest; 96 leaves room for a longer unit without
 * anyone having to recount.
 */
#define PREFIX_RESERVE 96
#define SUFFIX "]}"
#define SUFFIX_LEN 2

esp_err_t batch_init(batch_t *b, const char *topic, const char *unit,
                     uint16_t max_entries, uint16_t max_bytes, uint16_t drop_entries)
{
    if (!b || !topic || max_entries == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_bytes <= PREFIX_RESERVE + SUFFIX_LEN + 16) {
        /* Not enough left for a single value once the envelope is paid for. */
        return ESP_ERR_INVALID_ARG;
    }

    memset(b, 0, sizeof(*b));
    b->buf = calloc(1, (size_t)max_bytes + 1);
    if (!b->buf) {
        return ESP_ERR_NO_MEM;
    }
    b->cap = (size_t)max_bytes - PREFIX_RESERVE - SUFFIX_LEN;
    b->topic = topic;
    b->unit = unit ? unit : "";
    b->max_entries = max_entries;
    b->drop_entries = drop_entries > max_entries ? drop_entries : max_entries;
    return ESP_OK;
}

void batch_free(batch_t *b)
{
    if (b && b->buf) {
        free(b->buf);
        b->buf = NULL;
    }
}

/* The values live after the reserved prefix so the envelope can be written in
 * front of them without moving them. */
static char *values(batch_t *b) { return b->buf + PREFIX_RESERVE; }

static void reset(batch_t *b)
{
    b->len = 0;
    b->count = 0;
    b->started_us = 0;
    b->started_wall = 0;
    b->pending = false;
    b->dropped_since_flush = 0;
}

void batch_discard(batch_t *b)
{
    if (!b || b->count == 0) {
        return;
    }
    b->discarded_batches++;
    b->discarded_entries += b->count;
    ESP_LOGW(TAG, "%s: discarding %u entries", b->topic, (unsigned)b->count);
    reset(b);
}

esp_err_t batch_flush(batch_t *b)
{
    if (!b || b->count == 0) {
        return ESP_OK;
    }

    const int64_t now_us = esp_timer_get_time();
    const uint32_t duration_ms = (uint32_t)((now_us - b->started_us) / 1000);

    /*
     * Both a duration and, when the clock is set, an absolute start time.
     * `duration_ms` alone is what the predecessor published, and it is enough
     * to reconstruct per-sample times *within* a batch but not to place the
     * batch on a timeline -- so a gap between two batches was unmeasurable.
     * The duration stays for consumers that already parse it.
     */
    char prefix[PREFIX_RESERVE + 1];
    int prefix_len;
    if (b->started_wall > 0) {
        prefix_len = snprintf(prefix, sizeof(prefix),
                              "{\"ts\":%lld,\"duration_ms\":%lu,\"count\":%u,"
                              "\"unit\":\"%s\",\"data\":[",
                              (long long)b->started_wall, (unsigned long)duration_ms,
                              (unsigned)b->count, b->unit);
    } else {
        prefix_len = snprintf(prefix, sizeof(prefix),
                              "{\"duration_ms\":%lu,\"count\":%u,"
                              "\"unit\":\"%s\",\"data\":[",
                              (unsigned long)duration_ms, (unsigned)b->count, b->unit);
    }
    if (prefix_len < 0 || prefix_len > PREFIX_RESERVE) {
        /* Cannot happen with the reserve above, but silently publishing a
         * truncated envelope would be worse than saying so. */
        ESP_LOGE(TAG, "%s: envelope does not fit its reserve", b->topic);
        batch_discard(b);
        return ESP_ERR_INVALID_SIZE;
    }

    char *start = values(b) - prefix_len;
    memcpy(start, prefix, (size_t)prefix_len);
    memcpy(values(b) + b->len, SUFFIX, SUFFIX_LEN + 1);
    const size_t total = (size_t)prefix_len + b->len + SUFFIX_LEN;

    /*
     * QoS 0: a "successful" send means the message was handed to the
     * publisher task, not that it reached the broker. Consumers must tolerate
     * gaps, which is why the timestamp and not the arrival order is the
     * source of timing truth. Queued rather than published directly so a
     * client mid-reconnect cannot stall the sampling task this is called
     * from -- see publish_queue.h.
     */
    esp_err_t err = publish_queue_send(b->topic, start, total, 0, false);
    if (err != ESP_OK) {
        b->publish_failures++;
        /*
         * Keep the buffer. The next append retries it, so a broker that was
         * briefly away costs nothing. What the next append cannot do is grow
         * the buffer past its byte cap, so incoming samples are dropped -- and
         * counted -- until either the retry succeeds or drop_entries is reached
         * and the whole batch goes. That is the resolution of a genuine
         * conflict in the specification: a batch cannot both grow across
         * retries and stay under a payload limit.
         */
        b->pending = true;
        return err;
    }

    b->published++;
    reset(b);
    return ESP_OK;
}

esp_err_t batch_append(batch_t *b, const char *formatted)
{
    if (!b || !b->buf || !formatted) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t n = strlen(formatted);
    if (n > b->cap) {
        /* One value larger than a whole message. Only reachable with an absurd
         * batch_max_bytes, and dropping it beats overrunning the buffer. */
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t need = n + (b->count ? 1 : 0); /* the comma */
    esp_err_t flush_err = ESP_OK;

    if (b->count >= b->max_entries || b->len + need > b->cap) {
        flush_err = batch_flush(b);
    }

    if (b->pending) {
        /* The flush failed and the buffer is still full. */
        b->dropped_since_flush++;
        b->discarded_entries++;
        if (b->count + b->dropped_since_flush >= b->drop_entries) {
            /* Bounded staleness: nobody wants a batch whose oldest entry is an
             * hour old, and holding one is how memory quietly disappears. */
            batch_discard(b);
        } else {
            return flush_err;
        }
    }

    if (b->count == 0) {
        /* Recorded after the first entry is accepted, not before: the duration
         * a consumer divides by the entry count has to span the entries it
         * actually received. */
        b->started_us = esp_timer_get_time();
        b->started_wall = app_time_epoch_ms();
    } else {
        b->buf[PREFIX_RESERVE + b->len++] = ',';
    }

    memcpy(values(b) + b->len, formatted, n);
    b->len += n;
    b->buf[PREFIX_RESERVE + b->len] = '\0';
    b->count++;

    return flush_err;
}
