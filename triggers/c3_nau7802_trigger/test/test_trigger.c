/*
 * Host test for the trigger state machine.
 *
 * These are the acceptance tests from REQUIREMENTS.md section 14, the ones that
 * describe the detection logic rather than the transport. None of them needs a
 * bird and none of them can be run on a bench: they are about the shape of the
 * branches, and every way of getting that wrong produces output that looks like
 * a plausible feeding event.
 *
 *   make -C test
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "trigger.h"

static int failures;

static void expect(bool cond, const char *what)
{
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
    else       { printf("ok  : %s\n", what); }
}

/* The shipped defaults, so the tests exercise the constants a device runs. */
static trigger_config_t defaults(void)
{
    trigger_config_t cfg = {
        .min_g = 1.75,
        .max_g = 40.0,
        .hysteresis_g = 1.25,
        .baseline_alpha = 0.1,
        .reset_interval_samples = 100,
        .average_every = 20,
        .rebaseline_after_ms = 300000,
    };
    return cfg;
}

/* A run of samples, with the tallies a test wants to assert on. */
typedef struct {
    int asserts;
    int clears;
    int averages;
    int rebaselines;
    int inconsistent; /* steps where delta_g != |baseline_g - sample| */
    int64_t now_ms;
} run_t;

/* 500 ms per sample: 10 SPS over an average of 5, the shipped default. */
#define SAMPLE_MS 500

static void feed(trigger_state_t *s, const trigger_config_t *cfg, run_t *r,
                 double sample_g, int count)
{
    for (int i = 0; i < count; i++) {
        r->now_ms += SAMPLE_MS;
        trigger_action_t a = trigger_step(s, cfg, sample_g, r->now_ms);

        /*
         * The invariant every published trigger message must satisfy: the delta
         * is the distance from the baseline reported beside it. It is easy to
         * lose on a clear, because that same step also nudges the baseline --
         * and being wrong by alpha x delta looks exactly like rounding.
         */
        if (fabs(a.delta_g - fabs(a.baseline_g - sample_g)) > 1e-12) {
            r->inconsistent++;
        }
        r->asserts += a.publish_assert;
        r->clears += a.publish_clear;
        r->averages += a.publish_average;
        if (a.rebaseline) {
            r->rebaselines++;
            /* What weight.c does: acquire a baseline at the current load. Here
             * the load is known, so no acquisition is needed. */
            trigger_set_baseline(s, sample_g, r->now_ms);
        }
    }
}

static trigger_state_t at_baseline(double baseline_g, run_t *r)
{
    trigger_state_t s;
    memset(&s, 0, sizeof(s));
    memset(r, 0, sizeof(*r));
    trigger_set_baseline(&s, baseline_g, 0);
    return s;
}

/* T1 -- steady load: averages on the cadence, no triggers, baseline converges. */
static void t1_steady(void)
{
    const trigger_config_t cfg = defaults();
    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 100.0, 60);

    expect(r.asserts == 0 && r.clears == 0, "T1: a steady load triggers nothing");
    expect(r.averages == 3, "T1: an average every 20th settled sample");
    expect(fabs(s.baseline_g - 100.0) < 1e-9, "T1: the baseline holds");

    /* Convergence from a baseline that is wrong: alpha 0.1 over 50 samples
     * leaves (0.9^50) = 0.5% of the initial error. */
    s = at_baseline(90.0, &r);
    feed(&s, &cfg, &r, 90.5, 50);
    expect(fabs(s.baseline_g - 90.5) < 0.01,
           "T1: the baseline converges on a small step within ~50 samples");
}

/* T2 -- a 3 g step for 10 samples: exactly one assert and one clear. */
static void t2_one_landing(void)
{
    const trigger_config_t cfg = defaults();
    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 100.0, 5);
    feed(&s, &cfg, &r, 103.0, 10);
    feed(&s, &cfg, &r, 100.0, 5);

    expect(r.asserts == 1, "T2: one assert for one landing");
    expect(r.clears == 1, "T2: one clear when the weight settles");
    expect(r.inconsistent == 0,
           "T2: delta_g matches the baseline reported with it, on every step");
    expect(!s.triggered, "T2: the trigger is clear afterwards");
    expect(s.triggered_readings == 0, "T2: and the event counter is reset");
}

