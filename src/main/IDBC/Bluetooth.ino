#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_bt.h"

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

void loadSavedDevices();
void saveDeviceAddress(const char* macAddr);
void printSavedDevices();

// Minimal HID Keyboard Report Descriptor - AI generated
const uint8_t hidReportDescriptor[] = {
    0x05, 0x01, // USAGE_PAGE (Generic Desktop)
    0x09, 0x06, // USAGE (Keyboard)
    0xa1, 0x01, // COLLECTION (Application)
    0x85, 0x01, //   REPORT_ID (1)
    0x05, 0x07, //   USAGE_PAGE (Keyboard)
    0x19, 0xe0, //   USAGE_MINIMUM (Keyboard Right GUI)
    0x29, 0xe7, //   USAGE_MAXIMUM (Keyboard Right GUI)
    0x15, 0x00, //   LOGICAL_MINIMUM (0)
    0x25, 0x01, //   LOGICAL_MAXIMUM (1)
    0x75, 0x01, //   REPORT_SIZE (1)
    0x95, 0x08, //   REPORT_COUNT (8)
    0x81, 0x02, //   INPUT (Data,Var,Abs)
    0x95, 0x01, //   REPORT_COUNT (1)
    0x75, 0x08, //   REPORT_SIZE (8)
    0x81, 0x01, //   INPUT (Cnst,Ary,Abs)
    0xc0        // END_COLLECTION
};

// Server Callbacks
class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        deviceConnected = true;
        
        // Zero-allocation stack buffer for client address
        char clientMac[MAC_STR_LEN];
        snprintf(clientMac, sizeof(clientMac), "%s", connInfo.getAddress().toString().c_str());

        Serial.print(F(">>> OS CONNECTED NATIVELY! Address: "));
        Serial.print(clientMac);
        Serial.println(F(" <<<"));

        saveDeviceAddress(clientMac);
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        Serial.print(F(">>> BLE DISCONNECTED (Reason: "));
        Serial.print(reason);
        Serial.println(F("). Restarting Advertising... <<<"));
        
        NimBLEDevice::startAdvertising();
    }

    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        if (connInfo.isEncrypted()) {
            Serial.println(F(">>> Pairing & Encryption Complete! Bound to Phone OS. <<<"));
        }
    }
};

// RX Callback for Custom Data / File Handling
class RxCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            Serial.print(F("Received Custom Data Packet ("));
            Serial.print(value.length());
            Serial.print(F(" bytes): "));
            for (size_t i = 0; i < value.length(); i++) {
                Serial.printf("%02X ", (uint8_t)value[i]);
            }
            Serial.println();
        }
    }
};

void initBLE() {
    // Release Classic BT memory footprint back to system heap - mem issues otherwise (attempt 1 to get back under psram)
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    loadSavedDevices();
    printSavedDevices();

    // Initialize NimBLE Stack
    NimBLEDevice::init(DEVICE_NAME);

    // Clear existing ESP32 bond keys - Only use when upating connection algorithm
    //NimBLEDevice::deleteAllBonds();

    // Configure Security Capabilities for Native OS HID Pairing - stay connected after .2 ms
    NimBLEDevice::setSecurityAuth(true, false, true); 
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT); 

    NimBLEDevice::setPower(7);

    NimBLEServer *pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // -------------------------------------------------------------
    // Service 1: Standard HID Keyboard Service - Negligible and could be truncated if need be.
    // -------------------------------------------------------------
    NimBLEService *pHidService = pServer->createService(HID_SERVICE_UUID);

    NimBLECharacteristic *pHidInfo = pHidService->createCharacteristic(
                                        HID_INFORMATION_UUID,
                                        NIMBLE_PROPERTY::READ
                                     );
    const uint8_t hidInfoVal[] = {0x11, 0x01, 0x00, 0x02};
    pHidInfo->setValue(hidInfoVal, sizeof(hidInfoVal));

    NimBLECharacteristic *pReportMap = pHidService->createCharacteristic(
                                          HID_REPORT_MAP_UUID,
                                          NIMBLE_PROPERTY::READ
                                       );
    pReportMap->setValue(hidReportDescriptor, sizeof(hidReportDescriptor));

    NimBLECharacteristic *pProtocolMode = pHidService->createCharacteristic(
                                             PROTOCOL_MODE_UUID,
                                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR
                                          );
    uint8_t mode = 0x01;
    pProtocolMode->setValue(&mode, 1);

    // -------------------------------------------------------------
    // Service 2: Custom Service (For File / Data Transfer) - what we're here to use
    // -------------------------------------------------------------
    NimBLEService *pDataService = pServer->createService(SERVICE_UUID_DATA);

    pTxCharacteristic = pDataService->createCharacteristic(
                                 CHARACTERISTIC_UUID_TX,
                                 NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
                               );

    NimBLECharacteristic *pRxCharacteristic = pDataService->createCharacteristic(
                                               CHARACTERISTIC_UUID_RX,
                                               NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                                             );
    pRxCharacteristic->setCallbacks(new RxCallbacks());

    // -------------------------------------------------------------
    // Service 3: Auto Reconnect to devices in our list
    // -------------------------------------------------------------
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName(DEVICE_NAME);
    pAdvertising->setAppearance(0x03C1); // Standard Keyboard Icon for phone UI
    pAdvertising->addServiceUUID(HID_SERVICE_UUID);
    pAdvertising->addServiceUUID(SERVICE_UUID_DATA);
    pAdvertising->enableScanResponse(false); // Reduced advertising allocation payload

    // Check if we have a valid saved address
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

    pAdvertising->start();

    if(Serial) {
        Serial.println(F("========================================"));
        Serial.print(F("BLE Server Started: "));
        Serial.println(DEVICE_NAME);
        Serial.println(F("Pairable directly in Phone Bluetooth Settings!"));
        Serial.println(F("========================================"));
    }
}

/**
 * Loads saved MAC addresses from NVS flash storage using fixed char buffers.
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
 * Saves connected MAC address to flash storage
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

    if(Serial) {
        Serial.println(F("Updated Recent Devices List:"));
        printSavedDevices();
    }
}

/**
 * Output recent devices to Serial Monitor.
 */
void printSavedDevices() {
    if(Serial) {
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