#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ESP32Ping.h>
#include <time.h>
#include <WiFiUdp.h>
#include "driver/rtc_io.h"
#include <SPI.h>
#include <RadioLib.h>
#include "boards/heltec_wifi_lora_32_V3/board_pinout.h"  // <-- Add this line
#include <U8g2lib.h>
#include <Wire.h>

#define LORA_BAND 433E6 // or 868E6 or 915E6 depending on your module

#define RELAY_ON  HIGH
#define RELAY_OFF LOW

const char* buildDate = __DATE__;
const char* buildTime = __TIME__;



// For SSD1306 128x64 I2C (DollaTek/Heltec V3, ESP32-S3: SCL=18, SDA=17)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 18, /* data=*/ 17);


struct Schedule {
  bool enabled;
  String powerOnTime;
  String powerOffTime;
  int pollIntervalMinutes;
};

Schedule globalSchedule;


WiFiUDP syslogUdp;
IPAddress syslogIP;


bool rebootPending = false;
unsigned long rebootStartTime = 0;

const int ledPin = 2;  // GPIO pin for onboard blue LED

// Original ESP32 pins
//const int relayPins[6] = {14, 27, 26, 25, 33, 32};
//ESP32S3 Pins
const int relayPins[6] = {19,20,26,48,47,33};


String relayLabels[6];
String relayIPs[6];

bool relayStates[6] = {false, false, false, false, false, false};
bool relayPingEnabled[6] = {false};
bool relayResetEnabled[6] = {false};
bool globalScheduleEnabled = false;
int globalPollIntervalMinutes = 10;

String globalPowerOnTime = "06:00";
String globalPowerOffTime = "23:00";

unsigned long lastScheduleCheck = 0;
unsigned long lastStatusFlashOn = 0;
unsigned long lastStatusFlashOff = 0;

File uploadFile;
size_t lastUploadSize = 0;

String wifiSSID = "";
String wifiPassword = "";
WebServer server(80);
unsigned long lastPingTime = 0;
String logText = "";


unsigned long measureElapsedMs() {
  static unsigned long lastCall = 0;
  unsigned long now = millis();
  unsigned long elapsed = 0;
  if (lastCall != 0) {
    elapsed = now - lastCall;
  }
  lastCall = now;
  return elapsed;
}


void debugPrint(const String& msg) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print(" ms] ");
  Serial.println(msg);
}


void debugPrintf(const char* format, ...) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print(" ms] ");

  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  Serial.println(buf);
}



String htmlEscape(const String& input) {
  String output = input;
  output.replace("&", "&amp;");
  output.replace("<", "&lt;");
  output.replace(">", "&gt;");
  output.replace("\"", "&quot;");
  output.replace("'", "&#39;");
  return output;
}



String loadHTMLPart(const char* path) {
  File file = SPIFFS.open(path, "r");
  if (!file || file.isDirectory()) {
    debugPrint("Failed to open HTML part file");
    return "";
  }

  String content;
  while (file.available()) {
    content += file.readString();
  }
  file.close();
  return content;
}


int timeToMinutesX(String timeStr) {
  // Assumes timeStr format is "HH:mm"
  int colonIndex = timeStr.indexOf(':');
  if (colonIndex == -1) return 0; // invalid format fallback

  int hour = timeStr.substring(0, colonIndex).toInt();
  int minute = timeStr.substring(colonIndex + 1).toInt();
  return hour * 60 + minute;
}


// Used in the logging routine
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "- No Network Time -";
  }
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

// Used in the sleep routines
String getCurrentTimeStr() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  char buf[6]; // "HH:mm" + null terminator
  snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
  return String(buf);
}




bool shouldBeOnBySchedule() {
  if (!globalSchedule.enabled) return true;  // Always ON if schedule is disabled

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    debugPrint("[SCHEDULE] No local time available.");
    return true;  // Default to ON if no time info
  }
  else
  {
   debugPrintf("[SCHEDULE] Local time is good: %02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min);
  }

  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  int onHour, onMin, offHour, offMin;
  if (sscanf(globalSchedule.powerOnTime.c_str(), "%d:%d", &onHour, &onMin) != 2 ||
      sscanf(globalSchedule.powerOffTime.c_str(), "%d:%d", &offHour, &offMin) != 2) {
    debugPrint("[SCHEDULE] Invalid time format.");
    return true;  // Default to ON if time format is invalid
  }

  int onMinutes = onHour * 60 + onMin;
  int offMinutes = offHour * 60 + offMin;

  if (onMinutes == offMinutes) {
    return false;  // Off all day if on/off times are the same
  }

  if (onMinutes < offMinutes) {
    // Simple case: ON during same day window (e.g., 08:00–20:00)
    return currentMinutes >= onMinutes && currentMinutes < offMinutes;
  } else {
    // Over midnight (e.g., 20:00–08:00)
    return currentMinutes >= onMinutes || currentMinutes < offMinutes;
  }
}


