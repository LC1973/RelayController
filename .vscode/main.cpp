//-------------------------------------------------------
// Version : 0.1                              
// Comments: 
// The Relay control logic is good. 
// Style sheet is OK                                
// Settings is there
// Compiles cleanly
//-------------------------------------------------------

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ESP32Ping.h>
#include <time.h>

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

const int relayPins[6] = {14, 27, 26, 25, 33, 32};
bool relayStates[6] = {false, false, false, false, false, false};
String relayLabels[6];
String relayIPs[6];
bool relayPingEnabled[6] = {false};
bool relayResetEnabled[6] = {false};

String wifiSSID = "";
String wifiPassword = "";
WebServer server(80);
unsigned long lastPingTime = 0;
String logText = "";


String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "[NTP FAIL]";
  }
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}



// funciton to write to the log file on the local storage
void appendLog(const String& message) {
  String timestamp = getTimestamp();
  String logEntry = timestamp + " " + message + "\n";

  // Append to RAM (optional if you want fast page view updates)
  logText += logEntry + "<br>";
  if (logText.length() > 5000) logText = logText.substring(logText.length() - 5000);

  // Append to SPIFFS
  File file = SPIFFS.open("/log.txt", FILE_APPEND);
  if (file) {
    file.print(logEntry);
    file.close();
  }
}
// load config data from the config.json file on local storage
void loadConfig() {
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
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    debugPrint("Failed to parse config");
    return;
  }

  for (int i = 0; i < 6; i++) {
    relayLabels[i] = doc["relayLabels"][i].as<String>();
    relayIPs[i] = doc["relayIPs"][i].as<String>();
    relayStates[i] = doc["relayStates"][i] | true;
    relayPingEnabled[i] = doc["pingEnabled"][i] | false;
    relayResetEnabled[i] = doc["resetEnabled"][i] | false;
  }

  wifiSSID = doc["wifi"]["ssid"].as<String>();
  wifiPassword = doc["wifi"]["password"].as<String>();
}

// save config data from the config.json file on local storage
void saveConfig() {
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

  auto wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = wifiSSID;
  wifi["password"] = wifiPassword;

  File file = SPIFFS.open("/config.json", "w");
  serializeJson(doc, file);
  file.close();
}

// Simple relay toggle - if its on, turn if off. If its off, turn it on.
void toggleRelay(int i) {
  appendLog("Relay Switch ESP32 Pin:" + String(relayPins[i]) + " - " + String(relayLabels[i]) + " " + String(relayStates[i]) + " >> " + String(!relayStates[i]));
  relayStates[i] = !relayStates[i];
  digitalWrite(relayPins[i], relayStates[i] ? RELAY_OFF : RELAY_ON);
}


void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <link rel='stylesheet' href='/style.css'>
  <title>G7NRU Remote Power Switch</title>
</head>
<body>
<header class="main-header">
  <img class="logo" src="/G7NRU_RPS.png" alt="Logo">
</header>
  <div class="button-grid">
)rawliteral";

  for (int i = 0; i < 6; i++) {
    String buttonClass = relayStates[i] ? "green active" : "red active";
    html += "<button id='relay" + String(i) + "' class='" + buttonClass + "' onclick='toggleRelay(" + i + ")'>" + relayLabels[i] + "</button>";
  }

  html += R"rawliteral(
  </div>
  <div class="controls">
    <button class='settings-button' onclick="location.href='/settings'">Settings</button><br>
    <button class='settings-button' onclick="location.href='/log'">View Log</button>
    <button class='reboot-button' onclick="location.href='/reboot'">Reboot</button>
  </div>
  

<script>
function toggleRelay(id) {
  fetch('/toggle?id=' + id)
    .then(() => updateStatus());
}

function updateStatus() {
  fetch('/api/status')
    .then(response => response.json())
    .then(data => {
      for (var i = 0; i < data.states.length; i++) {
        var button = document.getElementById('relay' + i);
        if (!button) continue;

        button.className = data.states[i] ? 'green active' : 'red active';

        let label = data.labels[i];

        if (data.ping[i] && data.reset[i]) {
          label += " ⚡🔄"; // ping AND reset
        } else if (data.ping[i]) {
          label += " ⚡";   // ping only
        } else if (data.reset[i]) {
          label += " 🔄";   // reset only
        }

        button.innerText = label; // <<< IMPORTANT: use innerText, not innerHTML
      }
    });
}
setInterval(updateStatus, 10000);
</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleDownloadLog() {
  if (SPIFFS.exists("/log.txt")) {
    File file = SPIFFS.open("/log.txt", "r");
    server.streamFile(file, "text/plain");
    file.close();
    appendLog("Log downloaded");
  } else {
    server.send(404, "text/plain", "Log file not found");
  }
}


