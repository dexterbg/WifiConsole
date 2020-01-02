/***************************************************************************************************
 * OVMS WifiConsole Configuration
 */

// OVMS Module Connection:
const char* wifiSSID            = "SSID";           // OVMS wifi network ID (normally AP network)
const char* wifiPassword        = "PASSWORD";       // OVMS wifi password
const char* ovmsHost            = "192.168.4.1";    // OVMS module IP address on Wifi network
const char* ovmsAPIKey          = "PASSWORD";       // OVMS command apikey, normally = admin password

// OVMS Tuning Defaults:
const int defaultRecupLevel     = 18;               // set to 21 for Twizy45

// User Interface:
const int sleepTimeout          = 30;               // enter sleep mode after n seconds
const int dimTimeout            = 5;                // dim display after n seconds
const int applyTimeout          = 8;                // apply config change after n 1/10 seconds
const int socTimeout            = 3;                // show SOC on boot for n seconds (button: push/hold)

const int maxButtons            = 10;               // max profile buttons allowed

// Battery Monitoring:
const float vbatCalibration     = 0.004266827f;     // set this to real vbat / A0, see serial output for A0
const float vbatAlert           = 3.55f;            // display BATT LOW info below this voltage level
const float vbatLimit           = 3.45f;            // inhibit operation below this voltage level

// Rotary Encoder:
const int encoderPinDT          = 12;               // GPIO12
const int encoderPinCLK         = 13;               // GPIO13

// Display:
const int displayPinReset       = -1;               // set if display RST is on separate IO pin
