# bluetoothForHardwareTeam

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