void handleToggle() {
  if (server.hasArg("id")) {
    int id = server.arg("id").toInt();
    if (id >= 0 && id < 6) toggleRelay(id);
  }
  saveConfig();
  server.send(200, "text/plain", "OK");  // No redirect
}



void handleSettings() {
String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel='stylesheet' href='/style.css'>
    <title>Settings - G7NRU Remote Power Switch</title>
    <style>
      body {
        background-color: #121212;
        color: #ffffff;
        font-family: sans-serif;
        margin: 0;
        padding: 20px;
      }
      h1 {
        text-align: center;
        margin-bottom: 20px;
      }
      table {
        width: 100%;
        max-width: 600px;
        margin: auto;
        border-collapse: collapse;
      }
      td {
        padding: 10px;
        vertical-align: top;
      }
      input[type="text"] {
        width: 100%;
        padding: 8px;
        border-radius: 6px;
        border: none;
        background-color: #1e1e1e;
        color: #ffffff;
      }
      input[type="checkbox"] {
        transform: scale(1.2);
        margin-right: 8px;
      }
      .save-button {
        display: block;
        margin: 30px auto;
        padding: 12px 30px;
        font-size: 18px;
        border: none;
        border-radius: 8px;
        background-color: #03dac6;
        color: #000000;
        cursor: pointer;
        transition: background-color 0.3s;
      }
      .save-button:hover {
        background-color: #018786;
      }
      .back-link {
        display: block;
        text-align: center;
        margin-top: 20px;
        color: #03dac6;
        text-decoration: none;
      }
    </style>
  </head>
  <body>
    <h1>Settings</h1>
    <form action='/save'>
      <table>
  )rawliteral";
  
    for (int i = 0; i < 6; i++) {
      html += "<tr><td>Label:</td><td><input type='text' name='label" + String(i) + "' value='" + relayLabels[i] + "'></td></tr>";
      html += "<tr><td>IP Address:</td><td><input type='text' name='ip" + String(i) + "' value='" + relayIPs[i] + "'></td></tr>";
      html += "<tr><td colspan='2'>";
      html += "<label><input type='checkbox' name='ping" + String(i) + "'" + (relayPingEnabled[i] ? " checked" : "") + ">Enable Ping Monitoring</label><br>";
      html += "<label><input type='checkbox' name='reset" + String(i) + "'" + (relayResetEnabled[i] ? " checked" : "") + ">Reset on Failure</label>";
      html += "</td></tr>";
      html += "<tr><td colspan='2'><hr style='border-color:#333;'></td></tr>";
    }
  
    html += R"rawliteral(
      </table>
      <button class="settings-button" type="submit">Save Settings</button>
    </form>
  </body>
  </html>
  )rawliteral";
  
    server.send(200, "text/html", html);
  }

void handleStatusApi() {

/*  JsonDocument doc;
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
  doc["log"] = logText;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);*/

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

  // Read the latest log from /log.txt
  String fullLog = "";
  if (SPIFFS.exists("/log.txt")) {
    File logFile = SPIFFS.open("/log.txt", "r");
    if (logFile) {
      while (logFile.available()) {
        fullLog += logFile.readStringUntil('\n') + "<br>";
      }
      logFile.close();
    }
  }

  doc["log"] = fullLog;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);

}

/*
void handleLogPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <link rel='stylesheet' href='/style.css'>
  <title>G7NRU Log Viewer</title>
</head>
<body>
  <header class="main-header">
    <img class="logo" src="/G7NRU_RPS.png" alt="Logo">
  </header>

  <div class="log-container">
    <h2>System Log</h2>
    <div id="log-box" class="log-box">)rawliteral" + logText + R"rawliteral(</div>
    <button class="settings-button" onclick="location.href='/'">Back to Control Panel</button>
    <button class="settings-button" onclick="location.href='/download_log'">Download Log</button>
  </div>

  <script>
    // Auto-scroll to bottom
    var logBox = document.getElementById('log-box');
    logBox.scrollTop = logBox.scrollHeight;
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}
*/


void handleLogPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <link rel='stylesheet' href='/style.css'>
  <title>System Log - G7NRU Remote Power Switch</title>
  <style>
    body {
      background-color: #121212;
      color: #ffffff;
      font-family: sans-serif;
      margin: 0;
      padding: 20px;
    }
    h2 {
      text-align: center;
    }
    .log-box {
      background-color: #1e1e1e;
      padding: 10px;
      border-radius: 8px;
      overflow-y: scroll;
      height: 400px;
      margin-bottom: 20px;
    }
    .controls {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 10px;
    }
    .settings-button {
      padding: 12px 30px;
      font-size: 18px;
      border: none;
      border-radius: 8px;
      background-color: #03dac6;
      color: #000000;
      cursor: pointer;
      transition: background-color 0.3s;
      width: 80%;
      max-width: 300px;
    }
    .settings-button:hover {
      background-color: #018786;
    }
  </style>