/*
 * T3 -- a 1 g step is below min_g, so it is not a bird.
 *
 * It is also *inside* the 1.25 g settled band, which is the part worth being
 * explicit about: the device considers itself settled and the baseline absorbs
 * the step. Two thresholds sit between "nothing happened" and "a bird landed",
 * and only the gap between them freezes the baseline -- see the case below.
 */
static void t3_too_small(void)
{
    const trigger_config_t cfg = defaults();
    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 101.0, 20);
    expect(r.asserts == 0, "T3: a 1.0 g step is below min_g and is not a bird");
    expect(s.baseline_g > 100.5,
           "T3: 1.0 g is inside the settled band, so the baseline follows it");
}

/*
 * The gap between hysteresis_g and min_g: too big to be settled, too small to
 * be a bird. Nothing is published and the baseline is frozen -- which is what
 * makes a slow drift of, say, 1.5 g stall the device until the re-baseline
 * fires, and is why rebaseline_after_s must not be 0 in the field.
 */
static void between_the_thresholds(void)
{
    trigger_config_t cfg = defaults();
    cfg.rebaseline_after_ms = 0;

    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 101.5, 40);
    expect(r.asserts == 0 && r.clears == 0,
           "between the thresholds: nothing is published");
    expect(r.averages == 0, "between the thresholds: no averages either");
    expect(fabs(s.baseline_g - 100.0) < 1e-9,
           "between the thresholds: the baseline is frozen");
    expect(s.triggered_readings == 40,
           "between the thresholds: the counter still advances");
}

/* T4 -- a 60 g step is above max_g: no trigger, frozen baseline, and the
 * re-baseline is what recovers. */
static void t4_refill(void)
{
    const trigger_config_t cfg = defaults();
    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    /* 300 s at 500 ms a sample is 600 samples; stop just short. */
    feed(&s, &cfg, &r, 160.0, 599);
    expect(r.asserts == 0, "T4: a 60 g step is above max_g and is not a bird");
    expect(fabs(s.baseline_g - 100.0) < 1e-9, "T4: the baseline stays frozen");
    expect(r.rebaselines == 0, "T4: and has not re-baselined yet");

    feed(&s, &cfg, &r, 160.0, 2);
    expect(r.rebaselines == 1, "T4: the re-baseline fires at 300 s");
    expect(fabs(s.baseline_g - 160.0) < 1e-9, "T4: at the new load");

    /* And the device works again: a bird on the refilled feeder is detected. */
    const int before = r.asserts;
    feed(&s, &cfg, &r, 163.0, 5);
    expect(r.asserts == before + 1, "T4: detection resumes after the re-baseline");
}

/*
 * T5 -- a 3 g step held for 400 samples: one assert, no repeats, no counter
 * wrap, and a clear on return. This is the test the predecessor's uint8_t
 * counter failed: it wrapped at 255 and re-armed the trigger branch.
 */
static void t5_long_event(void)
{
    trigger_config_t cfg = defaults();
    /* Long enough that the 400-sample event does not trip the re-baseline,
     * which is a different requirement being tested elsewhere. */
    cfg.rebaseline_after_ms = 0;

    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 103.0, 400);
    expect(r.asserts == 1, "T5: one assert for a 400-sample event");
    expect(r.clears == 0, "T5: it has not cleared while the weight is up");
    expect(s.triggered, "T5: and the trigger is still asserted");
    expect(s.triggered_readings == 400,
           "T5: the counter reached 400 without wrapping at 255");

    feed(&s, &cfg, &r, 100.0, 3);
    expect(r.clears == 1, "T5: one clear on return");
    expect(r.asserts == 1, "T5: and still only one assert in total");
}

/*
 * T6 -- an asserted trigger oscillating between 1.5 g and 3 g stays asserted
 * with no republishing. 1.5 g is above the hysteresis band but below min_g, so
 * it lands in the middle branch; a two-state machine would clear there.
 */
