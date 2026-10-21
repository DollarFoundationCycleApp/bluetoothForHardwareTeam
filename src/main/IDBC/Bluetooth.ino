#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <SD.h>
#include <vector>
#include "esp_bt.h"
#include "ImageChunker.h" 
#include "IncidentManager.h"

// --- Nanopb Includes ---
#include <pb_decode.h>
#include <pb_encode.h>
#include "IBDC_v0.3.2.pb.h"

// --- Custom UUIDs for Data/File Transfers ---
#define SERVICE_UUID_DATA      "D4A51F4B-93EF-4AB1-B2B6-0E445CC297BA"
#define CHARACTERISTIC_UUID_RX "D4A51F4C-93EF-4AB1-B2B6-0E445CC297BA"
#define CHARACTERISTIC_UUID_TX "D4A51F4D-93EF-4AB1-B2B6-0E445CC297BA"

// --- Standard Bluetooth SIG HID Service UUIDs ---
#define HID_SERVICE_UUID           NimBLEUUID((uint16_t)0x1812)
#define HID_REPORT_MAP_UUID        NimBLEUUID((uint16_t)0x2A4B)
#define HID_INFORMATION_UUID       NimBLEUUID((uint16_t)0x2A4A)
#define HID_CONTROL_POINT_UUID     NimBLEUUID((uint16_t)0x2A4C)
#define PROTOCOL_MODE_UUID         NimBLEUUID((uint16_t)0x2A4E)

#define MAX_SAVED_DEVICES 3
#define MAC_STR_LEN       18 // Fixed length for "XX:XX:XX:XX:XX:XX"
#define DEVICE_NAME       "Bike_CAM"

//extern std::vector<IncidentMetadata> pendingIncidents;

NimBLECharacteristic *pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool deviceReady = false;

Preferences preferences;

char recentDevices[MAX_SAVED_DEVICES][MAC_STR_LEN]; 
int savedDeviceCount = 0;

// Function Prototypes
void loadSavedDevices();
void saveDeviceAddress(const char* macAddr);
void printSavedDevices();
bool sendDeviceToAppResponse(const IDBC_DeviceToApp* response);
void handleProtobufCommand(const IDBC_AppToDevice* appCmd);
void notifyPhoneOfEvent(uint32_t eventId, uint32_t distanceCm, uint32_t timeOffsetMs, uint32_t imageCount, uint32_t format);

// Minimal HID Keyboard Report Descriptor
const uint8_t hidReportDescriptor[] = {
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

// Server Callbacks
class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        deviceConnected = true;
        
        // Zero-allocation stack buffer for client address
        char clientMac[MAC_STR_LEN];
        snprintf(clientMac, sizeof(clientMac), "%s", connInfo.getAddress().toString().c_str());

        if (Serial) {
            Serial.print(F(">>> OS CONNECTED NATIVELY! Address: "));
            Serial.print(clientMac);
            Serial.println(F(" <<<"));
        }

        saveDeviceAddress(clientMac);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        if (Serial) {
            Serial.print(F(">>> BLE DISCONNECTED (Reason: "));
            Serial.print(reason);
            Serial.println(F("). Restarting Advertising... <<<"));
        }
        
        NimBLEDevice::startAdvertising();
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (connInfo.isEncrypted()) {
            if (Serial) {
                Serial.println(F(">>> Pairing & Encryption Complete! Bound to Phone OS. <<<"));
            }
        }
    }
};

// RX Callback for Custom Data / Protobuf File Handling
class RxCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() == 0) return;

        // Decode incoming Nanopb AppToDevice frame
        IDBC_AppToDevice appCmd = IDBC_AppToDevice_init_default;
        pb_istream_t stream = pb_istream_from_buffer(
            reinterpret_cast<const uint8_t*>(value.data()), 
            value.length()
        );

        if (pb_decode(&stream, IDBC_AppToDevice_fields, &appCmd)) {
            handleProtobufCommand(&appCmd); //Potentially overwhelming BLE
        } else {
            if (Serial) {
                Serial.printf("Protobuf Decode Error: %s\n", PB_GET_ERROR(&stream));
            }
        }
    }
};

