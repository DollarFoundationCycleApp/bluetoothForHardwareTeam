# bluetoothForHardwareTeam

## Proto testing

Adding the documentation here so others can use it when they have access. In the IDBC.ino file from the hardware team at the top add:

```Arduino

#include "IBDC_v0.3.3.pb.h"
// Forward declarations for BLE symbols in Bluetooth.ino
extern bool deviceConnected;
void initBLE();
void notifyPhoneOfEvent(uint32_t eventId, uint32_t distanceCm, uint32_t timeOffsetMs, uint32_t imageCount, uint32_t format);
//Button
const int buttonPin = 4;
unsigned long lastButtonPressMs = 0;
const unsigned long debounceDelayMs = 200; // 200ms debounce
uint32_t testEventId = 100;
```

and at the end of the loop method add:

```Arduino
  if (digitalRead(buttonPin) == LOW) {
    if (millis() - lastButtonPressMs >= debounceDelayMs) {
        lastButtonPressMs = millis();
        
        if (deviceConnected) {
            Serial.println("Button pressed: Sending Protobuf Event Notification...");
            
            // Send a test EventNotification protobuf packet over BLE
            notifyPhoneOfEvent(
                testEventId++,                           // event_id
                static_cast<uint32_t>(distanceInch * 2.54f), // distance_cm
                millis(),                                // time_offset_ms
                1,                                       // image_count
                IDBC_ImageFormat_IMAGE_FORMAT_JPEG       // image_format
            );
        } else {
            Serial.println("Button pressed, but BLE is not connected!");
        }
    }
  }
```

## Image Chunker Demo

The Image Chunker demo demonstrates how an image is:
1. Read from a file as raw bytes.
2. Identified as JPEG or PNG.
3. Divided into 400-byte `ImageChunk` payloads.
4. Reassembled into the original byte sequence.
5. Verified to be byte-for-byte identical to the original image.

### Requirements
The demo is currently built and tested using GCC through MSYS2 UCRT64.

### Building
From the repository root, open an MSYS2 UCRT64 terminal and run:

```bash
gcc -Isrc/main/IDBC -Isrc/main/IDBC/Nanopb -c src/main/IDBC/ImageChunker.c -o ImageChunker.o
gcc -Isrc/main/IDBC -Isrc/main/IDBC/Nanopb -c src/main/IDBC/IBDC_v0.3.2.pb.c -o IBDC_v0.3.2.pb.o
gcc -Isrc/main/IDBC -Isrc/main/IDBC/Nanopb -c src/main/IDBC/Nanopb/pb_common.c -o pb_common.o
gcc -Isrc/main/IDBC -Isrc/main/IDBC/Nanopb -c src/main/IDBC/Nanopb/pb_encode.c -o pb_encode.o
gcc -Isrc/main/IDBC -Isrc/main/IDBC/Nanopb -c src/main/IDBC/Nanopb/pb_decode.c -o pb_decode.o
gcc -Isrc/main/IDBC -Isrc/main/IDBC/Nanopb -c src/main/IDBC/ImageChunkerDemo.c -o ImageChunkerDemo.o
gcc -Isrc/main/IDBC -Isrc/main/IDBC/Nanopb ImageChunkerDemo.o ImageChunker.o IBDC_v0.3.2.pb.o pb_common.o pb_encode.o pb_decode.o -o ImageChunkerDemo.exe
```

### Running
```bash
./ImageChunkerDemo.exe <input-image> <output-image>
```

---------------------------------------------------------------------------------------------------------------------------- 

## Libraries necessary for development and loading

### Arduino

 * NanoPb Files are included and can be moved into the Ardunio library folder.
   * Secondary option is to change the header to be the files in the NanoPb but then you'll have to transport that over as well.
 * NimBLE Files are zipped but can otherwise be moved into the Arduino library folder.

### Setup

Take the IDBC zip file from the discord and move into an unzipped location. From there copy and paste the Bluetooth.ino, IBDC_V0.3.2 Options, pb.c, pb.h, and .proto files into the unzipped IDBC folder. Then open the ardunio IDE. 

Configure the IDE to allow the ESP32 s3 Xiao to be seen on the com port. At the very bottom of the Setup() function in IDBC.ino place `initBLE();` so that bluetooth gets initialized.

From there go ahead and click upload. Then wait it should take approximately 10 minutes for longer compilation times. If this is the first time compiling the code it can take up to 45 minutes (thank you Arduino).

### Notes

Bluetooth should be accessible from the standard bluetooth module in a phone but is not compatible with the computer.