static void t6_oscillation(void)
{
    trigger_config_t cfg = defaults();
    cfg.rebaseline_after_ms = 0;

    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 103.0, 3);
    expect(r.asserts == 1 && s.triggered, "T6: asserted");

    for (int i = 0; i < 10; i++) {
        feed(&s, &cfg, &r, 101.5, 2);
        feed(&s, &cfg, &r, 103.0, 2);
    }

    expect(r.asserts == 1, "T6: no second assert across the oscillation");
    expect(r.clears == 0, "T6: and no clear -- 1.5 g is still outside the band");
    expect(s.triggered, "T6: the trigger is held throughout");
}

/*
 * The long-event cap. Once reset_interval_samples is reached the trigger branch
 * stops re-arming, but the event does not end: FR-5.5 is easy to implement as
 * "clear after N samples", which is a different behaviour entirely.
 */
static void cap_does_not_clear(void)
{
    trigger_config_t cfg = defaults();
    cfg.rebaseline_after_ms = 0;
    cfg.reset_interval_samples = 10;

    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 103.0, 50);
    expect(s.triggered, "cap: the trigger stays asserted past the cap");
    expect(r.clears == 0, "cap: reaching the cap does not clear the event");
    expect(s.triggered_readings == 50, "cap: the counter keeps counting");

    feed(&s, &cfg, &r, 100.0, 2);
    expect(r.clears == 1, "cap: the event ends when the weight settles");

    /* And the next landing arms normally, because the counter was reset. */
    feed(&s, &cfg, &r, 103.0, 2);
    expect(r.asserts == 2, "cap: the next landing arms again");
}

/* Nothing triggers before the startup baseline lands. */
static void not_initialized(void)
{
    const trigger_config_t cfg = defaults();
    trigger_state_t s;
    run_t r;
    memset(&s, 0, sizeof(s));
    memset(&r, 0, sizeof(r));

    feed(&s, &cfg, &r, 3.0, 20);
    expect(r.asserts == 0, "uninitialized: no trigger before the baseline lands");
    expect(r.rebaselines == 0,
           "uninitialized: and no re-baseline either -- there is nothing to re-do");
}

/* A negative excursion is the same size as a positive one: delta is absolute,
 * so a feeder hanging from a cell wired the other way still detects landings. */
static void negative_excursion(void)
{
    const trigger_config_t cfg = defaults();
    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    feed(&s, &cfg, &r, 97.0, 5);
    expect(r.asserts == 1, "a -3 g excursion is a trigger too");
    feed(&s, &cfg, &r, 100.0, 3);
    expect(r.clears == 1, "and it clears");
}

/* The bounds are strict on both sides, exactly as the predecessor's were. */
static void bounds_are_strict(void)
{
    const trigger_config_t cfg = defaults();
    run_t r;

    trigger_state_t s = at_baseline(100.0, &r);
    feed(&s, &cfg, &r, 100.0 + cfg.min_g, 5);
    expect(r.asserts == 0, "delta exactly min_g does not trigger");

    s = at_baseline(100.0, &r);
    feed(&s, &cfg, &r, 100.0 + cfg.max_g, 5);
    expect(r.asserts == 0, "delta exactly max_g does not trigger");

    s = at_baseline(100.0, &r);
    feed(&s, &cfg, &r, 100.0 + cfg.min_g + 0.001, 5);
    expect(r.asserts == 1, "a hair above min_g does");
}

/* Averages are skipped during activity, because the counter advances anyway. */
static void averages_are_irregular(void)
{
    trigger_config_t cfg = defaults();
    cfg.rebaseline_after_ms = 0;

    run_t r;
    trigger_state_t s = at_baseline(100.0, &r);

    /* Samples 1..19 settled, 20..39 triggered, 40..60 settled. Sample 20 would
     * have published; it is inside the event, so it does not. */
    feed(&s, &cfg, &r, 100.0, 19);
    expect(r.averages == 0, "no average yet");
    feed(&s, &cfg, &r, 103.0, 21);
    expect(r.averages == 0, "the 20th sample fell inside an event and published nothing");
    feed(&s, &cfg, &r, 100.0, 21);
    expect(r.averages == 1, "the next multiple of 20 that is settled publishes");
}

int main(void)
{
    t1_steady();
    t2_one_landing();
    t3_too_small();
    between_the_thresholds();
    t4_refill();
    t5_long_event();
    t6_oscillation();
    cap_does_not_clear();
    not_initialized();
    negative_excursion();
    bounds_are_strict();
    averages_are_irregular();

    printf("\n%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