void initBLE() {
    // Release Classic BT memory footprint back to system heap
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    loadSavedDevices();
    printSavedDevices();

    // Initialize NimBLE Stack
    NimBLEDevice::init(DEVICE_NAME);

    // Configure Security Capabilities for Native OS HID Pairing
    NimBLEDevice::setSecurityAuth(true, false, true); 
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); 

    NimBLEDevice::setPower(7);

    NimBLEServer *pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Service 1: Standard HID Keyboard Service
    NimBLEService *pHidService = pServer->createService(HID_SERVICE_UUID);

    NimBLECharacteristic *pHidInfo = pHidService->createCharacteristic(
                                        HID_INFORMATION_UUID,
                                        NIMBLE_PROPERTY::READ_ENC
                                     );
    const uint8_t hidInfoVal[] = {0x11, 0x01, 0x00, 0x02};
    pHidInfo->setValue(hidInfoVal, sizeof(hidInfoVal));

    NimBLECharacteristic *pReportMap = pHidService->createCharacteristic(
                                          HID_REPORT_MAP_UUID,
                                          NIMBLE_PROPERTY::READ_ENC
                                       );
    pReportMap->setValue(hidReportDescriptor, sizeof(hidReportDescriptor));

    NimBLECharacteristic *pProtocolMode = pHidService->createCharacteristic(
                                             PROTOCOL_MODE_UUID,
                                             NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_NR
                                          );
    // NimBLECharacteristic *pCtrl = pHidService->createCharacteristic(
    //                                (uint16_t)0x2A4C, NIMBLE_PROPERTY::WRITE_NR);
    
    NimBLECharacteristic *pInput = pHidService->createCharacteristic(
                                    (uint16_t)0x2A4D,
                                    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_NR);

    NimBLEDescriptor *pRef = pInput->createDescriptor(
                                    (uint16_t)0x2908,
                                    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC );
    uint8_t refVal[] = {0x01, 0x01};
    pRef->setValue(refVal, 2);

    uint8_t mode = 0x01;
    pProtocolMode->setValue(&mode, 1);

    // Service 2: Custom Service (For Protobuf / File / Data Transfer)
    NimBLEService *pDataService = pServer->createService(SERVICE_UUID_DATA);

    pTxCharacteristic = pDataService->createCharacteristic(
                                 CHARACTERISTIC_UUID_TX,
                                 NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
                               );

    NimBLECharacteristic *pRxCharacteristic = pDataService->createCharacteristic(
                                               CHARACTERISTIC_UUID_RX,
                                               NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_NR
                                             );
    pRxCharacteristic->setCallbacks(new RxCallbacks());

    // Service 3: Advertising Configuration
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName(DEVICE_NAME);
    pAdvertising->setAppearance(0x03C1); // Standard Keyboard Icon
    pAdvertising->setName(DEVICE_NAME);
    pAdvertising->addServiceUUID(SERVICE_UUID_DATA); // Put data service first
    pAdvertising->enableScanResponse(true);

    if (savedDeviceCount > 0 && strlen(recentDevices[0]) > 0) {
        NimBLEAddress targetAddr(std::string(recentDevices[0]), BLE_ADDR_PUBLIC);
        if (Serial) {
            Serial.print(F(">>> Attempting Direct Auto-Reconnect to Last Device: "));
            Serial.println(recentDevices[0]);
        }

        NimBLEDevice::whiteListAdd(targetAddr);
        pAdvertising->setScanFilter(false, false);
    } else {
        if (Serial) {
            Serial.println(F(">>> No saved devices. Advertising for new pairing..."));
        }
    }

    //pHidService->start();
    pAdvertising->start();
    refreshList();
    if (Serial) {
        Serial.println(F("========================================"));
        Serial.print(F("BLE Server Started: "));
        Serial.println(DEVICE_NAME);
        Serial.println(F("========================================"));
    }
    deviceReady = true;
    if (Serial) {
      Serial.println("Device is now ready for proto");
    }
}

/**
 * Loads saved MAC addresses from NVS flash storage.
 */
void loadSavedDevices() {
    preferences.begin("ble_devices", true);
    savedDeviceCount = preferences.getInt("count", 0);

    char keyBuf[12];
    for (int i = 0; i < MAX_SAVED_DEVICES; i++) {
        snprintf(keyBuf, sizeof(keyBuf), "dev_%d", i);
        String savedStr = preferences.getString(keyBuf, "");
        
        if (savedStr.length() > 0) {
            snprintf(recentDevices[i], MAC_STR_LEN, "%s", savedStr.c_str());
        } else {
            recentDevices[i][0] = '\0';
        }
    }
    preferences.end();
}

/**
 * Saves connected MAC address to flash storage.
 */
