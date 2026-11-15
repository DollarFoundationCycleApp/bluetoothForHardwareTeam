#ifndef BLE_HANDLER_H
#define BLE_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
// includes below here are from bleHandler.cpp
#include "IncidentManager.h"
#include "ImageChunker.h"
#include "sdkconfig.h"

// --- Nanopb Includes ---
#include "Nanopb/pb_decode.h"
#include "Nanopb/pb_encode.h"
#include "Nanopb/pb.h"
#include "Nanopb/pb_common.h"
#include "IBDC_v0.3.2.pb.h"

#include "esp_bt.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// ESP-IDF / NimBLE C Host APIs
#include "nimble/ble.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"
#include "nvs.h"

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
extern uint8_t deviceConnected;
extern uint8_t deviceReady;
#if defined(__cplusplus)
extern "C" {
#endif
extern SemaphoreHandle_t incidentMutex;
extern QueueHandle_t bleImageReqQueue;
// --- Structs ---
typedef struct {
    uint8_t active;
    uint32_t eventId;
    uint32_t startImage;
    uint32_t startChunk;
    char targetFolder[64];
} PendingImageRequest;
#if defined(__cplusplus)
}
#endif
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

#endif