void appendLog(const String& message) {
  String timestamp = getTimestamp();
  String logEntry = timestamp + " " + message + "\n";

  logText += logEntry + "<br>";
  if (logText.length() > 5000) logText = logText.substring(logText.length() - 5000);

  File file = SPIFFS.open("/log.txt", FILE_APPEND);
  if (file) {
    file.print(logEntry);
    file.close();
  }

  // Send to syslog if valid
  if (syslogIP) {
    String syslogMsg = "<134>G7NRU-Relay: " + message;

    syslogUdp.beginPacket(syslogIP, 514);

    size_t bytesWritten = syslogUdp.write((const uint8_t*)syslogMsg.c_str(), syslogMsg.length());
    bool packetSent = syslogUdp.endPacket(); // true if successfully queued

    // debugPrint("[SYSLOG] Sending to " + syslogIP.toString() + ": " + syslogMsg);
    }
  else 
    {
      // debugPrint("[SYSLOG] No IP");
    }
  

debugPrint(message);




}


void logHardwareInfo() {
  
  
  String info = "Boot Info: ";
  info += "CPU: " + String(ESP.getCpuFreqMHz()) + "MHz, ";
  info += "Free RAM: " + String(ESP.getFreeHeap() / 1024) + "KB, ";
  info += "Flash Size: " + String(ESP.getFlashChipSize() / (1024 * 1024)) + "MB, ";
  info += "Flash Speed: " + String(ESP.getFlashChipSpeed() / 1000000) + "MHz, ";

  if (SPIFFS.begin(true)) {  // SPIFFS already mounted at this point
    size_t totalBytes = SPIFFS.totalBytes();
    size_t usedBytes = SPIFFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;
    info += "SPIFFS: " + String(freeBytes / 1024) + "KB free of " + String(totalBytes / 1024) + "KB";
  } else {
    info += "SPIFFS mount failed";
  }

  appendLog(info);
  appendLog("Build date: " + String(buildDate) + " " + String(buildTime));
}


void loadConfig() {
  measureElapsedMs();
  debugPrint("Loading configuration from SPIFFS");
  File file = SPIFFS.open("/config.json", "r");
  if (!file) {
    debugPrint("No config found");
    for (int i = 0; i < 6; i++) {
      relayLabels[i] = "Relay " + String(i + 1);
      relayIPs[i] = "";
      relayPingEnabled[i] = false;
      relayResetEnabled[i] = false;
    }
    wifiSSID = "";
    wifiPassword = "";
    return;
  }

  JsonDocument doc;

  // Dump the entire config to see that what is there is what we expect
  file.seek(0);  // Rewind to start in case anything was read
  String rawConfig = file.readString();
  debugPrint("[DEBUG] Raw config.json contents:");
  debugPrint(rawConfig);
  file.seek(0);  // Rewind again for actual parsing



  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    debugPrint("Failed to parse config");
    return;
  }

  
 // Syslog IP
 String syslogStr = doc["syslog"] | "";
 syslogStr.trim();
 debugPrint("[CONFIG] Raw syslog IP string: '" + syslogStr + "'");
 if (!syslogStr.isEmpty() && syslogIP.fromString(syslogStr)) {
   debugPrint("[CONFIG] Syslog IP loaded: " + syslogIP.toString());
 } else {
   debugPrint("[CONFIG] Invalid syslog IP or not set.");
 }

 // Relay config
  for (int i = 0; i < 6; i++) {
    relayLabels[i] = doc["relayLabels"][i].as<String>();
    relayIPs[i] = doc["relayIPs"][i].as<String>();
    relayStates[i] = doc["relayStates"][i] | true;
    relayPingEnabled[i] = doc["pingEnabled"][i] | false;
    relayResetEnabled[i] = doc["resetEnabled"][i] | false;
  }

  // Declare sched here before you use it:
  JsonObject sched = doc["globalSchedule"];

  globalSchedule.enabled = sched["enabled"] | false;
  globalSchedule.powerOnTime = sched["powerOnTime"] | "06:00";
  globalSchedule.powerOffTime = sched["powerOffTime"] | "23:00";
  globalSchedule.pollIntervalMinutes = sched["pollIntervalMinutes"] | 10;
    
  
  debugPrintf("[CONFIG] Global Schedule: enabled=%s, on=%s, off=%s, poll=%dmin\n",
                globalSchedule.enabled ? "true" : "false",
                globalSchedule.powerOnTime.c_str(),
                globalSchedule.powerOffTime.c_str(),
                globalSchedule.pollIntervalMinutes);

  wifiSSID = doc["wifi"]["ssid"].as<String>();
  wifiPassword = doc["wifi"]["password"].as<String>();
  debugPrint("loadConfig elapsed: " + String(measureElapsedMs()) + " ms");
}

