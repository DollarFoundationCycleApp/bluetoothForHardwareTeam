#include "BLEHandler.h"
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

#define TAG "BLE_HANDLER"

#define SERVICE_UUID_DATA "D4A51F4B-93EF-4AB1-B2B6-0E445CC297BA"
#define CHARACTERISTIC_UUID_RX "D4A51F4C-93EF-4AB1-B2B6-0E445CC297BA"
#define CHARACTERISTIC_UUID_TX "D4A51F4D-93EF-4AB1-B2B6-0E445CC297BA"

#undef ESP_LOGI
#undef ESP_LOGW
#undef ESP_LOGE

//todo delete this before prod
extern "C" void serial_print(const char* str);

#define ESP_LOGI(tag, format, ...) { char buf[512]; snprintf(buf, sizeof(buf), "[INFO] " format, ##__VA_ARGS__); serial_print(buf); }
#define ESP_LOGW(tag, format, ...) { char buf[512]; snprintf(buf, sizeof(buf), "[WARN] " format, ##__VA_ARGS__); serial_print(buf); }
#define ESP_LOGE(tag, format, ...) { char buf[512]; snprintf(buf, sizeof(buf), "[ERROR] " format, ##__VA_ARGS__); serial_print(buf); }

static uint8_t ble_is_synced = FALSE;
QueueHandle_t bleCmdQueue = NULL;

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

// Define 16-bit UUID variables statically to avoid taking rvalue addresses in C++
static const ble_uuid16_t uuid_svc_hid        = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t uuid_chr_hid_info   = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t uuid_chr_report_map = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t uuid_chr_protocol   = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t uuid_chr_hid_input  = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t uuid_dsc_report_ref = BLE_UUID16_INIT(0x2908);

// Global States
uint8_t deviceConnected = FALSE;
uint8_t deviceReady = FALSE;
uint8_t notificationsEnabled = FALSE;
uint16_t current_conn_handle = 0;
SemaphoreHandle_t incidentMutex = NULL;
QueueHandle_t bleImageReqQueue = NULL;

