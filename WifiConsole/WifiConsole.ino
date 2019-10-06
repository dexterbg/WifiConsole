/***************************************************************************************************
 * OVMS WifiConsole: wireless tuning console for OVMS v3 / Renault Twizy
 * 
 * Supports loading profiles and changing the neutral/brake recuperation levels
 * as well as the drive power level on the fly. Push the rotary encoder knob to
 * change the page, turn the rotary encoder to change the value.
 * 
 * Requirements:
 *  - OVMS v3: https://github.com/openvehicles/Open-Vehicle-Monitoring-System-3
 *  - OVMS script plugin WifiConsole.js needs to be installed (see extras folder)
 * 
 * Hardware used:
 *  - WEMOS/LOLIN D1 mini Pro v2 (ESP8266 + battery shield)
 *  - WEMOS/LOLIN D1 OLED shield 64x48
 *  - Rotary Encoder KY-040
 *  - LiPo battery, e.g. 400 mAh
 * 
 * Libraries used:
 *  - IoAbstraction @davetcc: https://github.com/davetcc/IoAbstraction
 *  - Adafruit_SSD1306 @mcauser: https://github.com/mcauser/Adafruit_SSD1306/tree/esp8266-64x48
 * 
 * History:
 *  - V1.0  2019-10-05  Michael Balzer <dexter@dexters-web.de>
 * 
 * License:
 *  This is free software under GNU Lesser General Public License (LGPL)
 *  https://www.gnu.org/licenses/lgpl.html
 */
#define APPLICATION "OVMS WifiConsole V1.0 (2019-10-05)"

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <string.h>
#include <IoAbstraction.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "WifiConsole_config.h"


uint32_t calculateCRC32(const uint8_t *data, size_t length);
String execCommand(String cmd);

int sleepTicker;
int dimTicker;
int applyTicker;


class MyDisplay : public Adafruit_SSD1306
{
  using Adafruit_SSD1306::Adafruit_SSD1306;

public:
  /**
   * printAt: positioned text output
   *  - x,y     pixel position
   *  - tsize   text scaling (1…3), font is 6x8 pixel
   *  - text    …guess…
   */
  void printAt(uint16_t x, uint16_t y, uint8_t tsize, String text) {
    setCursor(x, y);
    setTextSize(tsize);
    print(text);
  }

  /**
   * printLine: centered line output
   *  - y       vertical pixel position
   *  - tsize   text scaling (1…3), font is 6x8 pixel
   *  - text    …guess…
   *  - erase   true = erase full line area before output
   */
  void printLine(uint16_t y, uint8_t tsize, String text, bool erase=false) {
    uint16_t x = (width() - text.length() * 6 * tsize) / 2;
    if (x < 0) x = 0;
    if (erase) fillRect(0, y, width(), 8*tsize, BLACK);
    setCursor(x, y);
    setTextSize(tsize);
    print(text);
  }
  
} display(displayPinReset); // I2C mode


/**
 * OVMS SEVCON macro tuning configuration profile:
 *  (note: values shifted by +1)
 */
struct __attribute__ ((__packed__)) cfg_profile
{
  uint8_t       checksum;
  uint8_t       speed, warn;
  uint8_t       torque, power_low, power_high;
  uint8_t       drive, neutral, brake;
  struct tsmap {
    uint8_t       spd1, spd2, spd3, spd4;
    uint8_t       prc1, prc2, prc3, prc4;
  }             tsmap[3]; // 0=D 1=N 2=B
  uint8_t       ramp_start, ramp_accel, ramp_decel, ramp_neutral, ramp_brake;
  uint8_t       smooth;
  uint8_t       brakelight_on, brakelight_off;
  uint8_t       ramplimit_accel, ramplimit_decel;
  uint8_t       autorecup_minprc;
  uint8_t       autorecup_ref;
  uint8_t       autodrive_minprc;
  uint8_t       autodrive_ref;
  uint8_t       current;
};


/**
 * Application state & logic
 */

typedef struct __attribute__ ((__packed__)) {
  uint8_t       key;
  char          label[4];
} button_t;

struct __attribute__ ((__packed__)) Application {

  uint32_t      crc;                    // RTC consistency checksum

  int8_t        awake;                  // 0=sleeping/poweron, 1=awake
  int8_t        page;                   // selected page (0…3)

  uint8_t       button_cnt;             // drive mode button count
  uint8_t       button_sel;             // …selected index
  button_t      button[maxButtons];     // …keys & labels

  cfg_profile   profile;                // current tuning parameters


  /**
   * RTC RAM management:
   */

