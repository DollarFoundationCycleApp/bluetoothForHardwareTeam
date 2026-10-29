#include "BLEHandler.h"
#include "IncidentManager.h"
#include "ImageChunker.h"

// --- Nanopb Includes ---
#include <pb_decode.h>
#include <pb_encode.h>
#include "IBDC_v0.3.2.pb.h"

#include "esp_bt.h"
#include <stdio.h>
#include <string.h>
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

#define TAG "BLE_HANDLER"

//todo delete this before prod
extern void serial_print(const char* str);


// --- Custom UUIDs for Data/File Transfers (Little-Endian format for raw C structs) ---
static const ble_uuid128_t uuid_service_data = BLE_UUID128_INIT(
  0xBA, 0x97, 0xC2, 0x5C, 0x44, 0x0E, 0xB6, 0xB2, 
  0xB1, 0x4A, 0xEF, 0x93, 0x4B, 0x1F, 0xA5, 0xD4
);
static const ble_uuid128_t uuid_char_rx = BLE_UUID128_INIT(
  0xBA, 0x97, 0xC2, 0x5C, 0x44, 0x0E, 0xB6, 0xB2, 
  0xB1, 0x4A, 0xEF, 0x93, 0x4C, 0x1F, 0xA5, 0xD4
);
static const ble_uuid128_t uuid_char_tx = BLE_UUID128_INIT(
  0xBA, 0x97, 0xC2, 0x5C, 0x44, 0x0E, 0xB6, 0xB2, 
  0xB1, 0x4A, 0xEF, 0x93, 0x4D, 0x1F, 0xA5, 0xD4
);

// Global States
uint8_t deviceConnected = FALSE;
uint8_t deviceReady = FALSE;
uint16_t current_conn_handle = 0;
SemaphoreHandle_t incidentMutex = NULL;
QueueHandle_t bleImageReqQueue = NULL;

char recentDevices[MAX_SAVED_DEVICES][MAC_STR_LEN]; 
int savedDeviceCount = 0;
uint16_t tx_val_handle = 0;

// Minimal HID Keyboard Report Descriptor (Enables OS-level phone pairing)
static const uint8_t hidReportDescriptor[] = {
  0x05, 0x01, // USAGE_PAGE (Generic Desktop)
  0x09, 0x06, // USAGE (Keyboard)
  0xa1, 0x01, // COLLECTION (Application)
  0x85, 0x01, //   REPORT_ID (1)
  0x05, 0x07, //   USAGE_PAGE (Keyboard)
  0x19, 0x00, //   USAGE_MINIMUM (Keyboard Right GUI)
  0x29, 0x65, //   USAGE_MAXIMUM (Keyboard Right GUI)
  0x15, 0x00, //   LOGICAL_MINIMUM (0)
  0x25, 0x65, //   LOGICAL_MAXIMUM (101)
  0x75, 0x08, //   REPORT_SIZE (8)
  0x95, 0x06, //   REPORT_COUNT (6)
  0x81, 0x02, //   INPUT (Data,Var,Abs)
  0x95, 0x01, //   REPORT_COUNT (1)
  0x75, 0x08, //   REPORT_SIZE (8)
  0x81, 0x00, //   INPUT (Data,Ary,Abs)
  0xc0        // END_COLLECTION
};

// Forward Declarations
static int rx_characteristic_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event_handler(struct ble_gap_event *event, void *arg);
static void start_advertising(void);
static void handle_protobuf_command(const IDBC_AppToDevice* appCmd);
static uint8_t send_device_to_app_response(const IDBC_DeviceToApp* response);

