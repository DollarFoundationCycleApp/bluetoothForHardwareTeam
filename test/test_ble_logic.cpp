#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>

// --- Replicated Constants from BLEHandler.h ---
#define MAX_SAVED_DEVICES 3
#define MAC_STR_LEN       18
#define TRUE              1
#define FALSE             0

// --- Globals under test ---
char recentDevices[MAX_SAVED_DEVICES][MAC_STR_LEN];
int savedDeviceCount = 0;
uint8_t deviceConnected = FALSE;
uint8_t deviceReady = FALSE;

// --- Mocked save function matching BLEHandler.cpp logic ---
void mock_save_device_address(const char* macAddr) {
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
    memcpy(recentDevices[i], recentDevices[i - 1], MAC_STR_LEN);
  }
  snprintf(recentDevices[0], MAC_STR_LEN, "%s", macAddr);

  if (existingIdx == -1 && savedDeviceCount < MAX_SAVED_DEVICES) {
    savedDeviceCount++;
  }
}

// --- Unity Test Setup / Teardown ---
void setUp(void) {
  savedDeviceCount = 0;
  for (int i = 0; i < MAX_SAVED_DEVICES; i++) {
    recentDevices[i][0] = '\0';
  }
  deviceConnected = FALSE;
  deviceReady = FALSE;
}

void tearDown(void) {
  // Cleanup if needed
}

// --- Test Cases ---

void test_fifo_device_storage_limit(void) {
  mock_save_device_address("AA:BB:CC:DD:EE:01");
  mock_save_device_address("AA:BB:CC:DD:EE:02");
  mock_save_device_address("AA:BB:CC:DD:EE:03");

  TEST_ASSERT_EQUAL_INT(3, savedDeviceCount);
  // Most recent should be at index 0
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:03", recentDevices[0]);

  // Adding a 4th device should roll out the oldest ("AA:BB:CC:DD:EE:01")
  mock_save_device_address("AA:BB:CC:DD:EE:04");
  
  TEST_ASSERT_EQUAL_INT(3, savedDeviceCount);
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:04", recentDevices[0]);
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:03", recentDevices[1]);
  TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:02", recentDevices[2]);
}

void test_duplicate_device_deduplication(void) {
  mock_save_device_address("11:22:33:44:55:66");
  mock_save_device_address("77:88:99:AA:BB:CC");

  // Re-adding the older device should bubble it to index 0 without increasing count
  mock_save_device_address("11:22:33:44:55:66");

  TEST_ASSERT_EQUAL_INT(2, savedDeviceCount);
  TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", recentDevices[0]);
  TEST_ASSERT_EQUAL_STRING("77:88:99:AA:BB:CC", recentDevices[1]);
}

// --- Runner Main ---
int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_fifo_device_storage_limit);
  RUN_TEST(test_duplicate_device_deduplication);
  return UNITY_END();
}