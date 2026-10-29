// libraries
#include <Arduino.h>
#include <esp_camera.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

#include "detector.h"
#include "BLEHandler.h"
#include "IncidentManager.h"
#include "IBDC_v0.3.2.pb.h"
// Forward declarations for BLE symbols in Bluetooth
extern uint8_t deviceConnected;
void init_ble();
void notify_phone_of_event(uint32_t eventId, uint32_t distanceCm, uint32_t timeOffsetMs, uint32_t imageCount, uint32_t format);
//Button
const int buttonPin = 4;
unsigned long lastButtonPressMs = 0;
const unsigned long debounceDelayMs = 200; // 200ms debounce
//uint32_t testEventId = 1; //100;


// ultrasonic variables
const int trigPin = 1;
const int echoPin = 2;

// LEDs
const int redLEDPin = 43;
const int yellowLEDPin = 5;
const int greenLEDPin = 3;

//define sound speed in cm/uS
#define SOUND_SPEED 0.034
#define CM_TO_INCH 0.393701

long duration;
float distanceInch;

// camera variables
// pins for CAMERA_MODEL_XIAO_ESP32S3
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

// Camera status variable
bool camera_status = false;

// LED state
#define RED_LED_DURATION_MS 1000
volatile bool     redLedActive  = false;
volatile uint32_t redLedStartMs = 0;

// Blinks all three LEDs twice to signal a non-fatal error, then returns.
void errorFlash() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(redLEDPin,    HIGH);
    digitalWrite(yellowLEDPin, HIGH);
    digitalWrite(greenLEDPin,  HIGH);
    delay(150);
    digitalWrite(redLEDPin,    LOW);
    digitalWrite(yellowLEDPin, LOW);
    digitalWrite(greenLEDPin,  LOW);
    delay(150);
  }
}

// SD card variables
bool sd_status = false;

char folderPath[] = "/RecordedData1";

// Incident counter (only written by storageTask on Core 0)
int incidentCount = 0;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Storage queue — all SD and camera work is serialized through this

enum CmdType : uint8_t {
  CMD_MEASUREMENT,
  CMD_CHUNK_START,
  CMD_POTENTIAL_EVENT,
  CMD_INCIDENT_CONFIRMED,
  CMD_CHUNK_END,
  CMD_LOG_EVENT,
};

struct StorageCmd {
  CmdType type;
  union {
    struct { float distanceInch; uint32_t timestamp; } measurement;
    struct { uint32_t timestamp; }                    chunkStart;
    detector_event_t                                  incident;
    struct { uint32_t timestamp; char msg[48]; }      logEvent;
  };
};

QueueHandle_t storageQueue;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Helper methods

void CameraParameters() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_SVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }
}

bool writeFile(fs::FS &fs, const char *path, uint8_t *data, size_t len) {
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    char msg[48];
    snprintf(msg, sizeof(msg), "open failed: %.32s", path);
    writeLog(millis(), "ERR", msg);
    return false;
  }
  if (file.write(data, len) != len) {
    char msg[48];
    snprintf(msg, sizeof(msg), "write failed: %.31s", path);
    writeLog(millis(), "ERR", msg);
    file.close();
    return false;
  }
  file.close();
  return true;
}

// Take a text file on the SD card and append data to it
void appendToCSV(const char *fileName, const char *data) {
  File file = SD.open(fileName, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open CSV file for appending");
    return;
  }

  if (file.println(data)) {
    Serial.println("Data appended to CSV");
  } else {
    Serial.println("Append failed");
  }
  file.close();
}