void saveDeviceAddress(const char* macAddr) {
    if (macAddr == nullptr || macAddr[0] == '\0') return;

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

    preferences.begin("ble_devices", false);
    preferences.putInt("count", savedDeviceCount);
    
    char keyBuf[12];
    for (int i = 0; i < MAX_SAVED_DEVICES; i++) {
        snprintf(keyBuf, sizeof(keyBuf), "dev_%d", i);
        preferences.putString(keyBuf, recentDevices[i]);
    }
    preferences.end();

    if (Serial) {
        Serial.println(F("Updated Recent Devices List:"));
        printSavedDevices();
    }
}

/**
 * Output recent devices to Serial Monitor.
 */
void printSavedDevices() {
    if (Serial) {
        Serial.println(F("--- Saved Recent Devices (Max 3) ---"));
        if (savedDeviceCount == 0) {
            Serial.println(F("  (None)"));
        } else {
            for (int i = 0; i < savedDeviceCount; i++) {
                Serial.print(F("  ["));
                Serial.print(i + 1);
                Serial.print(F("] "));
                Serial.println(recentDevices[i]);
            }
        }
        Serial.println(F("------------------------------------"));
    }
}

/**
 * Sends a DeviceToApp protobuf message back to the phone via pTxCharacteristic.
 */
bool sendDeviceToAppResponse(const IDBC_DeviceToApp* response) {
    if (!pTxCharacteristic || !deviceConnected) {
        if(Serial){
            Serial.printf("Error sending due to connection");
        }
        return false;
    }

    static uint8_t buffer[1024];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, IDBC_DeviceToApp_fields, response)) {
        if (Serial) {
            Serial.printf("Nanopb encoding failed: %s\n", PB_GET_ERROR(&stream));
        }
        return false;
    }

    pTxCharacteristic->setValue(buffer, stream.bytes_written);
    if (deviceConnected) {
    pTxCharacteristic->notify();
    Serial.println("[BLE DEBUG] SENT");
    return true;
    } else {
      if (Serial) {
        Serial.println("We lost connection so we didn't notify on pTX channel");
      }
      return false;
    }
}

/**
 * Processes incoming protobuf AppToDevice requests.
 */
