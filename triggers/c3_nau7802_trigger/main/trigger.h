#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The trigger state machine, as arithmetic.
 *
 * Nothing here touches hardware, FreeRTOS or MQTT: a step takes a sample and
 * the wall clock and returns what should happen. That is the whole reason it is
 * its own file. The thresholds were tuned empirically against recorded feeder
 * data, the branch structure has three separate ways to be subtly wrong, and
 * none of it can be exercised on a bench without a bird -- so it is tested
 * off-target against synthetic and recorded traces instead. See test/.
 *
 * weight.c owns the publishing and the baseline re-acquisition; this owns the
 * decisions.
 */

typedef struct {
    double min_g;         /* below this change from the baseline, not a bird */
    double max_g;         /* above it, a refill or a knock */
    double hysteresis_g;  /* the settled band */
    double baseline_alpha;
    uint32_t reset_interval_samples;
    uint32_t average_every;
    /* 0 disables re-baselining, which reproduces the predecessor's lockup on
     * a refill. */
    uint32_t rebaseline_after_ms;
} trigger_config_t;

typedef struct {
    bool initialized;
    bool triggered;
    double baseline_g;
    double last_sample_g;
    double last_delta_g;
    uint32_t triggered_readings;
    uint64_t iteration;
    int64_t last_settled_ms;
    uint32_t triggers;
    uint32_t rebaselines;
} trigger_state_t;

/* What the caller should do about this sample. Several can be true at once. */
typedef struct {
    bool publish_assert;
    bool publish_clear;
    bool publish_average;
    /*
     * The device has been outside the settled band for rebaseline_after_ms.
     * The caller re-acquires a baseline at the current load and calls
     * trigger_set_baseline(); this module cannot do it, because acquiring a
     * baseline means taking more samples.
     */
    bool rebaseline;
    double delta_g;
    /*
     * The baseline `delta_g` was measured against -- before the settled branch
     * nudged it. Report this one with a trigger event, so that
     * delta_g == |baseline_g - sample| holds in the published message. Using
     * the updated value instead is off by alpha x delta, which is small enough
     * to look like rounding and wrong enough to make a consumer's own
     * arithmetic disagree with the device's.
     */
    double baseline_g;
    /* The baseline after this step: what `average` should carry, and what the
     * next sample will be compared against. Equal to baseline_g except on a
     * settled sample. */
    double updated_baseline_g;
} trigger_action_t;

/* Install a baseline: at startup, and again after a re-baseline. Clears any
 * asserted trigger, and the caller publishes the clear if `was_triggered`
 * comes back true. */
bool trigger_set_baseline(trigger_state_t *s, double baseline_g, int64_t now_ms);

/* One sample. */
trigger_action_t trigger_step(trigger_state_t *s, const trigger_config_t *cfg,
                              double sample_g, int64_t now_ms);
