#ifndef DETECTOR_H
#define DETECTOR_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>

/* ── Event data ──────────────────────────────────────────────────────────────
 *
 * Populated and passed to the user callback whenever a vehicle-passing event
 * is confirmed. Copy any fields you need; the struct is only valid for the
 * duration of the callback.
 */
typedef struct {
    float    speed_mph;               /**< Estimated relative speed (MPH).        */
    float    length_inches;           /**< Estimated vehicle length (inches).      */
    float    closest_approach_inches; /**< Closest measured distance (inches).     */
    uint32_t timestamp_ms;            /**< Time of closest approach (ms since boot). */
} detector_event_t;

/* ── Callback type ───────────────────────────────────────────────────────────
 *
 * The callback is invoked from within detector_update(), on the same thread
 * that calls detector_update(). Keep it short.
 *
 * Common patterns
 * ───────────────
 * Simple flag (any platform):
 *   static volatile bool vehicle_detected = false;
 *   void on_vehicle(const detector_event_t *e) { vehicle_detected = true; }
 *
 * FreeRTOS — notify a camera task:
 *   void on_vehicle(const detector_event_t *e) {
 *       xTaskNotifyFromISR(camera_task_handle, 0, eNoAction, NULL);
 *   }
 *
 * FreeRTOS — push event data to a queue for processing elsewhere:
 *   void on_vehicle(const detector_event_t *e) {
 *       xQueueSendFromISR(event_queue, e, NULL);
 *   }
 */
typedef void (*detector_callback_t)(const detector_event_t *event);

/* ── API ─────────────────────────────────────────────────────────────────────*/

/**
 * Register a callback to be invoked on each confirmed vehicle-passing event.
 * Pass NULL to clear an existing callback.
 * Should be called before the first detector_update().
 */
void detector_set_callback(detector_callback_t cb);

/* ── Chunk-start callback ────────────────────────────────────────────────────
 *
 * Invoked the moment a new measurement chunk begins — i.e. the first reading
 * that breaks the inter-chunk silence threshold.  Fires on the same thread as
 * detector_update(); keep the handler short (set a flag or post to a queue).
 *
 * @param timestamp_ms  Timestamp of the first reading in the new chunk.
 */
typedef void (*detector_chunk_start_callback_t)(uint32_t timestamp_ms);

/**
 * Register a callback to be invoked at the start of each new chunk.
 * Pass NULL to clear an existing callback.
 * Should be called before the first detector_update().
 */
void detector_set_chunk_start_callback(detector_chunk_start_callback_t cb);

/* ── Potential-event callback ────────────────────────────────────────────────
 *
 * Fired at most once per chunk, mid-stream, as soon as the incoming data
 * satisfies rules 4-7 (approach profile looks like a real vehicle) AND the
 * distance has started rising back up from the minimum (closest approach is
 * confirmed).  Rules 8 and 9 are intentionally skipped here — those require
 * the full chunk and are checked by the normal confirmed-event callback.
 *
 * Use this to trigger a photo while the vehicle is still present, rather
 * than waiting for the 2-second end-of-chunk gap.
 *
 * The callback is invoked from within detector_update(), on the same thread.
 * Keep it short (set a flag or post to a queue).
 */
typedef void (*detector_potential_callback_t)(void);

/**
 * Register a callback to be invoked when a potential vehicle event is
 * detected mid-chunk.  Pass NULL to clear.
 */
void detector_set_potential_callback(detector_potential_callback_t cb);

/* ── Chunk-end callback ──────────────────────────────────────────────────────
 *
 * Fired at the end of every chunk, after the confirmed-event callback (if
 * any).  Useful for resetting state that should be cleared regardless of
 * whether the chunk was a real detection.
 */
typedef void (*detector_chunk_end_callback_t)(void);

/**
 * Register a callback to be invoked at the end of each chunk.
 * Pass NULL to clear.
 */
void detector_set_chunk_end_callback(detector_chunk_end_callback_t cb);

/**
 * Force analysis of any open chunk immediately, without waiting for a 2-second
 * gap. Call this on graceful shutdown or at the end of a recording session to
 * ensure the final event is not lost.
 *
 * In normal streaming use on a device this is rarely needed — the 2-second gap
 * will flush automatically. It is primarily useful for test harnesses that feed
 * a finite recording and then exit.
 */
void detector_flush(void);

/**
 * Feed a new sensor reading into the streaming vehicle detector.
 * Call this with every distance measurement from the sensor loop.
 *
 * This function implements detector_estimated_speed_and_length() from
 * Algorithms.py, adapted for a live data stream instead of a batch DataFrame.
 *
 * @param timestamp_ms    Milliseconds since boot.
 *                        Arduino:  pass millis()
 *                        ESP-IDF:  pass (uint32_t)(esp_timer_get_time() / 1000)
 * @param distance_inches Raw distance reading from the sensor in inches.
 */
void detector_update(uint32_t timestamp_ms, float distance_inches);

#if defined(__cplusplus)
}
#endif

#endif /* DETECTOR_H */