char recentDevices[MAX_SAVED_DEVICES][MAC_STR_LEN]; 
int savedDeviceCount = 0;
uint16_t tx_val_handle = 0;
/*
static bool build_child_path(char *dest, size_t dest_size, const char *parent, const char *child) {
    size_t parent_len = std::strlen(parent);
    size_t child_len = std::strlen(child);
    size_t prefix_len = parent_len;
    size_t separator_len = parent_len > 0 && parent[parent_len - 1] != '/' ? 1 : 0;

    if (parent_len == 0) {
        dest[0] = '/';
        prefix_len = 1;
    } else if (parent_len == 1 && parent[0] == '/') {
        prefix_len = 1;
    }

    if (prefix_len + separator_len + child_len + 1 > dest_size) {
        return false;
    }

    if (parent_len > 0) {
        std::memcpy(dest, parent, prefix_len);
    }
    if (separator_len > 0) {
        dest[prefix_len++] = '/';
    }
    std::memcpy(dest + prefix_len, child, child_len + 1);
    return true;
} // end ai suggested helper */  

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
    if (!ctxt || !ctxt->om) return 0; 
    const uint8_t hidInfoVal[] = {0x11, 0x01, 0x00, 0x02};
    return os_mbuf_append(ctxt->om, hidInfoVal, sizeof(hidInfoVal)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_report_map_access_cb(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!ctxt || !ctxt->om) return 0; 
    return os_mbuf_append(ctxt->om, hidReportDescriptor, sizeof(hidReportDescriptor)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_protocol_mode_access_cb(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!ctxt || !ctxt->om) return 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t mode = 0x01;
        return os_mbuf_append(ctxt->om, &mode, sizeof(mode)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int hid_report_ref_access_cb(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!ctxt || !ctxt->om) return 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        uint8_t refVal[] = {0x01, 0x01};
        return os_mbuf_append(ctxt->om, refVal, sizeof(refVal)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int hid_input_report_access_cb(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!ctxt || !ctxt->om) return 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t dummyReport[6] = {0};
        return os_mbuf_append(ctxt->om, dummyReport, sizeof(dummyReport)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int tx_characteristic_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (!ctxt) return BLE_ATT_ERR_INVALID_HANDLE;
    return 0;
}


static int rx_characteristic_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
  // Immediate entry check to verify data is hitting the callback
  if (!ctxt || !ctxt->om) {
    ESP_LOGE(TAG, "[BLE RX ERROR] Received null context or mbuf!");
    return BLE_ATT_ERR_INVALID_HANDLE;
  }

  uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
  ESP_LOGI(TAG, "[BLE RX] Incoming write packet detected! Length: %u bytes", data_len);

  if (data_len == 0 || data_len > 512) {
    ESP_LOGE(TAG, "[BLE RX ERROR] Invalid packet length: %u", data_len);
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }

  uint8_t rx_buffer[512];
  int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buffer, data_len, &data_len);
  if (rc != 0) {
    ESP_LOGE(TAG, "[BLE RX ERROR] Failed to flatten mbuf, rc: %d", rc);
    return BLE_ATT_ERR_UNLIKELY;
  }

  IDBC_AppToDevice appCmd = IDBC_AppToDevice_init_default;
  pb_istream_t stream = pb_istream_from_buffer(rx_buffer, data_len);

  if (pb_decode(&stream, IDBC_AppToDevice_fields, &appCmd)) {
    ESP_LOGI(TAG, "[BLE RX] Protobuf decode success! Enqueuing command...");
    if (bleCmdQueue) {
      xQueueSend(bleCmdQueue, &appCmd, 0);
    }
  } else {
    ESP_LOGE(TAG, "Protobuf Decode Error: %s", PB_GET_ERROR(&stream));
  }

  return 0;
}


// --- GATT Server Table ---
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
  {
    /* Service 1: Standard Bluetooth SIG HID Service (0x1812) */
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &uuid_svc_hid.u, 
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        .uuid = &uuid_chr_hid_info.u, 
        .access_cb = hid_info_access_cb,
        .flags = BLE_GATT_CHR_F_READ
      },
      {
        .uuid = &uuid_chr_report_map.u, 
        .access_cb = hid_report_map_access_cb,
        .flags = BLE_GATT_CHR_F_READ
      },
      {
        .uuid = &uuid_chr_protocol.u, 
        .access_cb = hid_protocol_mode_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP
      },
      {
        .uuid = &uuid_chr_hid_input.u, 
        .access_cb = hid_input_report_access_cb,
        .descriptors = (struct ble_gatt_dsc_def[]) {
          {
            .uuid = &uuid_dsc_report_ref.u,
            .att_flags = BLE_ATT_F_READ,
            .access_cb = hid_report_ref_access_cb
          },
          { 0 }
        },
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY
      },
      { 0 }
    }
  },
  {
    /* Service 2: Custom Service (For Protobuf / Data Transfer) */
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &uuid_service_data.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        .uuid = &uuid_char_rx.u,
        .access_cb = rx_characteristic_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
      },
      {
        .uuid = &uuid_char_tx.u,
        .access_cb = tx_characteristic_access_cb,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, 
        .val_handle = &tx_val_handle
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
        notificationsEnabled = FALSE; 
        
        struct ble_gap_conn_desc desc;
        char clientMac[MAC_STR_LEN] = "Unknown";
        if (ble_gap_conn_find(current_conn_handle, &desc) == 0) {
            std::snprintf(clientMac, sizeof(clientMac), "%02X:%02X:%02X:%02X:%02X:%02X",
                   desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                   desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                   desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
        }
        
        ESP_LOGI(TAG, ">>> CONNECTED NATIVELY! Address: %s <<<", clientMac);
        save_device_address(clientMac);

        struct ble_gap_upd_params params = {
            .itvl_min = 24,  // 30ms
            .itvl_max = 40,  // 50ms
            .latency = 0,
            .supervision_timeout = 400 // 4 seconds
        };
        ble_gap_update_params(current_conn_handle, &params);

      } else {
        ESP_LOGW(TAG, "Connection failed. Restarting advertising...");
        start_advertising();
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      deviceConnected = FALSE;
      notificationsEnabled = FALSE;
      current_conn_handle = 0;
      ESP_LOGI(TAG, ">>> BLE DISCONNECTED. Restarting Advertising... <<<");
      start_advertising();
      break;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == tx_val_handle) {
        notificationsEnabled = event->subscribe.cur_notify;
        if (notificationsEnabled) {
          ESP_LOGI(TAG, ">>> NOTIFICATIONS ENABLED BY CLIENT! READY TO STREAM. <<<");
        } else {
          ESP_LOGI(TAG, ">>> NOTIFICATIONS DISABLED BY CLIENT <<<");
        }
      }
      break;
      
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status == 0) {
          ESP_LOGI(TAG, ">>> Pairing & Encryption Complete! Native OS Bond Established. <<<");
      } else {
          ESP_LOGW(TAG, "Encryption/Pairing failed, status: %d", event->enc_change.status);
      }
      break;
      
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      {
          struct ble_sm_io pkey = {};
          pkey.action = event->passkey.params.action;

          if (pkey.action == BLE_SM_IOACT_NUMCMP) {
              pkey.numcmp_accept = 1;
              ble_sm_inject_io(event->passkey.conn_handle, &pkey);
              ESP_LOGI(TAG, "Numeric comparison accepted automatically.");
          } else if (pkey.action == BLE_SM_IOACT_DISP) {
              pkey.passkey = 123456; 
              ble_sm_inject_io(event->passkey.conn_handle, &pkey);
              ESP_LOGI(TAG, "Display passkey injected: 123456");
          }
      }
      break;

    default:
      break;
  }
  return 0;
}