static int hid_info_access_cb(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    const uint8_t hidInfoVal[] = {0x11, 0x01, 0x00, 0x02};
    return os_mbuf_append(ctxt->om, hidInfoVal, sizeof(hidInfoVal)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_report_map_access_cb(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    return os_mbuf_append(ctxt->om, hidReportDescriptor, sizeof(hidReportDescriptor)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int tx_characteristic_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // TX characteristic is used for notifications, but standard reads can return ok or empty
    return 0;
}

// --- GATT Server Table ---
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
  {
    /* Service 1: Standard Bluetooth SIG HID Service (0x1812) */
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = BLE_UUID16_DECLARE(0x1812),
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        .uuid = BLE_UUID16_DECLARE(0x2A4A),
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
        .access_cb = hid_info_access_cb
      },
      {
        .uuid = BLE_UUID16_DECLARE(0x2A4B),
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
        .access_cb = hid_report_map_access_cb
      },
      { 0 }
    }
  },
  {
    /* Service 2: Custom Service (For Protobuf / File / Data Transfer) */
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &uuid_service_data.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        /* RX Characteristic: App -> Device Commands */
        .uuid = &uuid_char_rx.u,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
        .access_cb = rx_characteristic_access_cb
      },
      {
        /* TX Characteristic: Device -> App Notifications */
        .uuid = &uuid_char_tx.u,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .val_handle = &tx_val_handle,
        .access_cb = tx_characteristic_access_cb
      },
      { 0 }
    }
  },
  { 0 }
};

/**
* GAP Event Handler: Manages native connection, disconnection, and pairing events.
*/
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        deviceConnected = TRUE;
        current_conn_handle = event->connect.conn_handle;
        
        // Retrieve connection descriptor to get the peer's actual address
        struct ble_gap_conn_desc desc;
        char clientMac[MAC_STR_LEN] = "Unknown";
        if (ble_gap_conn_find(current_conn_handle, &desc) == 0) {
            snprintf(clientMac, sizeof(clientMac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                 desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                 desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
        }
        
        ESP_LOGI(TAG, ">>> CONNECTED NATIVELY! Address: %s <<<", clientMac);
        save_device_address(clientMac);
      } else {
        ESP_LOGW(TAG, "Connection failed. Restarting advertising...");
        start_advertising();
      }
      break;
      
    case BLE_GAP_EVENT_DISCONNECT:
      deviceConnected = FALSE;
      current_conn_handle = 0;
      ESP_LOGI(TAG, ">>> BLE DISCONNECTED (Reason: %d). Restarting Advertising... <<<", event->disconnect.reason);
      start_advertising();
      break;
      
    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, ">>> Pairing & Encryption Complete! Bound to Phone OS. <<<");
      break;
      
    default:
      break;
  }
  return 0;
}

/**
* RX Callback: Handles incoming write requests on the custom protobuf characteristic.
*/
static int rx_characteristic_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t rx_buffer[512];
    if (data_len > sizeof(rx_buffer)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    // Flatten the incoming OS network buffer into a clean C byte array
    int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buffer, data_len, &data_len);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    // Decode incoming Nanopb AppToDevice frame
    IDBC_AppToDevice appCmd = IDBC_AppToDevice_init_default;
    pb_istream_t stream = pb_istream_from_buffer(rx_buffer, data_len);

    if (pb_decode(&stream, IDBC_AppToDevice_fields, &appCmd)) {
      handle_protobuf_command(&appCmd);
    } else {
      ESP_LOGE(TAG, "Protobuf Decode Error: %s", PB_GET_ERROR(&stream));
    }
  }
  return 0;
}

static void nimble_host_task(void *param) {
    // This runs the NimBLE host background processing loop
    serial_print("[BLE Debug] Before nimble_port_run");
    nimble_port_run();
    serial_print("[BLE Debug] After nimble_port_run");
    nimble_port_freertos_deinit();
}

static void on_stack_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason: %d", reason);
}

static void on_stack_sync(void) {
    // Start advertising once the stack is synced and ready
    start_advertising();
}

static void ble_app_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason: %d", reason);
}

static void ble_app_on_sync(void) {
    on_stack_sync();
}

