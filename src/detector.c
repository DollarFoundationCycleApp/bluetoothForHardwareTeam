#include "detector.h"

#include <math.h>
#include <stdbool.h>

/* ── Tuning constants ── */

#define MAX_SENSOR_DISTANCE_INCHES            150.0f
#define MAX_DATA_GAP_MS                       2000U   /* 2-second gap splits chunks  */
#define MAX_DISTANCE_WITHIN_PERCENTAGE        0.05f   /* max must be in first 5%     */
#define MIN_DISTANCE_TOLERANCE_INCHES         15.0f   /* near-min window for spikes  */
#define MIN_DISTANCE_TRAVELED_DURING_APPROACH 30.0f
#define INCHES_PER_SECOND_TO_MPH              17.6f
#define MAX_RELATIVE_SPEED_MPH                80.0f
#define MAX_DISTANCE_UPTICK_ALLOWANCE_INCHES  30.0f
#define MIN_VEHICLE_LENGTH_INCHES             50.0f
#define MAX_VEHICLE_LENGTH_INCHES             1000.0f

/*
 * Buffer capacity for one chunk (number of in-range sensor readings).
 *
 * Size guide at 60 Hz (approach + alongside phases):
 *   ~1 mph relative  →  ~1100 samples   (cyclist overtaken very slowly)
 *   ~5 mph relative  →   ~220 samples
 *   ~30 mph relative →    ~40 samples
 *
 * Default of 4096 covers relative speeds down to ~1 mph comfortably.
 * Override at compile time if RAM is constrained, e.g.:
 *   -DMAX_CHUNK_SIZE=1024   (minimum ~5 mph relative)
 *   -DMAX_CHUNK_SIZE=16384  (test harness — unlimited headroom)
 *
 * If the buffer fills, new samples are silently dropped; the analysis
 * will see a truncated event and may fail Rule 9 (vehicle length).
 */
#ifndef MAX_CHUNK_SIZE
#define MAX_CHUNK_SIZE 4096
#endif

/* ── Internal state ─────────────────────────────────────────────────────────── */

static float                           s_dist[MAX_CHUNK_SIZE];
static uint32_t                        s_time[MAX_CHUNK_SIZE];   /* milliseconds */
static int                             s_count                = 0;
static uint32_t                        s_last_valid_ms        = 0;
static bool                            s_in_event             = false;
static detector_callback_t             s_callback             = NULL;
static detector_chunk_start_callback_t s_chunk_start_callback = NULL;
static detector_potential_callback_t   s_potential_callback   = NULL;
static detector_chunk_end_callback_t   s_chunk_end_callback   = NULL;

/* Running max/min state for mid-chunk potential-event detection */
static float    s_running_max_dist  = 0.0f;
static int      s_running_max_idx   = -1;
static float    s_running_min_dist  = MAX_SENSOR_DISTANCE_INCHES + 1.0f;
static int      s_running_min_idx   = -1;
static bool     s_potential_fired   = false;

/* ── analyze_chunk ──────────────────────────────────────────────────────────── */
/*
 * Translates the inner analyze_chunk() from Algorithms.py.
 * Operates on the s_dist / s_time buffers (s_count valid entries).
 *
 * Rule 4 – max distance within the first 5% of readings
 * Rule 5 – closest approach comes AFTER the max-distance point
 * Rule 6 – distance traveled during approach >= 30 in
 * Rule 7 – estimated relative speed <= 80 MPH
 * Rule 8 – last reading within 30 in of closest approach (graph ends flat)
 * Rule 9 – estimated vehicle length within [50, 1000] inches
 */