static void nimble_host_task(void *param) {
  ESP_LOGI(TAG, "NimBLE host task entered");
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void on_stack_reset(int reason) {
  ESP_LOGW(TAG, "NimBLE host reset, reason: %d", reason);
}

static void on_stack_sync(void) {
    serial_print("[ble debug] CALLBACK: entered on_stack_sync");
    ble_is_synced = TRUE;
    
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(0x03C1);

    ESP_LOGI(TAG, "[BLE RUNTIME CONFIG] Free MSYS_1 blocks: %d", (int)os_msys_num_free());
    ESP_LOGI(TAG, "[BLE CONFIG] Active MSYS1 Block Size: %d bytes", CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE);
    start_advertising();
    serial_print("[ble debug] stack synced & advertising started");
    queue_incident_folders("/sd");
    /*
    if (get_pending_incident_count() == 0) {
      queue_incident_folders("/");
    }
    if (get_pending_incident_count() == 0) {
      queue_incident_folders("");
    }
    if (get_pending_incident_count() == 0) {
      queue_incident_folders("./");
    } // */

}

static void ble_app_on_sync(void) {
  ESP_LOGI(TAG, "[ble debug] CALLBACK: entered ble_app_on_sync");
  on_stack_sync();
}

static void ble_app_on_reset(int reason) {
  ESP_LOGW(TAG, "NimBLE host reset, reason: %d", reason);
}

/**
 * Main initialization routine for BLE, queues, and security policies.
 */
void init_ble(void) { 
  esp_log_level_set("*", ESP_LOG_VERBOSE);
  esp_log_level_set(TAG, ESP_LOG_VERBOSE);
  esp_log_level_set("INCIDENT_MANAGER", ESP_LOG_VERBOSE);

  ESP_LOGI(TAG, "[ble debug] start of init");

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  incidentMutex = xSemaphoreCreateMutex();
  bleImageReqQueue = xQueueCreate(10, sizeof(PendingImageRequest));

  nimble_port_init();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
  assert(rc == 0);
  
  ble_hs_cfg.reset_cb = ble_app_on_reset;
  ble_hs_cfg.sync_cb = ble_app_on_sync;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;

  load_saved_devices();
  print_saved_devices();

  rc = ble_gatts_count_cfg(gatt_svr_svcs);
  assert(rc == 0);

  rc = ble_gatts_add_svcs(gatt_svr_svcs);
  assert(rc == 0);
  
  vTaskDelay(pdMS_TO_TICKS(100));

  nimble_port_freertos_init(nimble_host_task); 

  bleCmdQueue = xQueueCreate(10, sizeof(IDBC_AppToDevice));

  deviceReady = TRUE;
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "BLE Server Started: %s", DEVICE_NAME);
  ESP_LOGI(TAG, "========================================");
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
      std::snprintf(keyBuf, sizeof(keyBuf), "dev_%d", i);
      
      size_t required_size = MAC_STR_LEN;
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
    std::memcpy(recentDevices[i], recentDevices[i - 1], MAC_STR_LEN);
  }
  std::snprintf(recentDevices[0], MAC_STR_LEN, "%s", macAddr);

  if (existingIdx == -1 && savedDeviceCount < MAX_SAVED_DEVICES) {
    savedDeviceCount++;
  }

  nvs_handle_t nvs_handle;
  if (nvs_open("ble_devices", NVS_READWRITE, &nvs_handle) == ESP_OK) {
    nvs_set_i32(nvs_handle, "count", savedDeviceCount);
    char keyBuf[16];
    for (int i = 0; i < savedDeviceCount; i++) {
      std::snprintf(keyBuf, sizeof(keyBuf), "dev_%d", i);
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
  if (!deviceConnected || !notificationsEnabled) {
    ESP_LOGW(TAG, "Skipping send: Device not connected or notifications not enabled yet.");
    return FALSE;
  }

  while (os_msys_num_free() < 20) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  uint8_t buffer[512];
  pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

  if (!pb_encode(&stream, IDBC_DeviceToApp_fields, response)) {
    ESP_LOGE(TAG, "Nanopb encoding failed: %s", PB_GET_ERROR(&stream));
    return FALSE;
  }

  struct os_mbuf* om = NULL;
  int timeout_ms = 4000;

  while ((om = ble_hs_mbuf_from_flat(buffer, stream.bytes_written)) == NULL) {
    if (timeout_ms <= 0) {
      ESP_LOGE(TAG, "[BLE ERROR] Mbuf allocation timeout. Free blocks: %d", (int)os_msys_num_free());
      return FALSE;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    timeout_ms -= 50;
  }

  int rc = ble_gatts_notify_custom(current_conn_handle, tx_val_handle, om);
  if (rc != 0) {
    ESP_LOGE(TAG, "[BLE ERROR] Failed to send notification, rc: %d", rc);
    os_mbuf_free_chain(om);
    return FALSE;
  }

  vTaskDelay(pdMS_TO_TICKS(40));
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
      ESP_LOGI(TAG, "Proto Req: Transfer Image - Event ID: %lu", (unsigned long)req->event_id);
      
      const char* targetFolder = find_target_path(req->event_id);
      if (targetFolder == NULL || std::strlen(targetFolder) == 0) {
        ESP_LOGW(TAG, "[BLE Error] Failed to find target folder for Event ID %lu", (unsigned long)req->event_id);
        break;
      }

      PendingImageRequest reqItem;
      reqItem.active = TRUE;
      reqItem.eventId = req->event_id;
      reqItem.startImage = req->start_from_image;
      reqItem.startChunk = req->start_from_chunk;
      std::snprintf(reqItem.targetFolder, sizeof(reqItem.targetFolder), "%s", targetFolder);

      if (xQueueSend(bleImageReqQueue, &reqItem, 0) != pdTRUE) {
        ESP_LOGE(TAG, "[BLE ERROR] Image request queue is full!");
      }
      break;
    }
    
    case IDBC_AppToDevice_event_transfer_ack_tag: {
      const IDBC_EventTransferAck* ack = &appCmd->payload.event_transfer_ack;
      
      const char* folderPath = find_target_path(ack->event_id);
      if (folderPath == NULL || std::strlen(folderPath) == 0) { 
        ESP_LOGW(TAG, "[BLE WARNING] Received ACK for Event ID %lu, but it wasn't found.", (unsigned long)ack->event_id);
        break;
      }

      char folderPathCopy[64];
      std::snprintf(folderPathCopy, sizeof(folderPathCopy), "%s", folderPath);
      
      remove_incident_node(ack->event_id);
      vTaskDelay(pdMS_TO_TICKS(50));
        
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
      break;
    }

    case IDBC_AppToDevice_event_info_request_tag: {
      const IDBC_EventInfoRequest* infoReq = &appCmd->payload.event_info_request;
      
      const char* targetFolder = find_target_path(infoReq->event_id);
      if (targetFolder == NULL || std::strlen(targetFolder) == 0) {
        ESP_LOGW(TAG, "[BLE WARNING] Requested event info for an ID not found.");
        break;
      }

      DIR *dir = opendir(targetFolder);
      uint32_t imgCount = 0;
      if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
          if (entry->d_type == DT_REG) {
            if (std::strstr(entry->d_name, ".jpg") != NULL || std::strstr(entry->d_name, ".png") != NULL) {
              imgCount++;
            }
          }
        }
        closedir(dir);
      }

      notify_phone_of_event(
        infoReq->event_id,
        100,
        -100,
        imgCount,
        IDBC_ImageFormat_IMAGE_FORMAT_JPEG
      );
      break;
    }

    default:
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
  if (folderPath == NULL || std::strlen(folderPath) == 0) {
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

  uint8_t *chunkWindowBuf = (uint8_t*)std::malloc(IMAGE_CHUNK_DATA_SIZE);
  if (chunkWindowBuf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for image streaming window buffer");
    closedir(dir);
    return FALSE;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type == DT_REG) {
      if (std::strstr(entry->d_name, ".jpg") != NULL || std::strstr(entry->d_name, ".png") != NULL) {
       
        if (currentImageIndex < startImageIndex) {
          currentImageIndex++;
          continue;
        }

        char filePath[257];
        std::snprintf(filePath, sizeof(filePath), "%s/%s", folderPath, entry->d_name);
        /* if (!build_child_path(filePath, sizeof(filePath), folderPath, entry->d_name)) { // ai suggestion
          ESP_LOGW(TAG, "Skipping image with an overlong path");
          continue;
        }  // */

        FILE *f = std::fopen(filePath, "rb");
        if (f == NULL) {
          ESP_LOGE(TAG, "Failed to open image file: %s", filePath);
          continue;
        }

        std::fseek(f, 0, SEEK_END);
        long fileSize = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);

        if (fileSize <= 0 || fileSize > 1024 * 512) {
          ESP_LOGW(TAG, "Skipping invalid or oversized image file: %s (Size: %ld)", filePath, fileSize);
          std::fclose(f);
          currentImageIndex++;
          continue;
        }

        size_t totalChunks = calculateTotalChunks(fileSize);
        uint32_t startChunk = (currentImageIndex == startImageIndex) ? startChunkIndex : 0;

        ESP_LOGI(TAG, "Streaming Image [%lu] via ImageChunker window: %s (Chunks: %zu, Size: %ld)", 
                 (unsigned long)currentImageIndex, entry->d_name, totalChunks, fileSize);

        for (uint32_t c = startChunk; c < totalChunks; c++) {
          if (!deviceConnected) {
            ESP_LOGW(TAG, "Device disconnected mid-stream. Aborting transfer.");
            success = FALSE;
            break;
          }

          size_t byteOffset = (size_t)c * IMAGE_CHUNK_DATA_SIZE;
          size_t bytesToRead = IMAGE_CHUNK_DATA_SIZE;
          
          if (byteOffset + bytesToRead > (size_t)fileSize) {
            bytesToRead = (size_t)fileSize - byteOffset;
          }

          std::fseek(f, (long)byteOffset, SEEK_SET);
          size_t bytesRead = std::fread(chunkWindowBuf, 1, bytesToRead, f);

          if (bytesRead != bytesToRead) {
            ESP_LOGE(TAG, "Failed to read chunk %lu at offset %zu", (unsigned long)c, byteOffset);
            continue;
          }

          IDBC_ImageChunk chunkMsg;
          if (buildImageChunkFromWindow(chunkWindowBuf, bytesRead, eventId, currentImageIndex, c, totalChunks, &chunkMsg)) {
            IDBC_DeviceToApp resp = IDBC_DeviceToApp_init_default;
            resp.which_payload = IDBC_DeviceToApp_image_chunk_tag;
            resp.payload.image_chunk = chunkMsg;

            if (send_device_to_app_response(&resp) == FALSE) {
              ESP_LOGE(TAG, "Failed to transmit image chunk via BLE notification.");
            }
          }

          vTaskDelay(pdMS_TO_TICKS(40));
        }

        std::fclose(f);
        currentImageIndex++;

        if (!success) break;
      }
    }
  }

  std::free(chunkWindowBuf);
  closedir(dir);
  ESP_LOGI(TAG, "Finished streaming images from folder: %s", folderPath);
  return success;
}