/**
* Main initialization routine for BLE, queues, and security policies.
*/
void init_ble(void) { // todo verify this method gets through successfully. 
  serial_print("[BLE Debug] Start of init_ble\n");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  
  incidentMutex = xSemaphoreCreateMutex();
  if (incidentMutex == NULL) {
    serial_print("[BLE Error] Failed to create incident mutex\n");
    return;
  }

  bleImageReqQueue = xQueueCreate(10, sizeof(PendingImageRequest));
if (bleImageReqQueue == NULL) {
  serial_print("[BLE Error] Failed to create image request queue\n");
  return;
}

  serial_print("[BLE Debug] Here 1\n");

  //Release Classic BT memory footprint back to system heap
  serial_print("[BLE Debug] Before mem_release");
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  serial_print("[BLE Debug] After mem_release\n");

  //Initialize NimBLE port
  serial_print("[BLE Debug] Before port_init");
  nimble_port_init();
  serial_print("[BLE Debug] After port_init\n");

  //Set required host configuration & callbacks
  serial_print("[BLE Debug] Start of config & callbacks");
  ble_hs_cfg.reset_cb = ble_app_on_reset;
  serial_print("[BLE Debug] After reset");
  ble_hs_cfg.sync_cb = ble_app_on_sync;
  serial_print("[BLE Debug] After sync");
  ble_hs_cfg.sm_bonding = 1;
  serial_print("[BLE Debug] After bonding");
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  serial_print("[BLE Debug] After io cap");
  ble_hs_cfg.sm_mitm = 0;
  serial_print("[BLE Debug] After mitm");
  ble_hs_cfg.sm_sc = 1;
  serial_print("[BLE Debug] After sc\n");

  serial_print("[BLE Debug] Before loading saved devices");
  load_saved_devices();
  print_saved_devices();

  //serial_print("[BLE Debug] Before power set");
  //esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
  //serial_print("[BLE Debug] After power set");

  //Register GATT Services
  serial_print("[BLE Debug] Before count_cfg");
  int rc = ble_gatts_count_cfg(gatt_svr_svcs);
  assert(rc == 0);
  serial_print("[BLE Debug] After count_cfg");

  serial_print("[BLE Debug] Before add svcs");
  rc = ble_gatts_add_svcs(gatt_svr_svcs);
  assert(rc == 0);
  serial_print("[BLE Debug] After add_svcs");

  //Start the GATT Server & Device Name
  //serial_print("[BLE Debug] Before gatts_start");
  //rc = ble_gatts_start();
  //assert(rc == 0);
  //serial_print("[BLE Debug] After gatts_start");

  serial_print("[BLE Debug] Before device_name_set");
  ble_svc_gap_device_name_set(DEVICE_NAME);
  serial_print("[BLE Debug] After device_name_set");

  //Start FreeRTOS NimBLE host background task runner
  serial_print("[BLE Debug] Before freertos_init");
  nimble_port_freertos_init(nimble_host_task);
  serial_print("[BLE Debug] After freertos_init");

  deviceReady = TRUE;
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "BLE Server Started: %s", DEVICE_NAME);
  ESP_LOGI(TAG, "========================================");
  serial_print("[BLE Debug] End of init_ble");
}

/**
* Loads saved MAC addresses from NVS flash storage.
*/
void load_saved_devices(void) {
  savedDeviceCount = 0;
  for (int i = 0; i < MAX_SAVED_DEVICES; i++) {
    recentDevices[i][0] = '\0';
  }

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("ble_devices", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    return;
  }

  int32_t count = 0;
  if (nvs_get_i32(nvs_handle, "count", &count) == ESP_OK) {
    savedDeviceCount = (count > MAX_SAVED_DEVICES) ? MAX_SAVED_DEVICES : count;
    char keyBuf[16];
    for (int i = 0; i < savedDeviceCount; i++) {
      snprintf(keyBuf, sizeof(keyBuf), "dev_%d", i);
      
      size_t required_size = MAC_STR_LEN;
      // Safely fetch string with error checking
      if (nvs_get_str(nvs_handle, keyBuf, recentDevices[i], &required_size) != ESP_OK) {
        recentDevices[i][0] = '\0';
      }
    }
  }
  nvs_close(nvs_handle);
}