void handleProtobufCommand(const IDBC_AppToDevice* appCmd) {
    if (!deviceReady) {
      if(Serial) {
        Serial.println("[CRITICAL ERROR CAUGHT] Device was not ready to recieve protobuf commands");
      }
      return;
    }
    switch (appCmd->which_payload) {
        case IDBC_AppToDevice_image_transfer_request_tag: {
          const auto& req = appCmd->payload.image_transfer_request;
          if (Serial) {
            Serial.printf("Proto Req: Transfer Image - Event ID: %lu, Start Img: %lu, Start Chunk: %lu\n", 
                              req.event_id, req.start_from_image, req.start_from_chunk);
          }
          String targetFolder = findTargetPath(req.event_id);

          if (targetFolder.length() == 0) {
            if (Serial) {
              Serial.println("[BLE Error] failed to find target folder");
            }
            break;
          }
          bool success = handleImageRequest(targetFolder, req.event_id, req.start_from_image, req.start_from_chunk);
          if (!success) {
            if (Serial) {
              Serial.println("[BLE Error] failed to handleImageRequest for Proto");
            }
          }  
          break;
      }

        case IDBC_AppToDevice_event_transfer_ack_tag: {
          const auto& ack = appCmd->payload.event_transfer_ack;
          if (Serial) {
              Serial.printf("[BLE] Got ACK from phone for Event ID %lu. Processing cleanup...\n", ack.event_id);
          }
            
          bool foundAndCleaned = false;
            
          // Search vector for the acknowledged event ID
          for (auto it = pendingIncidents.begin(); it != pendingIncidents.end(); ++it) {
            if (it->eventId == ack.event_id) {
              String folderPathToClean = it->folderPath;
                    
              // Remove from tracking vector first
              pendingIncidents.erase(it);
                    
              // Recursively nuke the folder and all its contents off the SD card
              if (removeDirRecursive(SD, folderPathToClean.c_str())) {
                if(Serial) {
                  Serial.printf("[SD] Successfully wiped acknowledged directory: %s\n", folderPathToClean.c_str());
                }
              } else {
                if(Serial) {
                  Serial.println("[SD ERROR] Failed to delete folder from SD card. Check if files are locked.");
                }
              }
                    
              foundAndCleaned = true;
              break;
            }
          }

          if (!foundAndCleaned) {
            if(Serial) {
              Serial.printf("[BLE WARNING] Received ACK for Event ID %lu, but it wasn't found in the pending vector.\n", ack.event_id);
            }
          }
          break;
        }

        case IDBC_AppToDevice_pending_event_list_request_tag: {
          if (Serial) {
            Serial.println("[BLE] Pending Event List Requested by app.");
          }

          IDBC_DeviceToApp resp = IDBC_DeviceToApp_init_default;
          resp.which_payload = IDBC_DeviceToApp_pending_event_list_tag;

          // Populate the static array defined in the nanopb header (max 16 entries)
          resp.payload.pending_event_list.event_ids_count = 0;
          for (const auto& inc : pendingIncidents) {
            if (resp.payload.pending_event_list.event_ids_count < 8) {
              uint32_t idx = resp.payload.pending_event_list.event_ids_count;
              resp.payload.pending_event_list.event_ids[idx] = inc.eventId;
              resp.payload.pending_event_list.event_ids_count++;
            } else {
              if (Serial) {
                  Serial.println("[BLE WARNING] Pending event list truncated; exceeded max array size of 8.");
              }
              break;
            }
          }

          if (Serial) {
            Serial.printf("[BLE] Sending %u pending event IDs back to phone.\n", 
                          resp.payload.pending_event_list.event_ids_count);
          }

          sendDeviceToAppResponse(&resp);
          break;
        }

        case IDBC_AppToDevice_event_info_request_tag: {
          Serial.println("[TEST] Entered EventInfoRequest handler");
          const auto& infoReq = appCmd->payload.event_info_request;
          if (Serial) {
              Serial.printf("Proto Req: Event Info for Event ID %lu\n", infoReq.event_id);
          }

          String targetFolder = findTargetPath(infoReq.event_id);

          if (targetFolder.length() == 0) {
            if (Serial) {
              Serial.println("[BLE WARNING] Requested event info for an ID not found.");
            }
            break;
          }
          
          File dir = SD.open(targetFolder);
          uint32_t imgCount = 0;
          if (dir && dir.isDirectory()) {
            File file = dir.openNextFile();
            while (file) {
              if (!file.isDirectory()) {
                String name = file.name();
                if (name.endsWith(".jpg") || name.endsWith(".png")) {
                  imgCount++;
                }
              }
              file.close();
              file = dir.openNextFile();
            }
            dir.close();
          } else {
            if (Serial) {
              Serial.println("[SD ERROR] Failed to open event directory.");
            }
          }
          //todo  get distance from csv 
          uint32_t distanceCM = -100; // based on distance in col 2 next to ms in col 1
          uint32_t timeOffset = 0; //based on image name
          notifyPhoneOfEvent(
            infoReq.event_id,
            distanceCM,
            timeOffset,
            imgCount,
            IDBC_ImageFormat_IMAGE_FORMAT_JPEG
          );

          if (Serial) {
            Serial.printf("[BLE] Responded with EventNotification for ID %lu (Images: %lu)\n", 
                          infoReq.event_id, imgCount);
          }
          break;
        }
        /*
        case IDBC_AppToDevice_app_time_response_tag: { // todo
            const auto& timeResp = appCmd->payload.app_time_response;
            if (Serial) {
                Serial.printf("Proto Resp: App Time Received - Time: %lu:%lu:%lu\n", 
                              timeResp.hours, timeResp.minute, timeResp.seconds);
            }
            // TODO: Update your internal RTC or system time using timeResp.timestamp_ms
            break;
        }
        case IDBC_AppToDevice_settings_tag: {
            const auto& settings = appCmd->payload.settings;
            if (Serial) {
                Serial.printf("Proto Req: Settings Update Request - Images Per Event: %lu\n", settings.images_per_event);
            }
            //todo request information regarding who is in charge of settings.. is that us?
            break;
        }
        */

        default:
            if (Serial) {
                Serial.printf("Unknown Protobuf payload tag: %d\n", appCmd->which_payload);
            }
            break;
    }
}

/**
 * Helper function to request the current time from the app.
 * /
void requestAppTime() {
    IDBC_DeviceToApp msg = IDBC_DeviceToApp_init_default;
    msg.which_payload = IDBC_DeviceToApp_app_time_request_tag;
    
    if (Serial) {
        Serial.println("Sending AppTimeRequest to phone...");
    }
    sendDeviceToAppResponse(&msg);
} // */