/**
 * Task loop handler to process queued image transfer requests.
 */
void handle_pending_bluetooth_tasks(void) {
  if (!deviceReady || !deviceConnected) return;

  // Process queued commands safely outside the NimBLE host thread
  IDBC_AppToDevice appCmd;
  if (bleCmdQueue && xQueueReceive(bleCmdQueue, &appCmd, 0) == pdTRUE) {
    handle_protobuf_command(&appCmd);
  }

  // Existing image transfer queue handler...
  PendingImageRequest reqItem;
  if (bleImageReqQueue && xQueueReceive(bleImageReqQueue, &reqItem, 0) == pdTRUE) {
    if (reqItem.targetFolder[0] != '\0') {
      stream_incident_images(reqItem.eventId, reqItem.targetFolder, reqItem.startImage, reqItem.startChunk);
    }
  }
}

/**
 * Configures advertising parameters and starts broadcasting.
 */
static void start_advertising(void) {
    if (!ble_is_synced) return;

    serial_print("[BLE Debug] Entering start_advertising()");
    
    ble_gap_adv_stop();
    struct ble_hs_adv_fields fields;
    std::memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    if (name) {
        fields.name = (uint8_t *)name;
        fields.name_len = std::strlen(name);
        fields.name_is_complete = 1;
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE Error] Failed to set adv fields, rc = %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    std::memset(&rsp_fields, 0, sizeof(rsp_fields));

    rsp_fields.uuids128 = (ble_uuid128_t *)&uuid_service_data;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "[BLE Warning] Failed to set scan response fields, rc = %d", rc);
    }
    struct ble_gap_adv_params adv_params;
    std::memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
        BLE_OWN_ADDR_PUBLIC,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        gap_event_handler,
        NULL
    );

    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE Error] Failed to start advertising, rc = %d", rc);
    } else {
        ESP_LOGI(TAG, "[BLE Debug] Advertising started cleanly with name and 128-bit UUIDs!");
    }
}