/**
* Saves connected MAC address to NVS flash storage.
*/
void save_device_address(const char* macAddr) {
  if (macAddr == NULL || macAddr[0] == '\0') return;

  int existingIdx = -1;
  for (int i = 0; i < MAX_SAVED_DEVICES; i++) {
    if (strcasecmp(recentDevices[i], macAddr) == 0) {
      existingIdx = i;
      break;
    }
  }

  int shiftEnd = (existingIdx != -1) ? existingIdx : (MAX_SAVED_DEVICES - 1);
  for (int i = shiftEnd; i > 0; i--) {
    snprintf(recentDevices[i], MAC_STR_LEN, "%s", recentDevices[i - 1]);
  }
  snprintf(recentDevices[0], MAC_STR_LEN, "%s", macAddr);

  if (existingIdx == -1 && savedDeviceCount < MAX_SAVED_DEVICES) {
    savedDeviceCount++;
  }

  nvs_handle_t nvs_handle;
  if (nvs_open("ble_devices", NVS_READWRITE, &nvs_handle) == ESP_OK) {
    nvs_set_i32(nvs_handle, "count", savedDeviceCount);
    char keyBuf[16];
    for (int i = 0; i < savedDeviceCount; i++) {
      snprintf(keyBuf, sizeof(keyBuf), "dev_%d", i);
      nvs_set_str(nvs_handle, keyBuf, recentDevices[i]);
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }
  print_saved_devices();
}

/**
* Output recent devices to Serial Monitor / ESP Logger.
*/
void print_saved_devices(void) {
  ESP_LOGI(TAG, "--- Saved Recent Devices (Max 3) ---");
  if (savedDeviceCount == 0) {
    ESP_LOGI(TAG, "  (None)");
  } else {
    for (int i = 0; i < savedDeviceCount; i++) {
      ESP_LOGI(TAG, "  [%d] %s", i + 1, recentDevices[i]);
    }
  }
  ESP_LOGI(TAG, "------------------------------------");
}

/**
* Encodes and sends a DeviceToApp protobuf response back to the phone via TX characteristic notifications.
*/
static uint8_t send_device_to_app_response(const IDBC_DeviceToApp* response) {
  if (!deviceConnected) {
    ESP_LOGW(TAG, "Error sending: Device not connected.");
    return FALSE;
  }

  uint8_t buffer[1024];
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

  if (!pb_encode(&stream, IDBC_DeviceToApp_fields, response)) {
    ESP_LOGE(TAG, "Nanopb encoding failed: %s", PB_GET_ERROR(&stream));
    return FALSE;
  }

  // Allocate mbuf packet and push notification natively over NimBLE
  struct os_mbuf* om = ble_hs_mbuf_from_flat(buffer, stream.bytes_written);
  if (om == NULL) {
    ESP_LOGE(TAG, "Failed to allocate mbuf for notification");
    return FALSE;
  }

  int rc = ble_gatts_notify_custom(current_conn_handle, tx_val_handle, om);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to send BLE notification, error code: %d", rc);
    return FALSE;
  }

  vTaskDelay(pdMS_TO_TICKS(20)); // Allow BLE stack ring buffer to clear out
  ESP_LOGI(TAG, "[BLE DEBUG] Response SENT (Size: %zu)", (size_t)stream.bytes_written);
  return TRUE;
}

