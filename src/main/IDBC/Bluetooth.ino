#include <Arduino.h>
#include <NimBLEDevice.h>

#define SERVICE_UUID           "D4A51F4B-93EF-4AB1-B2B6-0E445CC297BA"
#define CHARACTERISTIC_UUID_RX "D4A51F4C-93EF-4AB1-B2B6-0E445CC297BA"
#define CHARACTERISTIC_UUID_TX "D4A51F4D-93EF-4AB1-B2B6-0E445CC297BA"

NimBLECharacteristic *pTxCharacteristic = nullptr;
bool deviceConnected = false;

// ble Server Callbacks
class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        deviceConnected = true;
        Serial.printf(">>> BLE CONNECTED! Device Address: %s <<<\n", 
                      connInfo.getAddress().toString().c_str());
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        Serial.println(">>> BLE DISCONNECTED. Restarting Advertising... <<<");
        NimBLEDevice::startAdvertising();
    }
};

// RX Callback (Receives data from phone)
class RxCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string value = pCharacteristic->getValue();
        if (value.length() > 0) {
            Serial.print("Received Data over BLE: ");
            for (size_t i = 0; i < value.length(); i++) {
                Serial.printf("%02X ", (uint8_t)value[i]);
            }
            Serial.println();
        }
    }
};

void initBLE() {
    // Initialize NimBLE
    NimBLEDevice::init("XIAO_TEST");

    // transmission power *do not touch*
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);


    NimBLEServer *pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    NimBLEService *pService = pServer->createService(SERVICE_UUID);
    pTxCharacteristic = pService->createCharacteristic(
                             CHARACTERISTIC_UUID_TX,
                             NIMBLE_PROPERTY::NOTIFY
                           );
    NimBLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                              CHARACTERISTIC_UUID_RX,
                                              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
                                            );
    pRxCharacteristic->setCallbacks(new RxCallbacks());
    pServer->start();

    //Fits in <31 bytes
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName("XIAO_TEST");
    pAdvertising->enableScanResponse(true);
    pAdvertising->start();
    if(Serial) {
    Serial.println("========================================");
    Serial.println("NimBLE Minimal Server Running!");
    Serial.println("Device Name: XIAO_TEST");
    Serial.println("========================================");
    }
}