/**
 * Helper function to send event notifications over BLE.
 */
void notifyPhoneOfEvent(uint32_t eventId, uint32_t distanceCm, uint32_t timeOffsetMs, uint32_t imageCount, uint32_t format) {
    IDBC_DeviceToApp msg = IDBC_DeviceToApp_init_default;
    msg.which_payload = IDBC_DeviceToApp_event_notification_tag;
    
    msg.payload.event_notification.event_id = eventId;
    msg.payload.event_notification.distance_cm = distanceCm;
    msg.payload.event_notification.time_offset_ms = timeOffsetMs;
    msg.payload.event_notification.image_count = imageCount;
    msg.payload.event_notification.image_format = static_cast<IDBC_ImageFormat>(format);

    sendDeviceToAppResponse(&msg);
}

/**
  * Helper to build a response for an image based on an event tag
*/
bool handleImageRequest(String targetFolder, uint32_t eventId, uint32_t imageIndex, uint32_t startChunk) {
  if (targetFolder.length() == 0) {
    if(Serial){
      Serial.printf("[BLE ERROR] target folder for Event:%lu is invalid\n", eventId);
    }
    refreshList();
    targetFolder = findTargetPath((unsigned long)eventId);
    if (targetFolder.length() == 0) {
      if(Serial){
        Serial.println("[BLE Error] Attempted refresh failed.");
      }
      return false;
    }
  }
  Serial.println("[DEBUG] Before SD.open");
  Serial.println("Target folder: " + targetFolder);
  targetFolder.replace("//", "/");
  File dir = SD.open(targetFolder);/// ISSUE HERE
  Serial.println("[DEBUG] After SD.open");
  if (!dir || !dir.isDirectory()) {
    if (Serial) {
      Serial.printf("[SD ERROR] Failed to find or open incident folder path. %s\n", targetFolder.c_str());
    }
    if (dir) dir.close();
     dir = SD.open("/" + targetFolder);
     if(!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return false;
     }
  }
    Serial.println("[DEBUG] Target folder opened successfully");

  std::vector<String> imageFiles; 
  // build vector
  File file = dir.openNextFile();
  String name = "";
  while (file) {
    if (!file.isDirectory()) {
      name = file.name();
      if (name.endsWith(".jpg") || name.endsWith(".png")) {
        imageFiles.push_back(name);
      }
    }
    file.close();
    file = dir.openNextFile();
  }
  dir.close();
  Serial.println("[BLE DEBUG] target directory closed");

  if (imageFiles.empty()) {
    if (Serial) {
      Serial.println("[BLE ERROR] No image files found in target folder.");
    }
    return false;
  }

  //double check
  if (imageIndex >= imageFiles.size()) {
    if (Serial){
      Serial.println("[BLE ERROR] Requested image index is out of bounds");
    }
    return false;
  }
  Serial.println("Trying to open target folder with image specification");
  // open target, load it
  String imgPath = targetFolder+ "/" + imageFiles[imageIndex];
  File imgFile = SD.open(imgPath, FILE_READ);
  if (!imgFile) {
    if (Serial) {
      Serial.println("[SD ERROR] Failed to open target image file for reading.");
      return false;
    }
  }
  
  size_t imgSize = imgFile.size();

  // Verify that image file is not empty
  if(imgSize == 0) {
    if (Serial) {
      Serial.println("[BLE ERROR] Image file is empty.");
    }

    imgFile.close();
    return false;
  }
  Serial.println("[BLE DEBUG] testing right before image buffer");

  uint8_t *imgBuf = nullptr;
  if (psramFound()) {
      imgBuf = (uint8_t *)heap_caps_malloc(imgSize, MALLOC_CAP_SPIRAM);
  }
  
  // Fall back to regular internal DRAM if PSRAM allocation didn't work
  if (!imgBuf) {
      imgBuf = (uint8_t *)malloc(imgSize);
  } //todo jacob do we actually need to have a malloc here?

  if (!imgBuf) {
    if (Serial) {
      Serial.println("[BLE ERROR] Malloc failed! Not enough heap for this Image Buffer");
      imgFile.close();
      return false;
    }
  }
  size_t bytesRead = imgFile.read(imgBuf, imgSize);
  imgFile.close();

  // Verify that all bytes for the image were successfully read
  if (bytesRead != imgSize) {
    if (Serial) {
      Serial.println("[BLE ERROR] Failed to read the complete image.");
    }

    free(imgBuf);
    return false;
  }
  Serial.println("[BLE DEBUG] now we format");
  IDBC_ImageFormat format;
  if (!detectImageFormat(imgBuf, imgSize, &format)) {
    if (Serial) {
      Serial.println("[BLE ERROR] Unsupported or invalid image format.");
    }

    free(imgBuf);
    return false;
  }

  ImageData img = {
      .data = imgBuf,
      .size = imgSize,
      .format = format
  };
   size_t totalChunks = calculateTotalChunks(imgSize);
   if (Serial){
    Serial.printf("[BLE DEBUG] expected chunk count %u, image size %u\n", totalChunks, img.size);
   }
  //send imageInfo metapacket first so mobile knows whats up
  IDBC_DeviceToApp infoResp = IDBC_DeviceToApp_init_default;
  infoResp.which_payload = IDBC_DeviceToApp_image_info_tag;
  infoResp.payload.image_info.event_id = eventId;
  infoResp.payload.image_info.image_index = imageIndex; // Scoped parameter
  infoResp.payload.image_info.image_size_bytes = imgSize;
  infoResp.payload.image_info.total_chunks = totalChunks;
  infoResp.payload.image_info.image_format = format;
  sendDeviceToAppResponse(&infoResp);
  Serial.println("[BLE DEBUG] WE SHOULD HAVE SENT");
  vTaskDelay(pdMS_TO_TICKS(40)); // Give the BLE stack a breather

  // stream chunks
  for (uint32_t c = startChunk; c < totalChunks; c++) {
    IDBC_ImageChunk chunk;
    if (buildImageChunk(&img, eventId, imageIndex, c, &chunk)) {
      IDBC_DeviceToApp chunkResp = IDBC_DeviceToApp_init_default;
      chunkResp.which_payload = IDBC_DeviceToApp_image_chunk_tag;
      
      // Assign fields directly to the image_chunk struct inside the payload union
      chunkResp.payload.image_chunk = chunk;
      sendDeviceToAppResponse(&chunkResp);

      vTaskDelay(pdMS_TO_TICKS(40)); // real life needs delays for functions to work
    }
  }
  Serial.println("[BLE DEBUG] CHUNKS sent");

  free(imgBuf);
  return true;
}