/**
* Processes incoming protobuf AppToDevice requests.
*/
static void handle_protobuf_command(const IDBC_AppToDevice* appCmd) {
  if (!deviceReady) {
    ESP_LOGE(TAG, "[CRITICAL ERROR] Device was not ready to receive protobuf commands");
    return;
  }

  switch (appCmd->which_payload) {
    case IDBC_AppToDevice_image_transfer_request_tag: {
      const IDBC_ImageTransferRequest* req = &appCmd->payload.image_transfer_request;
      ESP_LOGI(TAG, "Proto Req: Transfer Image - Event ID: %lu, Start Img: %lu, Start Chunk: %lu", 
           req->event_id, req->start_from_image, req->start_from_chunk);
      
      const char* targetFolder = find_target_path(req->event_id);
      if (strlen(targetFolder) == 0) {
        ESP_LOGW(TAG, "[BLE Error] Failed to find target folder for Event ID %lu", req->event_id);
        break;
      }

      // Enqueue the request so nothing gets overwritten
      PendingImageRequest reqItem;
      reqItem.active = TRUE;
      reqItem.eventId = req->event_id;
      reqItem.startImage = req->start_from_image;
      reqItem.startChunk = req->start_from_chunk;
      snprintf(reqItem.targetFolder, sizeof(reqItem.targetFolder), "%s", targetFolder);

      if (xQueueSend(bleImageReqQueue, &reqItem, 0) != pdTRUE) {
        ESP_LOGE(TAG, "[BLE ERROR] Image request queue is full!");
      } else {
        ESP_LOGI(TAG, "[BLE] Image request queued successfully.");
      }
      break;
    }
    
    case IDBC_AppToDevice_event_transfer_ack_tag: {
      const IDBC_EventTransferAck* ack = &appCmd->payload.event_transfer_ack;
      ESP_LOGI(TAG, "[BLE] Got ACK from phone for Event ID %lu. Preparing safe cleanup...", ack->event_id);
      
      const char* folderPath = find_target_path(ack->event_id);
      char folderPathCopy[64];
      snprintf(folderPathCopy, sizeof(folderPathCopy), "%s", folderPath);

      if (strlen(folderPathCopy) > 0) {
        // Remove from linked list tracking first
        remove_incident_node(ack->event_id);
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Attempt recursive wipe with retry logic (Pure C POSIX)
        uint8_t deleted = FALSE;
        for (int attempt = 0; attempt < 3 && deleted == FALSE; attempt++) {
          if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(100));
          deleted = remove_dir_recursive(folderPathCopy);
        }

        if (deleted == TRUE) {
          ESP_LOGI(TAG, "[SD] Successfully wiped acknowledged directory: %s", folderPathCopy);
        } else {
          ESP_LOGE(TAG, "[SD ERROR] Failed to delete folder. Files may still be active or locked.");
        }
      } else {
        ESP_LOGW(TAG, "[BLE WARNING] Received ACK for Event ID %lu, but it wasn't found.", ack->event_id);
      }
      break;
    }

    case IDBC_AppToDevice_pending_event_list_request_tag: {
      ESP_LOGI(TAG, "[BLE] Pending Event List Requested by app.");

      IDBC_DeviceToApp resp = IDBC_DeviceToApp_init_default;
      resp.which_payload = IDBC_DeviceToApp_pending_event_list_tag;
      resp.payload.pending_event_list.event_ids_count = 0;

      // Populate event IDs from linked list safely under mutex protection
      if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        IncidentNode* current = pendingIncidentsHead;
        while (current != NULL && resp.payload.pending_event_list.event_ids_count < 16) {
          uint32_t idx = resp.payload.pending_event_list.event_ids_count;
          resp.payload.pending_event_list.event_ids[idx] = current->eventId;
          resp.payload.pending_event_list.event_ids_count++;
          current = current->next;
        }
        xSemaphoreGive(incidentMutex);
      }

      ESP_LOGI(TAG, "[BLE] Sending %u pending event IDs back to phone.", 
              resp.payload.pending_event_list.event_ids_count);

      send_device_to_app_response(&resp);
      break;
    }

    case IDBC_AppToDevice_event_info_request_tag: {
      const IDBC_EventInfoRequest* infoReq = &appCmd->payload.event_info_request;
      ESP_LOGI(TAG, "Proto Req: Event Info for Event ID %lu", infoReq->event_id);

      const char* targetFolder = find_target_path(infoReq->event_id);
      if (strlen(targetFolder) == 0) {
        ESP_LOGW(TAG, "[BLE WARNING] Requested event info for an ID not found.");
        break;
      }

      // POSIX VFS directory scan for image count
      DIR *dir = opendir(targetFolder);
      uint32_t imgCount = 0;
      if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
          if (entry->d_type == DT_REG) {
            if (strstr(entry->d_name, ".jpg") != NULL || strstr(entry->d_name, ".png") != NULL) {
              imgCount++;
            }
          }
        }
        closedir(dir);
      } else {
        ESP_LOGE(TAG, "[SD ERROR] Failed to open event directory via POSIX.");
      }

      notify_phone_of_event(
        infoReq->event_id,
        100,  // Distance placeholder
        -100, // Time offset placeholder
        imgCount,
        IDBC_ImageFormat_IMAGE_FORMAT_JPEG
      );

      ESP_LOGI(TAG, "[BLE] Responded with EventNotification for ID %lu (Images: %lu)", 
              infoReq->event_id, imgCount);
      break;
    }

    default:
      ESP_LOGW(TAG, "Unknown Protobuf payload tag: %d", (int)appCmd->which_payload);
      break;
  }
}