void saveConfig() {

  measureElapsedMs();
  debugPrint("Preparing configuration for save...");

  JsonDocument doc;
  auto labels = doc["relayLabels"].to<JsonArray>();
  auto ips = doc["relayIPs"].to<JsonArray>();
  auto states = doc["relayStates"].to<JsonArray>();
  auto pingEnabled = doc["pingEnabled"].to<JsonArray>();
  auto resetEnabled = doc["resetEnabled"].to<JsonArray>();
  
  for (int i = 0; i < 6; i++) {
    labels.add(relayLabels[i]);
    ips.add(relayIPs[i]);
    states.add(relayStates[i]);
    pingEnabled.add(relayPingEnabled[i]);
    resetEnabled.add(relayResetEnabled[i]);
  }

  // Global schedule object
  auto schedule = doc["globalSchedule"].to<JsonObject>();
  schedule["enabled"] = globalSchedule.enabled;
  schedule["powerOnTime"] = globalSchedule.powerOnTime;
  schedule["powerOffTime"] = globalSchedule.powerOffTime;
  schedule["pollIntervalMinutes"] = globalSchedule.pollIntervalMinutes;


  auto wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = wifiSSID;
  wifi["password"] = wifiPassword;

   // Save syslog IP
  doc["syslog"] = syslogIP.toString();
  /*
  debugPrintf("[CONFIG] Saved schedule: enabled=%d, on=%s, off=%s, pollInterval=%d\n",
              globalSchedule.enabled,
              globalSchedule.powerOnTime.c_str(),
              globalSchedule.powerOffTime.c_str(),
              globalSchedule.pollIntervalMinutes);  // no .c_str()
  */

  
  debugPrint("Opening config file for writing...");

  File file = SPIFFS.open("/config.json", "w");
  if (!file) {
    debugPrint("[ERROR] Failed to open /config.json for writing");
    return;
  }

  debugPrint("Writing configuration to file...");

  serializeJson(doc, file);

  debugPrint("Finished writing configuration to file");
  file.close();


  debugPrint("[CONFIG] Configuration saved to /config.json");
debugPrint("saveConfig elapsed: " + String(measureElapsedMs()) + " ms");

}

void toggleRelay(int i) {
  appendLog("Toggle Pin:" + String(relayPins[i]) + " - " + String(relayLabels[i]) + " " + String(relayStates[i]) + " >> " + String(!relayStates[i]));
  relayStates[i] = !relayStates[i];
  digitalWrite(relayPins[i], relayStates[i] ? RELAY_OFF : RELAY_ON);
}