/**
* helper to find target folder
*/
String findTargetPath(unsigned long event_id) {
  String targetFolder = "";
  bool found = false;
  if (Serial) {
    Serial.printf("[BLE Debug] pendingIncidents has %d incidents\n", pendingIncidents.size());
    //Serial.println("[BLE DEBUG] double check max size isn't the error");
    Serial.printf("[BLE Debug] pendingIncidents has %u max size \n", pendingIncidents.max_size());
  }
  for (const auto& inc: pendingIncidents) { /// pendingIncidents possibly empty: why?
    if (inc.eventId == event_id) {
      targetFolder = inc.folderPath;
      found = true;
      break;
    }
  }
  if (!found) {
    refreshList();
    for (const auto& inc: pendingIncidents) { /// pendingIncidents possibly empty: why?
      if (inc.eventId == event_id) {
        targetFolder = inc.folderPath;
        if (Serial) {
          Serial.println("Found the target folder");
        }
        found = true;
        break;
      }
    }
  }
  return targetFolder;
}

/** 
* Test method to help run transfer acknowledgments
*/ 
void runTransferTest(int num, bool counter){
  //refreshList();
  if (counter) {
    int count = 0;
    if (Serial) {
      Serial.printf("[BLE TEST] Running transfer test for %d incidents...\n", num);
    }
    for(const auto& inc: pendingIncidents){
      if (count >= num) {break;}
      if (Serial) {
        Serial.printf("[BLE TEST] Sending image transfer response for Event ID %lu, Folder: %s\n", inc.eventId, inc.folderPath.c_str());
      }
      handleImageRequest(inc.folderPath, inc.eventId, 0, 0);
      count++;
    }
  } else {
    if (Serial) {
      Serial.printf("[BLE TEST] Running transfer test for incident num %d...\n", num);
    }
    for (const auto& inc: pendingIncidents) {
      if (inc.eventId == num) {
        if (Serial) {
          Serial.printf("[BLE TEST] Found image transfer Event ID\n");
        }
        handleImageRequest(inc.folderPath, inc.eventId, 0, 0);
        break;
      }
    }
  }
}