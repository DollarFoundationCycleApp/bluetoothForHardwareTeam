#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_bt.h"

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

NimBLECharacteristic *pTxCharacteristic = nullptr;
bool deviceConnected = false;

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
            handleProtobufCommand(&appCmd);
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

    if (Serial) {
        Serial.println(F("========================================"));
        Serial.print(F("BLE Server Started: "));
        Serial.println(DEVICE_NAME);
        Serial.println(F("Pairable directly in Phone Bluetooth Settings!"));
        Serial.println(F("========================================"));
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

    uint8_t buffer[IDBC_DeviceToApp_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));

    if (!pb_encode(&stream, IDBC_DeviceToApp_fields, response)) {
        if (Serial) {
            Serial.printf("Nanopb encoding failed: %s\n", PB_GET_ERROR(&stream));
        }
        return false;
    }

    pTxCharacteristic->setValue(buffer, stream.bytes_written);
    pTxCharacteristic->notify();
    return true;
}

/**
 * Processes incoming protobuf AppToDevice requests.
 */
void handleProtobufCommand(const IDBC_AppToDevice* appCmd) {
    switch (appCmd->which_payload) {
        case IDBC_AppToDevice_image_transfer_request_tag: {
            const auto& req = appCmd->payload.image_transfer_request;
            if (Serial) {
                Serial.printf("Proto Req: Transfer Image - Event ID: %lu, Start Img: %lu, Start Chunk: %lu\n", 
                              req.event_id, req.start_from_image, req.start_from_chunk);
            }
            // TODO: Trigger image chunk transfer logic
            break;
        }

        case IDBC_AppToDevice_event_transfer_ack_tag: {
            const auto& ack = appCmd->payload.event_transfer_ack;
            if (Serial) {
                Serial.printf("Proto ACK: Event ID %lu confirmed by phone.\n", ack.event_id);
            }
            // TODO: Mark event as completed or purge from storage
            break;
        }

        case IDBC_AppToDevice_settings_tag: {
            const auto& settings = appCmd->payload.settings;
            if (Serial) {
                Serial.printf("Proto Req: Settings Update Request - Images Per Event: %lu\n", settings.images_per_event);
            }
            break;
        }

        case IDBC_AppToDevice_pending_event_list_request_tag: {
            if (Serial) {
                Serial.println("Proto Req: Pending Event List Requested.");
            }
            
            IDBC_DeviceToApp resp = IDBC_DeviceToApp_init_default;
            resp.which_payload = IDBC_DeviceToApp_pending_event_list_tag;
            resp.payload.pending_event_list.event_ids_count = 0; // Populate with active event count
            sendDeviceToAppResponse(&resp);
            break;
        }

        case IDBC_AppToDevice_event_info_request_tag: {
            const auto& infoReq = appCmd->payload.event_info_request;
            if (Serial) {
                Serial.printf("Proto Req: Event Info for Event ID %lu\n", infoReq.event_id);
            }
            break;
        }

        default:
            if (Serial) {
                Serial.printf("Unknown Protobuf payload tag: %d\n", appCmd->which_payload);
            }
            break;
    }
}

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