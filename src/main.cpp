#include <Arduino.h>

// Declare your Arduino setup and loop as they are defined in IDBC.ino
extern void setup();
extern void loop();

extern "C" void app_main(void) {
    // Initialize Arduino core libraries and run standard tasks
    initArduino();
    
    setup();
    for (;;) {
        loop();
        vTaskDelay(1); // Allow background RTOS task switching
    }
}