void handleRoot() {
  measureElapsedMs();

  
  IPAddress clientIP = server.client().remoteIP();
  String logEntry = "Connection from IP: " + clientIP.toString();
  debugPrint(logEntry);

  // Optional: save to SPIFFS log file
  appendLog(logEntry);


  String html = loadHTMLPart("/header.html");
  html  += R"rawliteral(<div class="button-grid">)rawliteral";



  for (int i = 0; i < 6; i++) {
    String buttonClass = relayStates[i] ? "green active" : "red active";
  
    String dotStyle = "";
    if (relayPingEnabled[i] && relayResetEnabled[i]) {
      dotStyle = "style='--dot-color: yellow;'";
      buttonClass += " has-dot";
    } else if (relayPingEnabled[i]) {
      dotStyle = "style='--dot-color: blue;'";
      buttonClass += " has-dot";
    }
  
    html += "<button id='relay" + String(i) + "' class='" + buttonClass + "' " + dotStyle + " onclick='toggleRelay(" + i + ")'>" + relayLabels[i] + "</button>";
  }


  html += R"rawliteral(
  <button class="settings-button" onclick="location.href='/settings'">Settings</button>
  <button class="settings-button" onclick="location.href='/log'">View Log</button>
  <button class="reboot-button" onclick="location.href='/reboot'">Reboot</button>
</div>

)rawliteral";

  html += loadHTMLPart("/footer.html");
  server.send(200, "text/html", html);
  debugPrint("handleRoot elapsed: " + String(measureElapsedMs()) + " ms");
}

void handleToggle() {
  measureElapsedMs();

  debugPrint("Handling toggle request");
  if (server.hasArg("id")) {
    int id = server.arg("id").toInt();
    if (id >= 0 && id < 6) toggleRelay(id);
  }
  saveConfig();
  server.send(200, "text/plain", "OK");
  debugPrint("Toggle request handled successfully");
  debugPrint("handleToggle elapsed: " + String(measureElapsedMs()) + " ms");
}

void handleSettings() {
  measureElapsedMs();
  debugPrintf("Schedule enabled: %d\n", globalSchedule.enabled);
  debugPrintf("Power ON time: %s\n", globalSchedule.powerOnTime.c_str());
  debugPrintf("Power OFF time: %s\n", globalSchedule.powerOffTime.c_str());

  String html = loadHTMLPart("/header.html");

  html += R"rawliteral(
  <div class="container">
    <form action='/save'>
      <table class='settings-table'>
        <thead>
          <tr>
            <th>Label</th>
            <th>Enable Ping</th>
            <th>IP Address</th>
            <th>Reset on Fail</th>
          </tr>
        </thead>
        <tbody>
  )rawliteral";

  for (int i = 0; i < 6; i++) {
    html += "<tr>";
    html += "<td><input type='text' name='label" + String(i) + "' value='" + relayLabels[i] + "' maxlength='20'></td>";
    html += "<td><input type='checkbox' id='ping" + String(i) + "' name='ping" + String(i) + "'" + (relayPingEnabled[i] ? " checked" : "") + " onchange='togglePing(" + String(i) + ")'></td>";
    html += "<td><input type='text' id='ip" + String(i) + "' name='ip" + String(i) + "' value='" + relayIPs[i] + "' maxlength='20' pattern='^$|^(?:[0-9]{1,3}\\.){3}[0-9]{1,3}$' title='Enter a valid IPv4 address or leave blank'></td>";
    html += "<td><input type='checkbox' id='reset" + String(i) + "' name='reset" + String(i) + "'" + (relayResetEnabled[i] ? " checked" : "") + "></td>";
    html += "</tr>";
  }

  html += R"rawliteral(
        </tbody>
      </table>
      <br>
      <table class='settings-table'>
        <tr>
          <td><label for='globalSchedEnable'>Enable Deep Sleep Schedule</label></td>
          <td><input type='checkbox' id='globalSchedEnable' name='globalScheduleEnabled')rawliteral";
  html += globalSchedule.enabled ? " checked" : "";
  html += R"rawliteral(></td>
        </tr>
        <tr>
          <td><label for='globalOnTime'>Wake Time</label></td>
          <td><input type='time' id='globalOnTime' name='globalOnTime' value=')rawliteral";
  html += globalSchedule.powerOnTime;
  html += R"rawliteral('></td>
        </tr>
        <tr>
          <td><label for='globalOffTime'>Sleep Time</label></td>
          <td><input type='time' id='globalOffTime' name='globalOffTime' value=')rawliteral";
  html += globalSchedule.powerOffTime;
  html += R"rawliteral('></td>
        </tr>
        <tr>
          <td><label for='pollInterval'>Poll Interval (minutes)</label></td>
          <td><input type='number' id='pollInterval' name='pollInterval' min='1' max='1440' value=')rawliteral";
  html += String(globalSchedule.pollIntervalMinutes);
  html += R"rawliteral('></td>
        </tr>
      </table>

      <div class='form-buttons'>
        <button class='settings-button' type='submit' form='settingsForm'>Save Settings</button>
        <form action="/download_config" method="GET" style="display: inline;">
          <button class="settings-button" type="submit">Download Settings</button>
        </form>
        <form id="uploadForm" method="POST" action="/upload_config" enctype="multipart/form-data" style="display: inline;">
          <input type="file" id="uploadInput" name="upload" style="display: none;" required>
          <button class="settings-button" type="button" onclick="triggerUpload()">Upload Settings</button>
        </form>
        <a href="/" style="margin-left: 10px;">
          <button class="settings-button" type="button">Back</button>
        </a>
      </div>
    </form>
  </div>
  )rawliteral";

  html += loadHTMLPart("/footer.html");
  server.send(200, "text/html", html);
  debugPrint("handleSettings elapsed: " + String(measureElapsedMs()) + " ms");
}