// Captures a frame into a heap-allocated buffer (PSRAM preferred, DRAM fallback).
// Caller must free with free() when done.
uint8_t *photo_capture(size_t *outLen) {
  // Retry up to 3 times — esp_camera_fb_get() can return NULL transiently
  // when the camera driver's DMA buffer is mid-swap. A short yield is enough.
  camera_fb_t *fb = nullptr;
  for (int attempt = 0; attempt < 3 && !fb; attempt++) {
    if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(30));
    fb = esp_camera_fb_get();
  }
  if (!fb) {
    writeLog(millis(), "ERR", "camera frame buffer unavailable");
    *outLen = 0;
    return nullptr;
  }

  // Try PSRAM first, fall back to DRAM
  uint8_t *buf = nullptr;
  if (psramFound()) {
    buf = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
  }
  if (!buf) {
    buf = (uint8_t *)malloc(fb->len);
  }

  if (buf) {
    memcpy(buf, fb->buf, fb->len);
    *outLen = fb->len;
    Serial.printf("Photo captured: %u bytes (%s)\n", fb->len,
                  esp_ptr_external_ram(buf) ? "PSRAM" : "DRAM");
  } else {
    char msg[48];
    snprintf(msg, sizeof(msg), "photo alloc failed: need %u bytes", fb->len);
    writeLog(millis(), "ERR", msg);
    *outLen = 0;
  }

  esp_camera_fb_return(fb);
  return buf;
}

void folderInitialization() {
  while (SD.exists(folderPath)) {
    // / R e c o r d e d D a  t  a  #
    // 0 1 2 3 4 5 6 7 8 9 10 11 12 13
    int folderNumber = atoi(&folderPath[13]);
    folderNumber++;
    sprintf(&folderPath[13], "%d", folderNumber);
    yield();
  }
  SD.mkdir(folderPath);
}