  void init() {
    *this = {};
    button_cnt = 1;
    button[0].key = 0;
    strcpy(button[0].label, "STD");
  }

  uint32_t crc32() {
    return calculateCRC32((const uint8_t*)this+sizeof(crc), sizeof(*this)-sizeof(crc));
  }

  void save() {
    crc = crc32();
    if (!ESP.rtcUserMemoryWrite(0, (uint32_t*) this, sizeof(*this))) {
      Serial.println("ERROR: state.save() failed");
    }
  }

  void load() {
    if (!ESP.rtcUserMemoryRead(0, (uint32_t*) this, sizeof(*this)) || crc != crc32()) {
      Serial.println("INFO: state.load() failed => init");
      init();
    }
  }

  /**
   * readConfig: parse config info string retreived by wificon.info() or wificon.load()
   *  - returns true if config was updated
   */
  bool readConfig(String info) {
    bool upd_buttons = false, upd_profile = false;
    int p1 = 0, p2, p3;
    String line;
    
    while ((p2 = info.indexOf('\n', p1)) >= 0) {
      // isolate line:
      line = info.substring(p1, p2);
      p1 = p2+1;
      
      if (line[1] != ':') {
        continue;
      }
      else if (line[0] == 'b' || line[0] == 'B') {
        // button definition: [bB]:<key>,<label>
        if (!upd_buttons) {
          upd_buttons = true;
          button_cnt = 0;
          button_sel = 0;
          memset(button, 0, sizeof(button));
        }
        if (button_cnt == maxButtons) {
          Serial.println("readConfig: max button cnt reached");
        }
        else {
          p3 = line.indexOf(',');
          if (p3 > 2) {
            button[button_cnt].key = line.substring(2,p3).toInt();
            line.substring(p3+1).toCharArray(button[button_cnt].label, sizeof(button[button_cnt].label));
            if (line[0] == 'B')
              button_sel = button_cnt; // selected button
            button_cnt++;
          }
        }
      }
      else if (line[0] == 'P') {
        // parse profile: P:<d1>,<d2>,...
        if (!upd_profile) {
          upd_profile = true;
          profile = {};
        }
        uint8_t* data = (uint8_t*)&profile;
        int i = 0, q1 = 2, q2;
        while (i < sizeof(profile) && (q2 = line.indexOf(',', q1)) >= 0) {
          data[i] = line.substring(q1,q2).toInt() + 1;
          q1 = q2+1;
          i++;
        }
      }
    }

    if (upd_buttons || upd_profile)
      save();
    
    Serial.printf("readConfig: %d buttons, sel=%d, recup %d %d, drive %d\n",
                  button_cnt, button_sel,
                  (int)profile.neutral-1, (int)profile.brake-1, (int)profile.drive-1);
    
    return (upd_buttons || upd_profile);
  }

  /**
   * getConfig: query configured buttons & active tuning from OVMS
   */
  bool getConfig() {
    String res = execCommand("script eval wificon.info()");
    if (res != "")
      return readConfig(res);
    else
      return false;
  }

  /**
   * nextPage: cycle through pages
   */
  void nextPage() {
    page = (page + 1) % 4;
    save();
  }
  
  /**
   * renderValue: display current page value
   */
  void renderValue(bool erase=false) {
    int level;
    switch (page) {
      case 0:
        // Page 0: profile selector
        display.printLine(32, 2, String(button[button_sel].label), erase);
        char nav[9];
        sprintf(nav, "%-3.3s%c%c%3.3s",
                (button_sel > 0) ? button[button_sel-1].label : "",
                (button_sel > 0) ? '<' : ' ',
                (button_sel < button_cnt - 1) ? '>' : ' ',
                (button_sel < button_cnt - 1) ? button[button_sel+1].label : "");
        display.printAt(0, 56, 1, nav);
        break;
      case 1:
        // Page 1: neutral recup level
        level = (int)profile.neutral - 1;
        if (level == -1) level = defaultRecupLevel;
        display.printLine(32, 3, String(level), erase);
        break;
      case 2:
        // Page 2: brake recup level
        level = (int)profile.brake - 1;
        if (level == -1) level = defaultRecupLevel;
        display.printLine(32, 3, String(level), erase);
        break;
      case 3:
        // Page 3: drive level
        level = (int)profile.drive - 1;
        if (level == -1) level = 100;
        display.printLine(32, 3, String(level), erase);
        break;
    }
  }
  