void handleLogPage() {
  measureElapsedMs();

  String html = loadHTMLPart("/header.html");
  html += R"rawliteral(<div class="log-box">)rawliteral";
  if (SPIFFS.exists("/log.txt")) {
    File file = SPIFFS.open("/log.txt", "r");
    if (file) {
      while (file.available()) {
        //html += file.readStringUntil('\n') + "<br>";
        html += htmlEscape(file.readStringUntil('\n')) + "<br>";
      }
      file.close();
    }
  } else {
    html += "<p>No log file found.</p>";
  }

  html += R"rawliteral(
</div>
<div class="controls">
  <button class="settings-button" onclick="clearLog()">Clear Log</button>
  <button class="settings-button" onclick="location.href='/'">Back to Control Panel</button>
</div>
)rawliteral";

html += loadHTMLPart("/footer.html");
  server.send(200, "text/html", html);
  debugPrint("handleLogPage elapsed: " + String(measureElapsedMs()) + " ms");
}

void handleStatusApi() {
  measureElapsedMs();

  debugPrint("Handling API status request");
  JsonDocument doc;
  auto states = doc["states"].to<JsonArray>();
  auto labels = doc["labels"].to<JsonArray>();
  auto resetFlags = doc["reset"].to<JsonArray>();
  auto ips = doc["ips"].to<JsonArray>();

  for (int i = 0; i < 6; i++) {
    states.add(relayStates[i]);
    labels.add(relayLabels[i]);
    resetFlags.add(relayResetEnabled[i]);
    ips.add(relayIPs[i]);
  }

  // This will return log data but for now we wont use this so I will supress it
  // doc["log"] = logText;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
  debugPrint("API status response sent");
  debugPrint("handleStatusApi elapsed: " + String(measureElapsedMs()) + " ms");
}

void handleDownloadLog() {
  measureElapsedMs();

  if (SPIFFS.exists("/log.txt")) {
    File file = SPIFFS.open("/log.txt", "r");
    server.streamFile(file, "text/plain");
    file.close();
    appendLog("Log downloaded");
  } else {
    server.send(404, "text/plain", "Log file not found");
  }
  debugPrint("handleDownloadLog elapsed: " + String(measureElapsedMs()) + " ms");
}

void handleClearLog() {
  measureElapsedMs();

  if (SPIFFS.exists("/log.txt")) {
    SPIFFS.remove("/log.txt");
  }
  File logFile = SPIFFS.open("/log.txt", FILE_WRITE);
  debugPrint("Trying to clear the log..");
  if (logFile) {
    logFile.print(""); // Not strictly necessary, but keeps logic clear
    logFile.close();
    appendLog("Log cleared");
    server.send(200, "text/plain", "Log cleared");
  } else {
    appendLog("Failed to clear the log");
    server.send(500, "text/plain", "Failed to clear log");
  }
  debugPrint("handleClearLog elapsed: " + String(measureElapsedMs()) + " ms");
}