// Appends a timestamped entry to the session log file.
// Call only from the storage task (Core 0) where SD access is safe.
// Format: timestamp_ms,LEVEL,message
void writeLog(uint32_t timestamp, const char *level, const char *msg) {
  char logPath[48];
  sprintf(logPath, "%s/log.csv", folderPath);
  File logFile = SD.open(logPath, FILE_APPEND);
  if (!logFile) return;
  char line[96];
  int len = snprintf(line, sizeof(line), "%lu,%s,%s\n", timestamp, level, msg);
  logFile.write((uint8_t *)line, len);
  logFile.close();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Storage task (Core 0) — owns all SD and camera operations

// Per-chunk state (held in PSRAM between chunk start and resolution)
struct ChunkMeasurement { uint32_t ts; float dist; };
#define MAX_CHUNK_MSMTS 1000

static uint8_t         *chunkPhotoBuf       = nullptr;  // chunk-start photo
static size_t           chunkPhotoLen       = 0;
static uint32_t         chunkPhotoTs        = 0;
static uint8_t         *potentialPhotoBuf   = nullptr;  // closest-approach photo
static size_t           potentialPhotoLen   = 0;
static uint32_t         potentialPhotoTs    = 0;
static bool             chunkPending        = false;
static ChunkMeasurement *chunkMsmts         = nullptr;
static int              chunkMsmtCount      = 0;
static uint32_t         lastPeriodicPhotoMs = 0;

// Write the pending photo to the main folder and release the buffer.
// Called when a chunk ends without being confirmed as an incident.
static void flushChunkToMainFolder() {
  if (!chunkPending) return;

  if (chunkPhotoBuf) {
    char path[120];
    sprintf(path, "%s/image-start-%lums.jpg", folderPath, chunkPhotoTs);
    if (!writeFile(SD, path, chunkPhotoBuf, chunkPhotoLen)) {
      writeLog(millis(), "ERR", "chunk start-photo flush failed");
    }
    free(chunkPhotoBuf);
    chunkPhotoBuf = nullptr;
    chunkPhotoLen = 0;
  }

  if (potentialPhotoBuf) {
    char path[120];
    sprintf(path, "%s/image-approach-%lums.jpg", folderPath, potentialPhotoTs);
    if (!writeFile(SD, path, potentialPhotoBuf, potentialPhotoLen)) {
      writeLog(millis(), "ERR", "chunk approach-photo flush failed");
    }
    free(potentialPhotoBuf);
    potentialPhotoBuf = nullptr;
    potentialPhotoLen = 0;
  }

  chunkPending   = false;
  chunkMsmtCount = 0;
}

void storageTask(void *pvParameters) {
  // Allocate the chunk measurement buffer (PSRAM preferred, DRAM fallback)
  if (psramFound()) {
    chunkMsmts = (ChunkMeasurement *)heap_caps_malloc(
        MAX_CHUNK_MSMTS * sizeof(ChunkMeasurement), MALLOC_CAP_SPIRAM);
  }
  if (!chunkMsmts) {
    chunkMsmts = (ChunkMeasurement *)malloc(
        MAX_CHUNK_MSMTS * sizeof(ChunkMeasurement));
  }

  char mainCsvPath[48];
  sprintf(mainCsvPath, "%s/data.csv", folderPath);

  // Keep the main CSV file open — eliminates open/close overhead on every write.
  // FatFS buffers internally; data is committed to SD on flush() or when the
  // internal sector buffer fills (~512 bytes ≈ every 25 measurements).
  File mainCsvFile = SD.open(mainCsvPath, FILE_APPEND);
  if (!mainCsvFile) {
    Serial.println("Failed to open main CSV!");
    errorFlash();
    // Can't log to SD if the file system is already failing — serial only
  }

  uint32_t lastFlushMs = 0;

  StorageCmd cmd;
  while (true) {
    // Process one command at a time and yield between each so IDLE0 can run
    // and reset the Task Watchdog. Without this, a burst of commands (or a
    // single slow incident save) can hold Core 0 long enough to trigger WDT.
    while (xQueueReceive(storageQueue, &cmd, 0) == pdTRUE) {
      switch (cmd.type) {

        case CMD_MEASUREMENT: {
          // Write directly to the open file — fast (buffered by FatFS)
          char line[64];
          int len = snprintf(line, sizeof(line), "%lu,%.2f\n",
                             cmd.measurement.timestamp,
                             cmd.measurement.distanceInch);
          mainCsvFile.write((uint8_t *)line, len);

          // Also buffer for a potential incident CSV
          if (chunkPending && chunkMsmts && chunkMsmtCount < MAX_CHUNK_MSMTS) {
            chunkMsmts[chunkMsmtCount++] = { cmd.measurement.timestamp,
                                             cmd.measurement.distanceInch };
          }

          // Periodic photo: once per second while a chunk is active
          if (chunkPending &&
              (cmd.measurement.timestamp - lastPeriodicPhotoMs) >= 1000) {
            size_t periodicLen;
            uint8_t *periodicBuf = photo_capture(&periodicLen);
            if (periodicBuf) {
              char periodicPath[120];
              sprintf(periodicPath, "%s/image-%lums.jpg", folderPath,
                      cmd.measurement.timestamp);
              if (!writeFile(SD, periodicPath, periodicBuf, periodicLen)) {
                writeLog(millis(), "ERR", "periodic photo write failed");
              }
              free(periodicBuf);
            } else {
              writeLog(millis(), "ERR", "periodic photo capture failed");
            }
            lastPeriodicPhotoMs = cmd.measurement.timestamp;
          }
          break;
        }

        case CMD_CHUNK_START: {
          // Previous chunk was not confirmed as an incident — save it to main folder
          flushChunkToMainFolder();

          // Capture the chunk-start photo
          chunkPhotoBuf       = photo_capture(&chunkPhotoLen);
          chunkPhotoTs        = cmd.chunkStart.timestamp;
          chunkPending        = (chunkPhotoBuf != nullptr);
          chunkMsmtCount      = 0;
          lastPeriodicPhotoMs = cmd.chunkStart.timestamp;
          if (!chunkPhotoBuf) {
            writeLog(cmd.chunkStart.timestamp, "ERR", "chunk-start photo capture failed");
          }

          // Clear any leftover potential photo from a previous chunk
          if (potentialPhotoBuf) {
            free(potentialPhotoBuf);
            potentialPhotoBuf = nullptr;
            potentialPhotoLen = 0;
          }
          break;
        }

        case CMD_POTENTIAL_EVENT: {
          // Capture the closest-approach photo, replacing any earlier one
          if (potentialPhotoBuf) {
            free(potentialPhotoBuf);
          }
          potentialPhotoTs  = millis();
          potentialPhotoBuf = photo_capture(&potentialPhotoLen);
          if (!potentialPhotoBuf) {
            writeLog(potentialPhotoTs, "ERR", "approach photo capture failed");
          }
          break;
        }

        case CMD_INCIDENT_CONFIRMED: {
          if (!chunkPending) break;

          // Flush main CSV before the slow incident writes so no data is lost
          mainCsvFile.flush();
          lastFlushMs = millis();

          // Create a new incident subfolder
          incidentCount++;
          char incidentPath[100];
          sprintf(incidentPath, "%s/incident%d", folderPath, incidentCount);
          SD.mkdir(incidentPath);

          // Each writeFile call can take hundreds of ms on a slow SD card.
          // Yield between them so IDLE0 can run and reset the Task Watchdog.
          vTaskDelay(pdMS_TO_TICKS(1));

          // Save the chunk-start photo (vehicle first appeared)
          if (chunkPhotoBuf) {
            char photoPath[120];
            sprintf(photoPath, "%s/image-start-%lums.jpg", incidentPath, chunkPhotoTs);
            if (!writeFile(SD, photoPath, chunkPhotoBuf, chunkPhotoLen)) {
              writeLog(millis(), "ERR", "incident start-photo write failed");
            }
            free(chunkPhotoBuf);
            chunkPhotoBuf = nullptr;
            chunkPhotoLen = 0;
          }

          vTaskDelay(pdMS_TO_TICKS(1));

          // Save the closest-approach photo (vehicle fully in frame)
          if (potentialPhotoBuf) {
            char photoPath[120];
            sprintf(photoPath, "%s/image-approach-%lums.jpg", incidentPath, potentialPhotoTs);
            if (!writeFile(SD, photoPath, potentialPhotoBuf, potentialPhotoLen)) {
              writeLog(millis(), "ERR", "incident approach-photo write failed");
            }
            free(potentialPhotoBuf);
            potentialPhotoBuf = nullptr;
            potentialPhotoLen = 0;
          }

          vTaskDelay(pdMS_TO_TICKS(1));

          // Save chunk measurements to a per-incident CSV (one open, many writes, one close)
          if (chunkMsmts && chunkMsmtCount > 0) {
            char incidentCsvPath[120];
            sprintf(incidentCsvPath, "%s/data.csv", incidentPath);
            File incidentCsvFile = SD.open(incidentCsvPath, FILE_WRITE);
            if (incidentCsvFile) {
              for (int i = 0; i < chunkMsmtCount; i++) {
                char line[64];
                int len = snprintf(line, sizeof(line), "%lu,%.2f\n",
                                   chunkMsmts[i].ts, chunkMsmts[i].dist);
                incidentCsvFile.write((uint8_t *)line, len);
              }
              incidentCsvFile.close();
            } else {
              errorFlash();
              writeLog(millis(), "ERR", "incident CSV open failed");
            }
          }

          chunkPending   = false;
          chunkMsmtCount = 0;
          Serial.printf("Incident %d saved: speed=%.1f mph, length=%.1f in\r\n",
                        incidentCount,
                        cmd.incident.speed_mph,
                        cmd.incident.length_inches);
          break;
        }

        case CMD_CHUNK_END: {
          // Chunk ended without a confirmed incident — flush any buffered
          // photos to the main folder and reset pending state.
          // (If CMD_INCIDENT_CONFIRMED already ran, chunkPending is false
          // and flushChunkToMainFolder returns immediately.)
          flushChunkToMainFolder();
          break;
        }

        case CMD_LOG_EVENT: {
          writeLog(cmd.logEvent.timestamp, "WARN", cmd.logEvent.msg);
          break;
        }
      }
    }

    // Flush the main CSV to SD once per second so data survives a crash
    uint32_t now = millis();
    if (now - lastFlushMs >= 1000) {
      mainCsvFile.flush();
      lastFlushMs = now;
    }

    // Yield for 1 tick so IDLE0 can run and reset the task watchdog
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Detector callbacks (called from detector_update() on Core 1 — keep fast)

void on_chunk_start(uint32_t timestamp_ms) {
  StorageCmd cmd;
  cmd.type = CMD_CHUNK_START;
  cmd.chunkStart.timestamp = timestamp_ms;
  xQueueSend(storageQueue, &cmd, 0);
}

void on_potential_event() {
  StorageCmd cmd;
  cmd.type = CMD_POTENTIAL_EVENT;
  xQueueSend(storageQueue, &cmd, 0);
}

void on_chunk_end() {
  StorageCmd cmd;
  cmd.type = CMD_CHUNK_END;
  xQueueSend(storageQueue, &cmd, 0);
}

void on_detection(const detector_event_t *event) {
  redLedActive  = true;
  redLedStartMs = millis();

  StorageCmd cmd;
  cmd.type = CMD_INCIDENT_CONFIRMED;
  cmd.incident = *event;
  xQueueSend(storageQueue, &cmd, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Blinks all three LEDs indefinitely — called on unrecoverable errors
void errorBlink() {
  while (true) {
    digitalWrite(redLEDPin,    HIGH);
    digitalWrite(yellowLEDPin, HIGH);
    digitalWrite(greenLEDPin,  HIGH);
    delay(300);
    digitalWrite(redLEDPin,    LOW);
    digitalWrite(yellowLEDPin, LOW);
    digitalWrite(greenLEDPin,  LOW);
    delay(300);
  }
}

void setup() {
  Serial.begin(115200); // Starts the serial communication

  // Increase the Task Watchdog timeout from the default 5s to 30s.
  // Incident saves write multiple large JPEGs to SD sequentially; even with
  // yields between them, a slow card can take several seconds per file.
  {
    esp_task_wdt_config_t wdt_cfg = {
      .timeout_ms     = 30000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
  }

  // initialize ultrasonic sensor
  pinMode(trigPin, OUTPUT); // Sets the trigPin as an Output
  pinMode(echoPin, INPUT); // Sets the echoPin as an Input

  pinMode(redLEDPin, OUTPUT);
  pinMode(yellowLEDPin, OUTPUT);
  pinMode(greenLEDPin, OUTPUT);

  pinMode(buttonPin, INPUT_PULLDOWN);

  digitalWrite(redLEDPin, HIGH);
  digitalWrite(yellowLEDPin, HIGH);
  digitalWrite(greenLEDPin, HIGH);

  // initialize camera
  CameraParameters(); // this function does the bulk of the camera setup
  camera_status = true; // camera status set to true
  Serial.println("Camera OK!"); // show through serial monitor that camera is working

  // initialize SD card
  // Must explicitly set SPI pins — default SPI pins (MOSI=11, SCK=12, MISO=13)
  // overlap with camera DVP pins and are already claimed by CameraParameters().
  // XIAO ESP32S3 SD card SPI pins: SCK=7, MISO=8, MOSI=9, CS=21
  SPI.begin(7, 8, 9, 21);
  // After a hard reset (watchdog or panic) the SD card needs longer to
  // recover than after a clean power-on. Give it extra time in that case.
  {
    esp_reset_reason_t reason = esp_reset_reason();
    delay((reason == ESP_RST_TASK_WDT || reason == ESP_RST_PANIC) ? 2000 : 200);
  }

  // Retry SD mount up to 5 times — the card may still be settling.
  // If all retries fail, reboot automatically rather than hanging forever.
  {
    bool mounted = false;
    for (int attempt = 0; attempt < 5 && !mounted; attempt++) {
      if (attempt > 0) delay(500);
      mounted = SD.begin(21);
    }
    if (!mounted) {
      Serial.println("Failed to mount SD after retries — rebooting in 3s");
      delay(3000);
      ESP.restart();
    }
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No MicroSD card inserted — rebooting in 3s");
    delay(3000);
    ESP.restart();
  }

  // print the type of MicroSD card being used
  // not technically needed for what we are doing but it is nice to see it is properly reading cardType for debugging SD card purposes
  Serial.print("MicroSD Card Type: ");
  if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD) Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");

  sd_status = true; // if at this point in setup, SD card is working!

  // setup initial folder
  Serial.println("Before folder initialization");
  folderInitialization(); // initialization for the folder containing all incidents and data "RecordedData#" This should happen everytime the device is powered on.
  Serial.println("After folder initialization");
  storageQueue = xQueueCreate(512, sizeof(StorageCmd));

  detector_set_chunk_start_callback(on_chunk_start);
  detector_set_potential_callback(on_potential_event);
  detector_set_callback(on_detection);
  detector_set_chunk_end_callback(on_chunk_end);

  xTaskCreatePinnedToCore(storageTask, "StorageTask", 8192, NULL, 1, NULL, 0);

  // Signal ready — green on, others off
  digitalWrite(redLEDPin,    LOW);
  digitalWrite(yellowLEDPin, LOW);
  digitalWrite(greenLEDPin,  HIGH);

  init_ble();
  serial_print("[Main Debug] After init_ble");
}

void loop() {
  // Clears the trigPin
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  // Sets the trigPin on HIGH state for 10 micro seconds
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Reads the echoPin, returns the sound wave travel time in microseconds
  duration = pulseIn(echoPin, HIGH, 500000); // RH: pulseIn has a timeout, which can be optimized / reduced; e.g.

  // Calculate the distance in inches
  distanceInch = CM_TO_INCH * duration * SOUND_SPEED/2;

  // Prints the distance in the Serial Monitor
  //Serial.println(distanceInch); //todo uncomment later

  // Feed detector (may trigger on_chunk_start or on_detection callbacks)
  detector_update(millis(), distanceInch);

  // RH: small delay needed for sensor stability
  delay(5);

  // Queue measurement for storage — non-blocking
  StorageCmd cmd;
  cmd.type = CMD_MEASUREMENT;
  cmd.measurement.distanceInch = distanceInch;
  cmd.measurement.timestamp    = millis();
  if (xQueueSend(storageQueue, &cmd, 0) != pdTRUE) {
    Serial.println("Storage queue full, measurement dropped!");
    StorageCmd logCmd;
    logCmd.type = CMD_LOG_EVENT;
    logCmd.logEvent.timestamp = millis();
    snprintf(logCmd.logEvent.msg, sizeof(logCmd.logEvent.msg), "queue full, msmt dropped");
    xQueueSend(storageQueue, &logCmd, 0);
  }

  // Update status LEDs
  if (redLedActive && (millis() - redLedStartMs >= RED_LED_DURATION_MS)) {
    redLedActive = false;
  }

  if (redLedActive) {
    digitalWrite(redLEDPin,    HIGH);
    digitalWrite(yellowLEDPin, LOW);
    digitalWrite(greenLEDPin,  LOW);
  } else if (distanceInch <= 150.0f) {
    digitalWrite(redLEDPin,    LOW);
    digitalWrite(yellowLEDPin, HIGH);
    digitalWrite(greenLEDPin,  LOW);
  } else {
    digitalWrite(redLEDPin,    LOW);
    digitalWrite(yellowLEDPin, LOW);
    digitalWrite(greenLEDPin,  HIGH);
  }
  if (digitalRead(buttonPin) == HIGH) {
    if (millis() - lastButtonPressMs >= debounceDelayMs) {
        lastButtonPressMs = millis();
        
        if (deviceConnected == 1) {
            Serial.println("Button pressed: Sending Protobuf Event Notification...");
            
            // Send a test EventNotification protobuf packet over BLE
            notify_phone_of_event(
                11,                           // event_id
                static_cast<uint32_t>(distanceInch * 2.54f), // distance_cm
                millis(),                                // time_offset_ms
                1,                                       // image_count
                IDBC_ImageFormat_IMAGE_FORMAT_JPEG       // image_format
            ); // */
            // /*
            // Test image transfer over proto
            runTransferTest((uint32_t)11, (uint8_t)0);
            // */
        } else {
            Serial.println("Button pressed, but BLE is not connected!");
        }
    }
  }
  handle_pending_bluetooth_tasks();
}


// TODO this is only as a helper so we should delete it
#ifdef __cplusplus
extern "C" {
#endif

void serial_print(const char* str) {
    if (Serial) {
        Serial.println(str);
    }
}

bool bleInUse(void) { return true; }
bool btInUse(void) { return true; }

#ifdef __cplusplus
}
#endif