/**
* Helper function to send event notifications over BLE.
*/
void notify_phone_of_event(uint32_t eventId, uint32_t distanceCm, uint32_t timeOffsetMs, uint32_t imageCount, uint32_t format) {
  IDBC_DeviceToApp msg = IDBC_DeviceToApp_init_default;
  msg.which_payload = IDBC_DeviceToApp_event_notification_tag;
  
  msg.payload.event_notification.event_id = eventId;
  msg.payload.event_notification.distance_cm = distanceCm;
  msg.payload.event_notification.time_offset_ms = timeOffsetMs;
  msg.payload.event_notification.image_count = imageCount;
  msg.payload.event_notification.image_format = (IDBC_ImageFormat)format;

  send_device_to_app_response(&msg);
}
/**
 * Streams all image files found in the target incident folder using the C-native ImageChunker.
 */
static uint8_t stream_incident_images(uint32_t eventId, const char* folderPath, uint32_t startImageIndex, uint32_t startChunkIndex) {
  if (folderPath == NULL || strlen(folderPath) == 0) {
    ESP_LOGE(TAG, "Invalid folder path for streaming.");
    return FALSE;
  }

  DIR *dir = opendir(folderPath);
  if (dir == NULL) {
    ESP_LOGE(TAG, "Failed to open directory for streaming: %s", folderPath);
    return FALSE;
  }

  struct dirent *entry;
  uint32_t currentImageIndex = 0;
  uint8_t success = TRUE;

  // Iterate through files in the incident folder
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type == DT_REG) {
      // Check for image extensions
      if (strstr(entry->d_name, ".jpg") != NULL || strstr(entry->d_name, ".png") != NULL) {
       
        // Skip if we are resuming from a specific image index
        if (currentImageIndex < startImageIndex) {
          currentImageIndex++;
          continue;
        }

        char filePath[256];
        snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, entry->d_name);

        // Open image file via POSIX
        FILE *f = fopen(filePath, "rb");
        if (f == NULL) {
          ESP_LOGE(TAG, "Failed to open image file: %s", filePath);
          continue;
        }

        // Get file size
        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (fileSize <= 0 || fileSize > 1024 * 512) { // Safety cap (e.g., max 512KB per image)
          ESP_LOGW(TAG, "Skipping invalid or oversized image file: %s (Size: %ld)", filePath, fileSize);
          fclose(f);
          currentImageIndex++;
          continue;
        }

        // Allocate temporary buffer to hold the raw image bytes in memory
        uint8_t *imgBuffer = (uint8_t*)malloc(fileSize);
        if (imgBuffer == NULL) {
          ESP_LOGE(TAG, "Failed to allocate memory for image streaming: %s", filePath);
          fclose(f);
          success = FALSE;
          break;
        }

        size_t bytesRead = fread(imgBuffer, 1, fileSize, f);
        fclose(f);

        if (bytesRead != (size_t)fileSize) {
          ESP_LOGE(TAG, "Failed to read full image file into memory.");
          free(imgBuffer);
          currentImageIndex++;
          continue;
        }

        // Setup ImageData struct required by ImageChunker
        ImageData imgData;
        imgData.data = imgBuffer;
        imgData.size = fileSize;
          
        if (!detectImageFormat(imgData.data, imgData.size, &imgData.format)) {
          ESP_LOGW(TAG, "Unknown or unsupported image format for file: %s", filePath);
          free(imgBuffer);
          currentImageIndex++;
          continue;
        }

        size_t totalChunks = calculateTotalChunks(imgData.size);
        uint32_t startChunk = (currentImageIndex == startImageIndex) ? startChunkIndex : 0;

        ESP_LOGI(TAG, "Streaming Image [%lu]: %s (Chunks: %zu)", currentImageIndex, entry->d_name, totalChunks);

        // Loop through chunks and transmit them over BLE
        for (uint32_t c = startChunk; c < totalChunks; c++) {
          if (!deviceConnected) {
            ESP_LOGW(TAG, "Device disconnected mid-stream. Aborting transfer.");
            success = FALSE;
            break;
          }

          IDBC_ImageChunk chunkMsg;
          if (buildImageChunk(&imgData, eventId, currentImageIndex, c, &chunkMsg)) {
                
            // Wrap the chunk inside the DeviceToApp outer wrapper
            IDBC_DeviceToApp resp = IDBC_DeviceToApp_init_default;
            resp.which_payload = IDBC_DeviceToApp_image_chunk_tag;
            resp.payload.image_chunk = chunkMsg;

            // Send over BLE TX characteristic
            if (send_device_to_app_response(&resp) == FALSE) {
              ESP_LOGE(TAG, "Failed to transmit image chunk via BLE notification.");
            }
          }

          // Small pacing delay to prevent flooding the NimBLE host output queue
          vTaskDelay(pdMS_TO_TICKS(15));
        }

        free(imgBuffer);
        currentImageIndex++;

        if (!success) break;
      }
    }
  }

  closedir(dir);
  ESP_LOGI(TAG, "Finished streaming images from folder: %s", folderPath);
  return success;
}