void handleSave() {
  measureElapsedMs();

  debugPrint("[SAVE] Handling settings save...");

  for (int i = 0; i < 6; i++) {
    relayLabels[i] = server.arg("label" + String(i));
    relayIPs[i] = server.arg("ip" + String(i));
    relayPingEnabled[i] = server.hasArg("ping" + String(i));
    relayResetEnabled[i] = server.hasArg("reset" + String(i));

    debugPrintf("[SAVE] Relay %d - Label: %s, IP: %s, Ping: %s, Reset: %s\n",
                  i,
                  relayLabels[i].c_str(),
                  relayIPs[i].c_str(),
                  relayPingEnabled[i] ? "ENABLED" : "DISABLED",
                  relayResetEnabled[i] ? "ENABLED" : "DISABLED");
  }

  // Save global deep sleep schedule
  globalSchedule.enabled = server.hasArg("globalScheduleEnabled");
  debugPrintf("[SAVE] Global schedule enabled: %s\n", globalSchedule.enabled ? "YES" : "NO");

  if (server.hasArg("globalOnTime")) {
    globalSchedule.powerOnTime = server.arg("globalOnTime");
    debugPrintf("[SAVE] Global On Time set to: %s\n", globalSchedule.powerOnTime.c_str());
  }

  if (server.hasArg("globalOffTime")) {
    globalSchedule.powerOffTime = server.arg("globalOffTime");
    debugPrintf("[SAVE] Global Off Time set to: %s\n", globalSchedule.powerOffTime.c_str());
  }

  if (server.hasArg("pollInterval")) {
    globalSchedule.pollIntervalMinutes = server.arg("pollInterval").toInt();
    if (globalSchedule.pollIntervalMinutes > 1440) {
      globalSchedule.pollIntervalMinutes = 5;
    } 
    debugPrintf("[SAVE] Poll interval set to: %d minutes\n", globalSchedule.pollIntervalMinutes);
  }


  saveConfig();
  debugPrint("[SAVE] Configuration saved. Redirecting to main page.");

  server.sendHeader("Location", "/");
  server.send(303);
  debugPrint("handleSave elapsed: " + String(measureElapsedMs()) + " ms");
}

void handleReboot() {
  measureElapsedMs();

  String html = loadHTMLPart("/header.html");
  html += "<h1>Rebooting...</h1><p>Check the log for confirmation</p><script>setPageTimeout();</script>";
  html += loadHTMLPart("/footer.html");
  server.send(200, "text/html", html);
  debugPrint("handleReboot elapsed: " + String(measureElapsedMs()) + " ms");
  
  rebootPending = true; // set the reboot flag
  rebootStartTime = millis(); // remember the time  

}

void goToDeepSleep(uint32_t sleepSeconds) {
  debugPrintf("Going to sleep for %u seconds...\n", sleepSeconds);
  delay(100);  // Allow Serial flush

  // Stop Wi-Fi to reduce power
  WiFi.disconnect(true); // Disconnect and turn off Wi-Fi
  WiFi.mode(WIFI_OFF);

  // Set all RTC-capable GPIOs to input to reduce current
  for (gpio_num_t gpio = GPIO_NUM_0; gpio < GPIO_NUM_MAX; gpio = (gpio_num_t)(gpio + 1)) {
    if (!rtc_gpio_is_valid_gpio(gpio)) continue;
    rtc_gpio_deinit(gpio);           // Reset RTC GPIO function
    gpio_reset_pin(gpio);            // Reset to default
    gpio_set_direction(gpio, GPIO_MODE_INPUT); // Set as input
    gpio_pullup_dis(gpio);           // Disable pull-up
    gpio_pulldown_dis(gpio);         // Disable pull-down
  }

  // Enable wake-up timer
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);

  // Start deep sleep
  esp_deep_sleep_start();
}

void handleDeepSleep() {
  measureElapsedMs();

  server.send(200, "text/plain", "Good night :-)");
  goToDeepSleep(30);
  return;

}