static bool analyze_chunk(detector_event_t *out)
{
    if (s_count == 0) return false;

    /* Find max and min distances and the index of the first maximum */
    float max_dist = s_dist[0];
    float min_dist = s_dist[0];
    int   max_idx  = 0;

    for (int i = 1; i < s_count; i++) {
        if (s_dist[i] > max_dist) {
            max_dist = s_dist[i];
            max_idx  = i;
        }
        if (s_dist[i] < min_dist) {
            min_dist = s_dist[i];
        }
    }

    /* Rule 4: max distance must fall within the first 5% of readings.
     * Keep the comparison in floating-point to match Python's idxmax() >= len*0.05
     * exactly — integer truncation shifts the boundary for small chunk sizes. */
    if ((float)max_idx >= (float)s_count * MAX_DISTANCE_WITHIN_PERCENTAGE) {
        return false;
    }

    uint32_t max_dist_ts = s_time[max_idx];

    /*
     * Find the first reading within MIN_DISTANCE_TOLERANCE_INCHES of the
     * minimum. This ignores short noise spikes and locates the real closest
     * approach point, mirroring the within_15_mask logic in the Python code.
     */
    int min_close_idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_dist[i] <= min_dist + MIN_DISTANCE_TOLERANCE_INCHES) {
            min_close_idx = i;
            break;
        }
    }

    if (min_close_idx == -1) return false;

    uint32_t min_close_ts   = s_time[min_close_idx];
    float    min_close_dist = s_dist[min_close_idx];

    /* Rule 5: closest approach must come after the max-distance timestamp.
     * Guards against a mirrored graph (bike overtaking car). */
    if (min_close_ts <= max_dist_ts) {
        return false;
    }

    float    first_dist      = s_dist[0];
    float    last_dist       = s_dist[s_count - 1];
    uint32_t last_ts         = s_time[s_count - 1];

    float time_approach_s   = (float)(min_close_ts - s_time[0]) / 1000.0f;
    float distance_traveled = first_dist - min_close_dist;

    /* Rule 6: enough distance covered during the approach */
    if (distance_traveled < MIN_DISTANCE_TRAVELED_DURING_APPROACH) {
        return false;
    }

    /* Guard against divide-by-zero */
    if (time_approach_s <= 0.0f) return false;

    /* Rule 7: estimated relative speed must be <= 80 MPH */
    float speed_in_per_s = distance_traveled / time_approach_s;
    float speed_mph      = speed_in_per_s / INCHES_PER_SECOND_TO_MPH;

    if (speed_mph > MAX_RELATIVE_SPEED_MPH) {
        return false;
    }

    /* Rule 8: last reading must be within 30 in of the closest approach.
     * Ensures the graph ends flat rather than rising (vehicle still present). */
    if (fabsf(last_dist - min_close_dist) > MAX_DISTANCE_UPTICK_ALLOWANCE_INCHES) {
        return false;
    }

    /* Rule 9: estimated vehicle length must fall within [50, 1000] inches.
     * Length = relative_speed * time_alongside */
    float time_after_min_s = (float)(last_ts - min_close_ts) / 1000.0f;
    float estimated_length = speed_in_per_s * time_after_min_s;

    if (estimated_length < MIN_VEHICLE_LENGTH_INCHES ||
        estimated_length > MAX_VEHICLE_LENGTH_INCHES) {
        return false;
    }

    out->speed_mph               = speed_mph;
    out->length_inches           = estimated_length;
    out->closest_approach_inches = min_close_dist;
    out->timestamp_ms            = min_close_ts;

    return true;
}

/* ── finalize_chunk ─────────────────────────────────────────────────────────── */

static void finalize_chunk(void)
{
    if (s_count > 0) {
        detector_event_t event;
        if (analyze_chunk(&event) && s_callback != NULL) {
            s_callback(&event);
        }
    }

    if (s_chunk_end_callback != NULL) {
        s_chunk_end_callback();
    }

    s_count             = 0;
    s_in_event          = false;
    s_running_max_dist  = 0.0f;
    s_running_max_idx   = -1;
    s_running_min_dist  = MAX_SENSOR_DISTANCE_INCHES + 1.0f;
    s_running_min_idx   = -1;
    s_potential_fired   = false;
}

/* ── detector_set_callback ──────────────────────────────────────────────────── */

void detector_set_callback(detector_callback_t cb)
{
    s_callback = cb;
}

/* ── detector_set_chunk_start_callback ──────────────────────────────────────── */

void detector_set_chunk_start_callback(detector_chunk_start_callback_t cb)
{
    s_chunk_start_callback = cb;
}

/* ── detector_set_potential_callback ────────────────────────────────────────── */