</head>
<body>
  <h2>System Log</h2>
  <div class="log-box" id="log-box">
)rawliteral";

  // Read log.txt and inject it
  if (SPIFFS.exists("/log.txt")) {
    File file = SPIFFS.open("/log.txt", "r");
    if (file) {
      while (file.available()) {
        html += file.readStringUntil('\n') + "<br>";
      }
      file.close();
    }
  } else {
    html += "<p>No log file found.</p>";
  }

  html += R"rawliteral(
  </div>

  <div class="controls">
    <button class="settings-button" onclick="location.href='/download_log'">Download Log</button>
    <button class="settings-button" onclick="clearLog()">Clear Log</button>
    <button class="settings-button" onclick="location.href='/'">Back to Control Panel</button>
  </div>

    <script>
    function clearLog() {
      if (confirm('Are you sure you want to clear the log?')) {
        fetch('/clearlog')
          .then(response => {
            if (response.ok) {
              location.reload(); // Reload page to refresh the cleared log
            } else {
              alert('Failed to clear log.');
            }
          });
      }
    }
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSave() {
  for (int i = 0; i < 6; i++) {
    relayLabels[i] = server.arg("label" + String(i));
    relayIPs[i] = server.arg("ip" + String(i));
    relayPingEnabled[i] = server.hasArg("ping" + String(i));
    relayResetEnabled[i] = server.hasArg("reset" + String(i));
  }
  saveConfig();
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  
  if (!SPIFFS.begin(true)) {
    debugPrint("SPIFFS mount failed");
    return;
  }

  loadConfig();

  debugPrint("\nInisialising Relay control pins");
  for (int i = 0; i < 6; i++) {
    // These two things need to be next to each other. If there is a gap between them then the relays will 
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], relayStates[i] ? RELAY_OFF : RELAY_ON);
  }

  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    debugPrint(".");
  }
  
  debugPrint("\nConnected to WiFi");
  // Setup NTP time
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

   

  server.serveStatic("/logo.png", SPIFFS, "/logo.png");
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.on("/settings", handleSettings);
  server.on("/log", handleLogPage);
  server.on("/api/status", handleStatusApi);
  server.on("/download_log", handleDownloadLog);

  server.on("/clearlog", HTTP_GET, [](AsyncWebServerRequest *request){
    File logFile = SPIFFS.open("/log.txt", FILE_WRITE);
    if (logFile) {
      logFile.print("");  // Empty the file
      logFile.close();
      request->send(200, "text/plain", "Log cleared");
    } else {
      request->send(500, "text/plain", "Failed to clear log");
    }
  });


  server.on("/reboot", []()  {
    String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
    <head>
      <meta http-equiv="refresh" content="5; url=/" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>Rebooting...</title>
      <style>
        body {
          font-family: sans-serif;
          text-align: center;
          padding-top: 50px;
          background-color: #121212;
          color: #ffffff;
        }
      </style>
    </head>
    <body>
      <h1>Rebooting...</h1>
      <p>The ESP32 will return to the main page in 5 seconds.</p>
    </body>
  </html>
  )rawliteral";
  
    server.send(200, "text/html", html);
    delay(1000); // Give browser time to receive response
    ESP.restart();
  });
  

  server.serveStatic("/G7NRU_RPS.png", SPIFFS, "/G7NRU_RPS.png");

  server.on("/save", handleSave);
  /*server.on("/style.css", HTTP_GET, []() {
    File file = SPIFFS.open("/style.css", "r");
    server.streamFile(file, "text/css");
    file.close();
  });*/
  server.on("/style.css", HTTP_GET, []() {
    File file = SPIFFS.open("/style.css", "r");
    server.sendHeader("Cache-Control", "public, max-age=86400"); // 1 day
    server.streamFile(file, "text/css");
    file.close();
  });
  

  server.begin();
}

void loop() {
  server.handleClient();

  if (millis() - lastPingTime > 60000) {
    lastPingTime = millis();
    for (int i = 0; i < 6; i++) {
      if (relayPingEnabled[i] && relayIPs[i].length() > 0) {
        IPAddress ip;
        if (ip.fromString(relayIPs[i])) {
          int failed = 0;
          for (int j = 0; j < 10; j++) {
            if (!Ping.ping(ip, 1)) failed++;
            delay(10);
          }
          if (failed == 10) {
            appendLog("Ping failed for " + relayLabels[i]);
            if (relayResetEnabled[i]) {
              appendLog("Resetting " + relayLabels[i]);
              toggleRelay(i);
              delay(10000);
              toggleRelay(i);
            }
          } else {
            appendLog("Ping OK for " + relayLabels[i]+ " (" + ip.toString() + ")");
          }
        } else {
          appendLog("Invalid IP for " + relayLabels[i]+ " (" + ip.toString() + ")");
        }
      }
    }
  }
}