  /**
   * renderPage: display selected page, configure encoder
   */
  void renderPage() {
    int max, level;
    
    Serial.printf("\n*** renderPage: %d\n", page);
    display.clearDisplay();
    
    switch (page) {
      case 0:
        // Page 0: profile selector
        max = button_cnt - 1;
        level = button_sel;
        display.printAt(0, 0, 2, "P");
        break;
      case 1:
        // Page 1: neutral recup level
        max = 100;
        level = (int)profile.neutral - 1;
        if (level == -1) level = defaultRecupLevel;
        display.printAt(12, 0, 2, "N");
        break;
      case 2:
        // Page 2: brake recup level
        max = 100;
        level = (int)profile.brake - 1;
        if (level == -1) level = defaultRecupLevel;
        display.printAt(24, 0, 2, "B");
        break;
      case 3:
        // Page 3: drive level
        max = 100;
        level = (int)profile.drive - 1;
        if (level == -1) level = 100;
        display.printAt(36, 0, 2, "D");
        break;
    }
    
    display.drawFastHLine(0, 17, 48, WHITE);
    renderValue();
    display.display();
    
    Serial.printf("- configure encoder: max=%d level=%d\n", max, level);
    switches.changeEncoderPrecision(max, level);
  }
  
  /**
   * updateValue: dispatch level change to page, start apply ticker
   */
  void updateValue(int level) {
    bool changed = false;
    
    switch (page) {
      case 0:
        // Page 0: profile selector
        if (button_sel != level) {
          button_sel = level;
          changed = true;
        }
        break;
      case 1:
        // Page 1: neutral recup level
        if (profile.neutral != level + 1) {
          profile.neutral = level + 1;
          changed = true;
        }
        break;
      case 2:
        // Page 2: brake recup level
        if (profile.brake != level + 1) {
          profile.brake = level + 1;
          changed = true;
        }
        break;
      case 3:
        // Page 3: drive level
        if (profile.drive != level + 1) {
          profile.drive = level + 1;
          changed = true;
        }
        break;
    }

    if (changed) {
      renderValue(true);
      display.display();
      save();
      applyTicker = applyTimeout;
    }
  }

  /**
   * Apply current value: load / change tuning profile
   */
  void apply() {
    String cmd, res;
    switch (page) {
      case 0:
        // Page 0: profile selector
        cmd = "script eval wificon.load("; cmd += button[button_sel].key; cmd += ")";
        res = execCommand(cmd);
        if (res != "")
          readConfig(res);
        break;
      case 1:
      case 2:
        // Page 1: neutral recup level
        // Page 2: brake recup level
        cmd = "xrt cfg recup "; cmd += (int)profile.neutral-1;
        cmd += " "; cmd += (int)profile.brake-1;
        cmd += " "; cmd += (int)profile.autorecup_ref-1;
        cmd += " "; cmd += (int)profile.autorecup_minprc-1;
        res = execCommand(cmd);
        break;
      case 3:
        // Page 3: drive level
        cmd = "xrt cfg drive "; cmd += (int)profile.drive-1;
        cmd += " "; cmd += (int)profile.autodrive_ref-1;
        cmd += " "; cmd += (int)profile.autodrive_minprc-1;
        res = execCommand(cmd);
        break;
    }
  }
  
} state;


/**
 * enterSleep: switch off OLED, enter ESP8266 deep sleep
 */
void enterSleep() {
  Serial.println("\n*** enterSleep");
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  ESP.deepSleep(0);
}

/**
 * ticker (100 ms callback): check timeouts
 */
void ticker() {
  if (sleepTicker > 0 && --sleepTicker == 0) {
    state.awake = 0;
    state.save();
    enterSleep();
  }
  if (dimTicker > 0 && --dimTicker == 0) {
    display.dim(true);
  }
  if (applyTicker > 0 && --applyTicker == 0) {
    state.apply();
  }
}

/**
 * onEncoderChange callback: rotary encoder value changed
 */
void onEncoderChange(int newValue) {
  sleepTicker = sleepTimeout * 10;
  if (dimTicker == 0)
    display.dim(false);
  dimTicker = dimTimeout * 10;
  state.updateValue(newValue);
}

/**
 * urlEncode: Mozilla standard URL character encoding
 */
String urlEncode(const String& text) {
	const char* hexcode = "0123456789ABCDEF";
	String buf;
	char c;
	for (int i = 0; i < text.length(); i++) {
    c = text.charAt(i);
    if (isalnum(c) || c=='-' || c=='_' || c=='.') {
			buf += c;
		} else if (c == ' ') {
			buf += '+';
		} else {
			buf += '%';
			buf += hexcode[c >> 4];
			buf += hexcode[c & 0x0f];
		}
	}
	return buf;
}