void handleFileUpload() {
  measureElapsedMs();

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    debugPrint("Upload Start");
    if (SPIFFS.exists("/config.json")) {
      SPIFFS.remove("/config.json");
      debugPrint("Old config.json removed");
    }
    uploadFile = SPIFFS.open("/config.json", FILE_WRITE);
    lastUploadSize = 0;
    if (!uploadFile) {
      debugPrint("Failed to open file for writing");
      return;
    } else {
      debugPrint("Opened config.json for writing");
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      size_t chunkSize = upload.currentSize - lastUploadSize;
      size_t written = uploadFile.write(upload.buf, chunkSize);
      lastUploadSize = upload.currentSize;
      debugPrintf("Wrote %u bytes, total uploaded: %u\n", written, lastUploadSize);
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    debugPrint("Upload Complete");
    if (uploadFile) {
      uploadFile.close();
      debugPrint("File closed");
    }

    // Optional: Verify uploaded file
    File f = SPIFFS.open("/config.json", FILE_READ);
    if (f) {
      debugPrintf("Final file size: %u bytes\n", f.size());

      // Print preview of contents
      char preview[101] = {0};
      size_t len = f.readBytes(preview, 100);
      debugPrintf("First %u bytes: %s\n", len, preview);

      f.close();
    } else {
      debugPrint("Failed to open uploaded file for verification");
    }

    // SPIFFS does not support file modification timestamps directly,
    // but you can log the time manually if you have a time source
    appendLog("New config uploaded at " + String(millis()) + "ms uptime");

    handleReboot();
  }
  debugPrint("handleFileUpload elapsed: " + String(measureElapsedMs()) + " ms");
}



SX1262 lora = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN);

void setup() {
  Serial.begin(115200);



  debugPrint("============================================");
  debugPrint("Booting the G7NRU Remote Station Controller");
  debugPrint("             Version 2.0");
  debugPrint("============================================");


/*
Display stuf for another day

  Wire.begin(17, 18);  // SDA=17, SCL=18 for your board
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 24, "G7NRU Remote Lora Station Controller");
  u8g2.sendBuffer();
  delay(1000); // Give time for the display to show the message
  debugPrint("Starting setup...");
*/

  if (!SPIFFS.begin(true)) {
    debugPrint("SPIFFS mount failed");
    return;
  }

  debugPrint("Logging Hardware Info");

  // logHardwareInfo();

  debugPrint("Loading Config");
  loadConfig();


  if (!shouldBeOnBySchedule())
    {
      debugPrint("I should be sleeping. Need to check for a LoRa message");
        
      // Pole for Lora  message. 

      // if (!validWakeMessage) {...
      
      // goToDeepSleep(globalSchedule.pollIntervalMinutes * 60);

      // ...}
    }
    else
    {
      debugPrint("I should wake");
    }


  // if !wake = true
  // Call deepsleep again..
      


  // Here we are looking at setting the relays according to the saved config. And setting up the onboard LED as an output

  debugPrint("Setting relays");
  for (int i = 0; i < 6; i++) {
       
    pinMode(relayPins[i], OUTPUT);
    debugPrint(String("Init GPIO Pin ") + relayPins[i] + " for Relay " + i);
    digitalWrite(relayPins[i], relayStates[i] ? RELAY_OFF : RELAY_ON);
    //delay(500);
  }

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);
 
  
  debugPrint("Setting up LoRa radio");

  // LoRa SX1262 setup
  SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);
  int state = lora.begin(439.9125); // Set frequency to 433 MHz
  if (state == RADIOLIB_ERR_NONE) {
    lora.setBandwidth(125.0);        // 125 kHz bandwidth
    lora.setSpreadingFactor(12);      // SF12
    lora.setCodingRate(5);            // 4/5 coding rate
    lora.setOutputPower(14);         // Set output power to 14 dBm
    lora.setSyncWord(0x12);          // LoRaWAN public sync word
    Serial.println("LoRa SX1262 configured!");
  } else {
    Serial.print("LoRa SX1262 init failed, code ");
    Serial.println(state);
    while (1);
  }
  
  
  
  
  
  // Setting up wifi and connecting to the network

  WiFi.setHostname("RelayController");  // Set this to something unique and descriptive
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  debugPrint("Trying WiFi SSID: " + wifiSSID + " | Password: " + wifiPassword);
  delay(500);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    debugPrint(".");
  }
  debugPrint(" -> Connected to wifi. Local allocated IP: " + WiFi.localIP().toString());

  // Setup NTP and sync
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  
    server.on("/style.css", HTTP_GET, []() {
    File file = SPIFFS.open("/style.css", "r");
    if (!file) {
      server.send(404, "text/css", "");
      return;
    }
    server.sendHeader("Cache-Control", "max-age=86400"); // cache for 1 day
    server.streamFile(file, "text/css");
    file.close();
  });
  
  server.on("/script.js", HTTP_GET, []() {
    File file = SPIFFS.open("/script.js", "r");
    if (!file) {
      server.send(404, "application/javascript", "");
      return;
    }
    server.sendHeader("Cache-Control", "max-age=86400"); // cache for 1 day
    server.streamFile(file, "application/javascript");
    file.close();
  });
  
  

  server.on("/logo.png", HTTP_GET, []() {
  File file = SPIFFS.open("/logo.png", "r");
  if (!file) {
    server.send(404, "image/png", "");
    return;
  }
  server.sendHeader("Cache-Control", "max-age=86400"); // cache for 1 day
  server.streamFile(file, "image/png");
  file.close();
});
  //server.serveStatic("/logo.png", SPIFFS, "/logo.png");
  
  
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.on("/settings", handleSettings);
  server.on("/log", handleLogPage);
  server.on("/api/status", handleStatusApi);
  server.on("/download_log", handleDownloadLog);
  server.on("/clearlog", HTTP_GET, handleClearLog);
  server.on("/save", handleSave);
  server.on("/reboot", handleReboot);
  server.on("/deepsleep", handleDeepSleep);
  server.on("/download_config", HTTP_GET, []() {
    File configFile = SPIFFS.open("/config.json", "r");
    if (!configFile) {
      server.send(404, "text/plain", "File not found");
      return;
    }

    // Manually set the headers to force download
    // server.sendHeader("Content-Type", "application/json");
    // server.sendHeader("Content-Disposition", "attachment; filename=config.json");
    // server.sendHeader("Cache-Control", "no-cache");

    // Stream the file
    server.streamFile(configFile, "application/json");
    configFile.close();
  });
  server.on("/upload_config", HTTP_POST, []() 
    {
    // server.send(200, "text/plain", "Upload handled");
    server.sendHeader("Location", "/settings");  // or "/"
    server.send(303);  // HTTP 303 See Other, redirect after POST

    debugPrint("Procesing uploaded file");
    }, handleFileUpload);
  
  server.begin();
}

