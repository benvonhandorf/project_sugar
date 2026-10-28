#pragma once

#include <stdint.h>

/*
 * Wall-clock time for message timestamps, or 0 when there is none.
 *
 * Zero rather than a plausible-looking number is the point: the ESP32 boots at
 * the start of 1970 and NTP may take a minute to arrive, so a timestamp taken
 * before the clock is set is off by decades while looking perfectly well
 * formed. Consumers can drop a zero; they cannot detect a 1970 date that has
 * already been graphed.
 */
int64_t app_time_epoch_ms(void);
