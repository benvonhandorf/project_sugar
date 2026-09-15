#include "trigger.h"

#include <math.h>
#include <string.h>

bool trigger_set_baseline(trigger_state_t *s, double baseline_g, int64_t now_ms)
{
    const bool was_triggered = s->triggered;

    s->baseline_g = baseline_g;
    s->initialized = true;
    s->triggered = false;
    s->triggered_readings = 0;
    s->last_settled_ms = now_ms;

    return was_triggered;
}

trigger_action_t trigger_step(trigger_state_t *s, const trigger_config_t *cfg,
                              double sample_g, int64_t now_ms)
{
    trigger_action_t action;
    memset(&action, 0, sizeof(action));

    s->iteration++;
    s->last_sample_g = sample_g;

    const double delta = fabs(s->baseline_g - sample_g);
    s->last_delta_g = delta;
    action.delta_g = delta;
    action.baseline_g = s->baseline_g;

    /*
     * Three branches, and they are not a switch on a state variable.
     *
     * The middle one deliberately does nothing except count, so an excursion
     * that has not returned to the settled band leaves an asserted trigger
     * asserted. Rewriting it as the obvious two-state machine -- "above the
     * threshold means triggered, below means not" -- ends an event every time a
     * bird shifts its weight, and produces a burst of assert/clear pairs
     * instead of one landing.
     */
    if (delta > cfg->min_g && delta < cfg->max_g &&
        s->triggered_readings < cfg->reset_interval_samples && s->initialized) {
        if (!s->triggered) {
            s->triggered = true;
            s->triggers++;
            action.publish_assert = true;
        }
        s->triggered_readings++;

    } else if (delta > cfg->hysteresis_g) {
        /*
         * Above the settled band but not a bird: too small, too large, or a
         * bird that has been here longer than reset_interval_samples. Count and
         * change nothing else.
         *
         * Saturating, and 32 bits wide. The predecessor counted here in a
         * uint8_t with no bound, so it wrapped at 255 and silently re-enabled
         * the first branch in the middle of an event -- a second assert for one
         * landing, roughly two minutes in.
         */
        if (s->triggered_readings < UINT32_MAX) {
            s->triggered_readings++;
        }

    } else {
        if (s->triggered) {
            s->triggered = false;
            /*
             * The counter is reset here and only here, and only when a trigger
             * was actually asserted. Resetting it in the middle branch would
             * make reset_interval_samples unreachable; resetting it on every
             * settled sample would make it count settled time rather than
             * event length.
             */
            s->triggered_readings = 0;
            action.publish_clear = true;
        }

        /*
         * The baseline moves only here. Freezing it everywhere else is what
         * stops a feeding bird being absorbed into the thing its weight is
         * measured against -- and it is also why a refill locks the device up
         * without the re-baseline below.
         */
        s->baseline_g = (1.0 - cfg->baseline_alpha) * s->baseline_g +
                        cfg->baseline_alpha * sample_g;
        s->last_settled_ms = now_ms;

        /*
         * The iteration counter advances on every sample but this test only
         * runs on a settled one, so `average` messages are irregular and are
         * skipped entirely while a bird is present. That is deliberate and is
         * the predecessor's behaviour: the cadence is no promise at all.
         */
        if (cfg->average_every > 0 && (s->iteration % cfg->average_every) == 0) {
            action.publish_average = true;
        }
    }

    /*
     * Long enough outside the settled band that the load itself has changed --
     * a refill, or the feeder being knocked. Without this the baseline stays
     * frozen for good: no triggers, no averages, and only a reboot recovers.
     */
    if (cfg->rebaseline_after_ms > 0 && s->initialized &&
        (now_ms - s->last_settled_ms) >= (int64_t)cfg->rebaseline_after_ms) {
        action.rebaseline = true;
    }

    action.updated_baseline_g = s->baseline_g;
    return action;
}