/**
 * execCommand: execute OVMS command, return command output
 */
String execCommand(String cmd) {
  Serial.printf("\n*** execCommand: %s\n", cmd.c_str());

  String url = "http://"; url += ovmsHost;
  url += "/api/execute?apikey="; url += urlEncode(ovmsAPIKey);
  url += "&command="; url += urlEncode(cmd);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  
  String res;
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    res = http.getString();
    Serial.println("OK, response:");
    Serial.println(res);
  } else {
    Serial.print("ERROR: httpCode=");
    Serial.println(httpCode);
  }

  return res;
}

/**
 * calculateCRC32: checksum generator
 */
uint32_t calculateCRC32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffff;
  while (length--) {
    uint8_t c = *data++;
    for (uint32_t i = 0x80; i > 0; i >>= 1) {
      bool bit = crc & 0x80000000;
      if (c & i) {
        bit = !bit;
      }
      crc <<= 1;
      if (bit) {
        crc ^= 0x04c11db7;
      }
    }
  }
  return crc;
}


/***************************************************************************************************
 * SETUP:
 *  - the rotary encoder switch triggers a reset
 *  - state.awake tells if we start from interactive or sleep state
 */
void setup() {

  Serial.begin(115200);
  Serial.print("\n\n[" APPLICATION "]\n\n");

  // Init state:
  state.load();
  if (state.awake) {
    Serial.println("*** SETUP: nextPage");
    state.nextPage();
  } else {
    Serial.println("*** SETUP: wakeup");
  }

  // Init display:
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3C (for the 64x48)
  display.setRotation(3);
  display.setTextColor(WHITE, BLACK);
  display.setTextWrap(false);
  display.clearDisplay();

  // Battery voltage & calibration check:
  int a0 = analogRead(A0);
  float vbat = a0 * vbatCalibration;
  Serial.printf("\nBattery voltage: A0=%d x %.9f = %.2fV\n", a0, vbatCalibration, vbat);

  // Display voltage + alert as necessary:
  if (!state.awake || vbat < vbatAlert) {
    display.printLine(4, 1, String(vbat,2) + "V");
  }
  if (vbat < vbatAlert) {
    Serial.println("\n*** BATTERY LOW!");
    display.printLine(32, 2, "BATT");
    display.printLine(48, 2, "LOW!");
    display.display();
    delay(3000);
  }
  if (vbat < vbatLimit) {
    Serial.println("\n*** BATTERY CRITICAL!");
    enterSleep();
  }
  else if (state.awake) {
    display.clearDisplay();
  }

  // Init Rotary Encoder:
  switches.initialise(ioUsingArduino(), true);
  setupRotaryEncoderWithInterrupt(encoderPinDT, encoderPinCLK, onEncoderChange);

  // Connect to OVMS WiFi network:
  Serial.printf("\nConnecting to WiFi SSID '%s' ...\n", wifiSSID);
  if (!state.awake) {
    display.printLine(12, 1, "Connect");
  }
  display.display();
  uint32_t time_wifistart = millis();

  // Save boot time by skipping WiFi init if unchanged:
  if (WiFi.SSID() != wifiSSID) {
    Serial.println("Initialising WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID, wifiPassword);
    WiFi.persistent(true);
    WiFi.setAutoConnect(true);
    WiFi.setAutoReconnect(true);
  }

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("\n*** WiFi ERROR: connection failed");
    display.printLine(32, 2, "WiFi");
    display.printLine(48, 2, "ERR");
    display.display();
    delay(5000);
    enterSleep();
  }

  uint32_t time_connect = millis();
  Serial.printf("Connect time: %u ms\n", time_connect - time_wifistart);
  Serial.print("Our IP address: ");
  Serial.println(WiFi.localIP());

  // Get OVMS config update on wakeup:
  if (!state.awake) {
    Serial.println("\nGet OVMS config update...");
    display.printLine(20, 1, "Update");
    display.display();
    
    state.awake = 1;
    if (!state.getConfig()) {
      // getConfig didn't get anything, so we need to save here:
      state.save();
    }
    
    uint32_t time_update = millis();
    Serial.printf("\nUpdate time: %u ms\n", time_update - time_connect);
  }

  // Init UI:
  state.renderPage();

  // Init tickers:
  sleepTicker = sleepTimeout * 10;
  dimTicker = dimTimeout * 10;
  applyTicker = 0;
  taskManager.scheduleFixedRate(100, ticker);
}

/***************************************************************************************************
 * LOOP:
 */
void loop() {
  // IoAbstraction event loop:
  taskManager.runLoop();
}
