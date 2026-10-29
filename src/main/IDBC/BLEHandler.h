#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// --- Constants ---
#define MAX_SAVED_DEVICES 3
#define MAC_STR_LEN       18
#define DEVICE_NAME       "Bike_CAM"

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

// --- Shared Externs ---
extern SemaphoreHandle_t incidentMutex;
extern QueueHandle_t bleImageReqQueue;
extern uint8_t deviceConnected;
extern uint8_t deviceReady;

// --- Structs ---
typedef struct {
    uint8_t active;
    uint32_t eventId;
    uint32_t startImage;
    uint32_t startChunk;
    char targetFolder[64];
} PendingImageRequest;

// --- Function Prototypes ---

/**
 * Handles the initialization of the shared variables and BLE services
 */
void init_ble(void);

/**
 * Helper to allow for auto reconnect attempts
 */
void load_saved_devices(void);

/**
 * Helper to save devices for auto reconnect later
 */
void save_device_address(const char* macAddr);

/**
 * Debug tool to see saved devices
 */
void print_saved_devices(void);

/**
 * Help prevent stack canaries by running on main stack
 */
void handle_pending_bluetooth_tasks(void);

/**
 * Send messages over BLE
 */
void notify_phone_of_event(uint32_t eventId, uint32_t distanceCm, uint32_t timeOffsetMs, uint32_t imageCount, uint32_t format);

/**
 * Test method to help run transfer acknowledgments
 */
void runTransferTest(uint32_t num, uint8_t counter);

#if defined(__cplusplus)
}
#endif

#endif