void detector_set_potential_callback(detector_potential_callback_t cb)
{
    s_potential_callback = cb;
}

/* ── detector_set_chunk_end_callback ────────────────────────────────────────── */

void detector_set_chunk_end_callback(detector_chunk_end_callback_t cb)
{
    s_chunk_end_callback = cb;
}

/* ── detector_flush ─────────────────────────────────────────────────────────── */

void detector_flush(void)
{
    if (s_in_event) {
        finalize_chunk();
    }
}

/* ── detector_update ────────────────────────────────────────────────────────── */
/*
 * Streaming entry point. Call once per sensor reading.
 *
 * Mirrors the outer logic of detector_estimated_speed_and_length():
 *   - Readings > MAX_SENSOR_DISTANCE_INCHES are filtered out (rule 1).
 *   - A gap > 2 s between consecutive valid readings splits chunks (rule 2).
 *   - When a chunk is closed, analyze_chunk() applies rules 3-9.
 *   - If any chunk passes all rules, the detection fires (rule 10).
 */
void detector_update(uint32_t timestamp_ms, float distance_inches)
{
    bool valid = (distance_inches <= MAX_SENSOR_DISTANCE_INCHES);

    if (valid) {
        /*
         * A gap > 2 seconds between valid readings means the previous event
         * ended. Close the old chunk before starting a new one.
         */
        if (s_in_event &&
            (timestamp_ms - s_last_valid_ms) > MAX_DATA_GAP_MS) {
            finalize_chunk();
        }

        /* First reading of a new chunk — fire the chunk-start callback */
        if (!s_in_event && s_chunk_start_callback != NULL) {
            s_chunk_start_callback(timestamp_ms);
        }

        /* Append to the current chunk (silently drop if buffer is full) */
        if (s_count < MAX_CHUNK_SIZE) {
            s_dist[s_count] = distance_inches;
            s_time[s_count] = timestamp_ms;
            s_count++;
        }

        s_last_valid_ms = timestamp_ms;
        s_in_event      = true;

        /* ── Mid-chunk potential-event check ────────────────────────────────
         * Update running max/min, then test rules 4-7 once the vehicle has
         * clearly passed the closest-approach point (distance rising again).
         * Fires at most once per chunk via s_potential_fired guard.
         */
        if (s_dist[s_count - 1] > s_running_max_dist) {
            s_running_max_dist = s_dist[s_count - 1];
            s_running_max_idx  = s_count - 1;
        }
        if (s_dist[s_count - 1] < s_running_min_dist) {
            s_running_min_dist = s_dist[s_count - 1];
            s_running_min_idx  = s_count - 1;
        }

        if (!s_potential_fired
            && s_running_max_idx >= 0
            && s_running_min_idx > s_running_max_idx
            && s_dist[s_count - 1] > s_running_min_dist + MIN_DISTANCE_TOLERANCE_INCHES)
        {
            /* Rule 4: max was in the first 5% of readings so far */
            if ((float)s_running_max_idx < (float)s_count * MAX_DISTANCE_WITHIN_PERCENTAGE) {
                /* Rule 6: enough distance traveled during approach */
                float distance_traveled = s_dist[0] - s_running_min_dist;
                if (distance_traveled >= MIN_DISTANCE_TRAVELED_DURING_APPROACH) {
                    /* Rule 7: estimated approach speed is plausible */
                    float time_approach_s =
                        (float)(s_time[s_running_min_idx] - s_time[0]) / 1000.0f;
                    if (time_approach_s > 0.0f) {
                        float speed_mph =
                            (distance_traveled / time_approach_s) / INCHES_PER_SECOND_TO_MPH;
                        if (speed_mph <= MAX_RELATIVE_SPEED_MPH) {
                            s_potential_fired = true;
                            if (s_potential_callback != NULL) {
                                s_potential_callback();
                            }
                        }
                    }
                }
            }
        }

    } else {
        /*
         * Out-of-range reading. If we are in an event and have not seen a
         * valid reading for > 2 seconds, close the chunk.
         */
        if (s_in_event &&
            (timestamp_ms - s_last_valid_ms) > MAX_DATA_GAP_MS) {
            finalize_chunk();
        }
    }
}