/**
* Task loop handler to process queued image transfer requests.
*/
void handle_pending_bluetooth_tasks(void) {
  if (!bleImageReqQueue) return;

  PendingImageRequest reqItem;
  if (xQueueReceive(bleImageReqQueue, &reqItem, 0) == pdTRUE) {
    ESP_LOGI(TAG, "[LOOP] Processing queued transfer: Event %lu, Start Img %lu, Start Chunk %lu", 
        reqItem.eventId, reqItem.startImage, reqItem.startChunk);

    // Pass reqItem.eventId into the function
    stream_incident_images(reqItem.eventId, reqItem.targetFolder, reqItem.startImage, reqItem.startChunk);
  }
}

/**
* Configures advertising parameters and starts broadcasting.
*/
static void start_advertising(void) {
  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof(fields));

  fields.name = (uint8_t *)DEVICE_NAME;
  fields.name_len = strlen(DEVICE_NAME);
  fields.name_is_complete = 1;
  fields.appearance = 0x03C1; // Standard Keyboard Icon
  fields.appearance_is_present = 1;
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

  // Put custom data service UUID first in advertisement
  fields.uuids128 = (ble_uuid128_t[]){uuid_service_data};
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;

  ble_gap_adv_set_fields(&fields);

  struct ble_gap_adv_params adv_params;
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  // Start advertising with GAP event callback registered
  ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
}


/**
 * Test method to help run transfer acknowledgments/simulations
 */
void runTransferTest(uint32_t num, uint8_t counter) {
  refresh_list();
    
  if (counter == TRUE) {
    int count = 0;
    ESP_LOGI(TAG, "[BLE TEST] Running transfer test for %d incidents...", num);
        
    if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      IncidentNode* current = pendingIncidentsHead;
      while (current != NULL) {
        if (count >= num) { 
          break; 
        }
                
        ESP_LOGI(TAG, "[BLE TEST] Sending image transfer response for Event ID %lu, Folder: %s", 
                current->eventId, current->folderPath);
                
        // Trigger the C-native image chunk streamer
        stream_incident_images(current->eventId, current->folderPath, 0, 0);
                
        count++;
        current = current->next;
        }
      xSemaphoreGive(incidentMutex);
    }
  } else {
    ESP_LOGI(TAG, "[BLE TEST] Running transfer test for specific Event ID %d...", num);
        
    if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      IncidentNode* current = pendingIncidentsHead;
      while (current != NULL) {
        if (current->eventId == (uint32_t)num) {
          ESP_LOGI(TAG, "[BLE TEST] Found image transfer Event ID %lu", current->eventId);
                    
          // Trigger the C-native image chunk streamer
          stream_incident_images(current->eventId, current->folderPath, 0, 0);
          break;
        }
        current = current->next;
      }
      xSemaphoreGive(incidentMutex);
    }
  }
}