void loop() {
  //debugPrint(">>");
  server.handleClient();
  //debugPrint("<<");
  
  String currentTime = getCurrentTimeStr();
  


  if (rebootPending && millis() - rebootStartTime > 1000) {  // 10 seconds
    appendLog("Rebooting ESP32");
    ESP.restart();
  }


  // Light pulses from the LED so that we know the ESP32 is stull processing this loop

  if (millis() - lastStatusFlashOn > 3000) {
    lastStatusFlashOn = millis();
    // debugPrint("LED ON");
    digitalWrite(ledPin, HIGH);  // Turn LED on
  }
  else if (millis() - lastStatusFlashOff > 1500) {
    lastStatusFlashOff = millis();
    // debugPrint("LED OFF");
    digitalWrite(ledPin, LOW);   // Turn LED off
  }


  // --- Ping test logic ---
  if (millis() - lastPingTime > 300000) {
    // Before we do the pings, lets see if we should be sleeping and if so, sleepy time
    if (!shouldBeOnBySchedule())
        {
          debugPrint("I should be sleeping - Going to sleep now");
          //goToDeepSleep(globalSchedule.pollIntervalMinutes * 60);
        }

    lastPingTime = millis();
    for (int i = 0; i < 6; i++) {
        if (relayPingEnabled[i] && relayIPs[i].length() > 0) {
        IPAddress ip;
        if (ip.fromString(relayIPs[i])) {
          int failed = 0;
          /*
          if (!Ping.ping(ip, 5))  {
            appendLog("Ping failed for " + relayLabels[i]);
            if (relayResetEnabled[i]) {
              appendLog("Resetting " + relayLabels[i]);
              appendLog("-- Toggle over ...  " + relayLabels[i]);
              toggleRelay(i);
              delay(1000);
              toggleRelay(i);
              appendLog("-- Toggle back ...  " + relayLabels[i]);
            }
          } else {
            appendLog("Ping OK for " + relayLabels[i] + " (" + ip.toString() + ")");
          }
            */
        } else {
          appendLog("Invalid IP for " + relayLabels[i]);
        }
      }
    
    }
  }
  
  // LoRa receive
    String incoming;
    int rxState = lora.receive(incoming, 0); // 0 = non-blocking
    if (rxState == RADIOLIB_ERR_NONE) {
        Serial.print("[LoRa RX] Received: ");
        Serial.println(incoming);
    }

}