/**
 * Test method to help run transfer acknowledgments/simulations
 */
void runTransferTest(uint32_t num, uint8_t counter) {
  refresh_list();
    
  if (counter == TRUE) {
    int count = 0;
    ESP_LOGI(TAG, "[BLE TEST] Queuing transfer test for %d incidents...", (int)num);
        
    if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      IncidentNode* current = pendingIncidentsHead;
      while (current != NULL) {
        if (count >= (int)num) { 
          break; 
        }
            
        // Enqueue instead of calling stream_incident_images directly!
        PendingImageRequest reqItem;
        reqItem.active = TRUE;
        reqItem.eventId = current->eventId;
        reqItem.startImage = 0;
        reqItem.startChunk = 0;
        std::snprintf(reqItem.targetFolder, sizeof(reqItem.targetFolder), "%s", current->folderPath);

        if (bleImageReqQueue && xQueueSend(bleImageReqQueue, &reqItem, 0) == pdTRUE) {
          ESP_LOGI(TAG, "[BLE TEST] Queued Event ID %lu for transfer", (unsigned long)current->eventId);
        }
            
        count++;
        current = current->next;
      }
      xSemaphoreGive(incidentMutex);
    }
  } else {
    ESP_LOGI(TAG, "[BLE TEST] Queuing transfer test for specific Event ID %lu...", (unsigned long)num);
        
    if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      IncidentNode* current = pendingIncidentsHead;
      while (current != NULL) {
        if (current->eventId == num) {
          PendingImageRequest reqItem;
          reqItem.active = TRUE;
          reqItem.eventId = current->eventId;
          reqItem.startImage = 0;
          reqItem.startChunk = 0;
          std::snprintf(reqItem.targetFolder, sizeof(reqItem.targetFolder), "%s", current->folderPath);

          if (bleImageReqQueue && xQueueSend(bleImageReqQueue, &reqItem, 0) == pdTRUE) {
            ESP_LOGI(TAG, "[BLE TEST] Queued specific Event ID %lu for transfer", (unsigned long)num);
          }
          break;
        }
        current = current->next;
      }
      xSemaphoreGive(incidentMutex);
    }
  }
}