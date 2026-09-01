#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <driver/temp_sensor.h>
#include <esp_system.h>
#include <esp_wifi.h>

#define OLED_SDA_PIN     5
#define OLED_SCL_PIN     6
#define BOOT_BUTTON_PIN  9
#define EXT_BUTTON_PIN   4
#define LED_PIN          8

// P1 Okosmérő (DSMR) Csatlakozás
#define P1_RX_PIN        3  // RJ12 Pin 5 (Inverted Data) -> ESP32-C3 GPIO3
#define P1_RTS_PIN       7  // RJ12 Pin 2 (Data Request)  -> ESP32-C3 GPIO7 (HIGH)

#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WebServer server(80);
Preferences preferences;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Dinamikus Wi-Fi Beállítások (NVS Memóriából)
char wifi_ssid[64] = "";
char wifi_pass[64] = "";

// Saját Hotspot (AP Mód) Beállítások
const char* ap_ssid = "ESP32-SuperMini";
const char* ap_pass = "12345678";
IPAddress ap_local_ip(192, 168, 4, 1);
IPAddress ap_gateway(192, 168, 4, 1);
IPAddress ap_subnet(255, 255, 255, 0);

// MQTT Broker Beállítások
char mqtt_server[64] = "192.168.0.253"; // Raspberry Pi 24/7 Server
int mqtt_port = 1883;
char mqtt_user[32] = "";
char mqtt_pass[32] = "";

const char* mqtt_topic_telemetry = "esp32c3/supermini/telemetry";
const char* mqtt_topic_p1 = "esp32c3/p1/telegram";
const char* mqtt_topic_state = "esp32c3/supermini/state";
const char* mqtt_topic_cmd = "esp32c3/supermini/cmd";

bool ledState = false;
int buttonPressCount = 0;
bool lastBootState = HIGH;
bool lastExtState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long lastMqttPublish = 0;
unsigned long lastMqttReconnectAttempt = 0;
bool tempSensorInitialized = false;

// P1 Mérőműszer adatok
float p1_current_power_kw = 0.0;
float p1_total_energy_kwh = 0.0;
float p1_current_export_kw = 0.0;
int p1_telegram_count = 0;
String p1_raw_buffer = "";

int getRssiPercent(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

void loadSettings() {
  preferences.begin("settings", false);
  String s_ssid = preferences.getString("ssid", "");
  String s_pass = preferences.getString("pass", "");
  String s_mqtt = preferences.getString("mqtt_ip", "192.168.0.253");
  int s_port = preferences.getInt("mqtt_port", 1883);
  String s_user = preferences.getString("mqtt_user", "");
  String s_mpass = preferences.getString("mqtt_pass", "");
  preferences.end();

  strncpy(wifi_ssid, s_ssid.c_str(), sizeof(wifi_ssid));
  strncpy(wifi_pass, s_pass.c_str(), sizeof(wifi_pass));
  strncpy(mqtt_server, s_mqtt.c_str(), sizeof(mqtt_server));
  mqtt_port = s_port;
  strncpy(mqtt_user, s_user.c_str(), sizeof(mqtt_user));
  strncpy(mqtt_pass, s_mpass.c_str(), sizeof(mqtt_pass));

  Serial.printf("💾 Beállítások betöltve: Wi-Fi='%s' | MQTT='%s:%d'\n", wifi_ssid, mqtt_server, mqtt_port);
}

void saveSettings() {
  preferences.begin("settings", false);
  preferences.putString("ssid", wifi_ssid);
  preferences.putString("pass", wifi_pass);
  preferences.putString("mqtt_ip", mqtt_server);
  preferences.putInt("mqtt_port", mqtt_port);
  preferences.putString("mqtt_user", mqtt_user);
  preferences.putString("mqtt_pass", mqtt_pass);
  preferences.end();
  Serial.println("💾 Új beállítások elmentve az NVS memóriába!");
}

void initTempSensor() {
  temp_sensor_config_t tsens = TSENS_CONFIG_DEFAULT();
  if (temp_sensor_set_config(tsens) == ESP_OK) {
    if (temp_sensor_start() == ESP_OK) {
      tempSensorInitialized = true;
      Serial.println("✅ ESP32-C3 Belső Hőmérséklet Érzékelő elindult!");
    }
  }
}

float getChipTemperature() {
  float result = 0.0;
  if (tempSensorInitialized) {
    temp_sensor_read_celsius(&result);
  }
  return result;
}

const char* getResetReasonString() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW_RESET";
    case ESP_RST_PANIC:     return "CRASH_PANIC";
    case ESP_RST_INT_WDT:   return "WDT_RESET";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "UNKNOWN";
  }
}

void updateOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("ESP32-C3 Smart Meter");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    float tempC = getChipTemperature();

    display.setCursor(0, 13);
    display.printf("STA:%s\n", WiFi.localIP().toString().c_str());

    display.setCursor(0, 25);
    display.printf("AP:192.168.4.1 | P1:#%d\n", p1_telegram_count);

    display.setCursor(0, 37);
    display.printf("P1 Pwr: %.3f kW\n", p1_current_power_kw);

    display.setCursor(0, 49);
    display.printf("Temp:%.1fC | MQTT:%s\n", tempC, mqttClient.connected() ? "OK" : "KI");
  } else {
    display.setCursor(0, 15);
    display.println("AP: 192.168.4.1 (OK)");
    display.setCursor(0, 27);
    display.println("SSID: ESP32-SuperMini");
    display.setCursor(0, 39);
    display.println("Pass: 12345678");
    display.setCursor(0, 51);
    if (strlen(wifi_ssid) > 0) {
      display.printf("STA: %s...\n", wifi_ssid);
    } else {
      display.println("STA: Konfigurasra var");
    }
  }

  display.display();
}

void parseP1Line(String line) {
  if (line.indexOf("1-0:1.7.0") >= 0) {
    int start = line.indexOf('(');
    int end = line.indexOf("*kW");
    if (start >= 0 && end > start) {
      p1_current_power_kw = line.substring(start + 1, end).toFloat();
    }
  }
  else if (line.indexOf("1-0:1.8.1") >= 0 || line.indexOf("1-0:1.8.0") >= 0) {
    int start = line.indexOf('(');
    int end = line.indexOf("*kWh");
    if (start >= 0 && end > start) {
      p1_total_energy_kwh = line.substring(start + 1, end).toFloat();
    }
  }
  else if (line.indexOf("2-0:2.7.0") >= 0 || line.indexOf("1-0:2.7.0") >= 0) {
    int start = line.indexOf('(');
    int end = line.indexOf("*kW");
    if (start >= 0 && end > start) {
      p1_current_export_kw = line.substring(start + 1, end).toFloat();
    }
  }
}

void readP1Serial() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    p1_raw_buffer += c;

    if (c == '\n') {
      p1_raw_buffer.trim();
      if (p1_raw_buffer.startsWith("!")) {
        p1_telegram_count++;
        Serial.printf("⚡ P1 Telegram #%d beérkezett!\n", p1_telegram_count);
        updateOLED();
      } else {
        parseP1Line(p1_raw_buffer);
      }
      p1_raw_buffer = "";
    }
  }
}

void publishMQTTTelemetry() {
  if (!mqttClient.connected()) return;

  float tempC = getChipTemperature();
  int freeRamKb = ESP.getFreeHeap() / 1024;
  int minRamKb = ESP.getMinFreeHeap() / 1024;
  int rssi = WiFi.RSSI();

  String payload = "{";
  payload += "\"temp_c\":" + String(tempC, 1) + ",";
  payload += "\"free_ram_kb\":" + String(freeRamKb) + ",";
  payload += "\"min_ram_kb\":" + String(minRamKb) + ",";
  payload += "\"wifi_rssi_dbm\":" + String(rssi) + ",";
  payload += "\"wifi_rssi_pct\":" + String(getRssiPercent(rssi)) + ",";
  payload += "\"button_press_count\":" + String(buttonPressCount) + ",";
  payload += "\"led_state\":\"" + String(ledState ? "ON" : "OFF") + "\",";
  payload += "\"p1_current_power_kw\":" + String(p1_current_power_kw, 3) + ",";
  payload += "\"p1_total_energy_kwh\":" + String(p1_total_energy_kwh, 1) + ",";
  payload += "\"p1_current_export_kw\":" + String(p1_current_export_kw, 3) + ",";
  payload += "\"p1_telegram_count\":" + String(p1_telegram_count) + ",";
  payload += "\"ip_address\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"ap_ip_address\":\"" + WiFi.softAPIP().toString() + "\",";
  payload += "\"mac_address\":\"" + WiFi.macAddress() + "\",";
  payload += "\"reset_reason\":\"" + String(getResetReasonString()) + "\",";
  payload += "\"uptime_sec\":" + String(millis() / 1000);
  payload += "}";

  mqttClient.publish(mqtt_topic_telemetry, payload.c_str());
  mqttClient.publish(mqtt_topic_state, ledState ? "ON" : "OFF");
  Serial.println("📤 MQTT Telemetria kiküldve: " + payload);
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    if (millis() - lastMqttReconnectAttempt > 5000) {
      lastMqttReconnectAttempt = millis();
      Serial.printf("🔌 Csatlakozás MQTT brokerhez (%s:%d)...\n", mqtt_server, mqtt_port);
      String clientId = "ESP32C3-P1Reader-" + String(random(0xffff), HEX);
      
      bool connected = false;
      if (strlen(mqtt_user) > 0) {
        connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass);
      } else {
        connected = mqttClient.connect(clientId.c_str());
      }

      if (connected) {
        Serial.println("✅ Sikeres csatlakozás az MQTT Brokerhez!");
        mqttClient.subscribe(mqtt_topic_cmd);
        publishMQTTTelemetry();
      } else {
        Serial.printf("❌ MQTT csatlakozási hiba (rc=%d)\n", mqttClient.state());
      }
    }
  }
}

void mqttCallback(char* topic, byte* message, unsigned int length) {
  String messageTemp;
  for (unsigned int i = 0; i < length; i++) {
    messageTemp += (char)message[i];
  }

  if (String(topic) == mqtt_topic_cmd) {
    if (messageTemp == "ON" || messageTemp == "1") {
      ledState = true;
      digitalWrite(LED_PIN, LOW);
    } else if (messageTemp == "OFF" || messageTemp == "0") {
      ledState = false;
      digitalWrite(LED_PIN, HIGH);
    }
    updateOLED();
    publishMQTTTelemetry();
  }
}

void handleScan() {
  Serial.println("📡 Wi-Fi hálózatok keresése...");
  WiFi.scanNetworks(true); // Async scan
  server.sendHeader("Location", "/settings");
  server.send(303);
}

void handleConfigSave() {
  if (server.hasArg("wifi_ssid")) strncpy(wifi_ssid, server.arg("wifi_ssid").c_str(), sizeof(wifi_ssid));
  if (server.hasArg("wifi_pass")) strncpy(wifi_pass, server.arg("wifi_pass").c_str(), sizeof(wifi_pass));
  if (server.hasArg("mqtt_server")) strncpy(mqtt_server, server.arg("mqtt_server").c_str(), sizeof(mqtt_server));
  if (server.hasArg("mqtt_port")) mqtt_port = server.arg("mqtt_port").toInt();
  if (server.hasArg("mqtt_user")) strncpy(mqtt_user, server.arg("mqtt_user").c_str(), sizeof(mqtt_user));
  if (server.hasArg("mqtt_pass")) strncpy(mqtt_pass, server.arg("mqtt_pass").c_str(), sizeof(mqtt_pass));

  saveSettings();

  Serial.printf("⚙️ Új Beállítások: SSID='%s' | MQTT='%s:%d'\n", wifi_ssid, mqtt_server, mqtt_port);

  if (strlen(wifi_ssid) > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(wifi_ssid, wifi_pass);
  }

  mqttClient.disconnect();
  mqttClient.setServer(mqtt_server, mqtt_port);

  updateOLED();

  server.sendHeader("Location", "/settings");
  server.send(303);
}

// Élő JSON API Végpont
void handleApiData() {
  float tempC = getChipTemperature();
  int freeRamKb = ESP.getFreeHeap() / 1024;
  int rssi = WiFi.RSSI();

  String json = "{";
  json += "\"temp_c\":" + String(tempC, 1) + ",";
  json += "\"free_ram_kb\":" + String(freeRamKb) + ",";
  json += "\"rssi_dbm\":" + String(rssi) + ",";
  json += "\"rssi_pct\":" + String(getRssiPercent(rssi)) + ",";
  json += "\"button_count\":" + String(buttonPressCount) + ",";
  json += "\"led_state\":\"" + String(ledState ? "BEKAPCSOLVA" : "KIKAPCSOLVA") + "\",";
  json += "\"p1_power_kw\":" + String(p1_current_power_kw, 3) + ",";
  json += "\"p1_total_kwh\":" + String(p1_total_energy_kwh, 1) + ",";
  json += "\"p1_export_kw\":" + String(p1_current_export_kw, 3) + ",";
  json += "\"p1_telegrams\":" + String(p1_telegram_count) + ",";
  json += "\"sta_ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Csatlakozás...") + "\",";
  json += "\"mqtt_status\":\"" + String(mqttClient.connected() ? "KAPCSOLÓDVA" : "Nem Elérhető") + "\",";
  json += "\"reset_reason\":\"" + String(getResetReasonString()) + "\",";
  json += "\"uptime_sec\":" + String(millis() / 1000);
  json += "}";

  server.send(200, "application/json", json);
}

String getHTMLHeader(const char* activeTab) {
  String html = "<!DOCTYPE html><html lang='hu'><head><meta charset='UTF-8'>";
  html += "<title>ESP32-C3 P1 Okosmérő</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial,sans-serif;text-align:center;margin:0;padding:20px;background:#121212;color:#fff;}";
  html += ".nav{display:flex;justify-content:center;gap:10px;margin-bottom:20px;}";
  html += ".nav a{padding:12px 20px;background:#2a2a2a;color:#fff;text-decoration:none;border-radius:8px;font-weight:bold;font-size:15px;}";
  html += ".nav a.active{background:#00e676;color:#000;}";
  html += ".card{background:#1e1e1e;border-radius:12px;padding:20px;margin:15px auto;max-width:440px;box-shadow:0 4px 10px rgba(0,0,0,0.5);}";
  html += "input,select{width:90%;padding:12px;margin:6px 0;border-radius:6px;border:none;background:#2a2a2a;color:#fff;font-size:16px;}";
  html += ".btn{padding:14px 28px;font-size:16px;background:#00e676;color:#000;border:none;border-radius:8px;cursor:pointer;text-decoration:none;display:inline-block;font-weight:bold;}";
  html += ".btn-scan{background:#29b6f6;color:#000;margin-bottom:10px;}";
  html += ".btn-off{background:#ff5252;color:#fff;}</style>";

  if (String(activeTab) == "data") {
    html += "<script>";
    html += "function updateData(){";
    html += "  fetch('/api/data').then(r=>r.json()).then(d=>{";
    html += "    document.getElementById('p1_power').innerText = d.p1_power_kw.toFixed(3) + ' kW';";
    html += "    document.getElementById('p1_total').innerText = d.p1_total_kwh.toFixed(1) + ' kWh';";
    html += "    document.getElementById('p1_export').innerText = d.p1_export_kw.toFixed(3) + ' kW';";
    html += "    document.getElementById('p1_telegrams').innerText = '#' + d.p1_telegrams;";
    html += "    document.getElementById('btn_count').innerText = d.button_count;";
    html += "    document.getElementById('led_status').innerText = d.led_state;";
    html += "  }).catch(e=>console.log(e));";
    html += "}";
    html += "setInterval(updateData, 2000);";
    html += "</script>";
  } else if (String(activeTab) == "status") {
    html += "<script>";
    html += "function updateStatus(){";
    html += "  fetch('/api/data').then(r=>r.json()).then(d=>{";
    html += "    document.getElementById('temp_c').innerText = d.temp_c + ' °C';";
    html += "    document.getElementById('free_ram').innerText = d.free_ram_kb + ' KB';";
    html += "    document.getElementById('wifi_rssi').innerText = d.rssi_dbm + ' dBm (' + d.rssi_pct + '%)';";
    html += "    document.getElementById('mqtt_status').innerText = d.mqtt_status;";
    html += "    document.getElementById('mqtt_status').style.color = (d.mqtt_status==='KAPCSOLÓDVA') ? '#00e676' : '#ff5252';";
    html += "    document.getElementById('uptime').innerText = d.uptime_sec + 's';";
    html += "  }).catch(e=>console.log(e));";
    html += "}";
    html += "setInterval(updateStatus, 2000);";
    html += "</script>";
  }

  html += "</head><body>";
  html += "<h1>🚀 ESP32-C3 P1 Okosmérő</h1>";

  // Navigation Tabs
  html += "<div class='nav'>";
  html += "<a href='/' class='" + String(String(activeTab) == "data" ? "active" : "") + "'>⚡ Adatok</a>";
  html += "<a href='/settings' class='" + String(String(activeTab) == "settings" ? "active" : "") + "'>⚙️ Beállítások</a>";
  html += "<a href='/status' class='" + String(String(activeTab) == "status" ? "active" : "") + "'>📊 Rendszer Státusz</a>";
  html += "</div>";

  return html;
}

// 1. FŐOLDAL: Kizárólag az Okosmérő élő adatai és vezérlése
void handleRoot() {
  String html = getHTMLHeader("data");

  html += "<div class='card'>";
  html += "<h2>⚡ P1 Okosmérő Adatok (Élő)</h2>";
  html += "<p><b>Aktuális Fogyasztás:</b> <span id='p1_power' style='font-size:28px;color:#00e676;font-weight:bold;'>" + String(p1_current_power_kw, 3) + " kW</span></p>";
  html += "<p><b>Összes Fogyasztás:</b> <span id='p1_total' style='font-size:20px;'>" + String(p1_total_energy_kwh, 1) + " kWh</span></p>";
  html += "<p><b>Visszatáplálás (Napelem):</b> <span id='p1_export' style='font-size:20px;'>" + String(p1_current_export_kw, 3) + " kW</span></p>";
  html += "<p><b>Fogadott Telegramok:</b> <span id='p1_telegrams'>#" + String(p1_telegram_count) + "</span></p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>💡 Vezérlés</h2>";
  html += "<p><b>Gomb megnyomva:</b> <span id='btn_count'>" + String(buttonPressCount) + "</span> alkalommal</p>";
  html += "<p><b>Fedélzeti LED:</b> <span id='led_status'>" + String(ledState ? "BEKAPCSOLVA" : "KIKAPCSOLVA") + "</span></p>";
  if (ledState) {
    html += "<p><a href='/led/off' class='btn btn-off'>LED KIKAPCSOLÁSA</a></p>";
  } else {
    html += "<p><a href='/led/on' class='btn'>LED BEKAPCSOLÁSA</a></p>";
  }
  html += "</div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// 2. BEÁLLÍTÁSOK OLDAL: Külön a Wi-Fi és MQTT konfiguráció
void handleSettingsPage() {
  String html = getHTMLHeader("settings");

  html += "<div class='card'>";
  html += "<h2>📶 Wi-Fi Hálózat Beállítása</h2>";
  html += "<p><a href='/scan' class='btn btn-scan'>🔍 Wi-Fi Hálózatok Keresése</a></p>";
  html += "<form method='POST' action='/config'>";

  int n = WiFi.scanComplete();
  if (n >= 0) {
    html += "<p style='text-align:left;margin-left:5%;'><b>Látható Wi-Fi Hálózatok (" + String(n) + " találat):</b></p>";
    html += "<select name='wifi_ssid' required>";
    html += "<option value=''>-- Válassz Hálózatot --</option>";
    for (int i = 0; i < n; ++i) {
      String networkSSID = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      int pct = getRssiPercent(rssi);
      String isSelected = (networkSSID == wifi_ssid) ? " selected" : "";
      html += "<option value='" + networkSSID + "'" + isSelected + ">" + networkSSID + " (" + String(pct) + "% / " + String(rssi) + " dBm)</option>";
    }
    html += "</select>";
  } else {
    html += "<p style='text-align:left;margin-left:5%;'><b>Wi-Fi SSID (Hálózat neve):</b></p>";
    html += "<input type='text' name='wifi_ssid' value='" + String(wifi_ssid) + "' placeholder='Írd be a Wi-Fi nevét' required>";
  }

  html += "<p style='text-align:left;margin-left:5%;margin-top:10px;'><b>Wi-Fi Jelszó:</b></p>";
  html += "<input type='password' name='wifi_pass' value='" + String(wifi_pass) + "' placeholder='Add meg a Wi-Fi jelszót'>";

  html += "<h2 style='margin-top:25px;'>🔌 MQTT Broker Beállítások</h2>";
  html += "<p style='text-align:left;margin-left:5%;'><b>MQTT Szerver IP:</b></p>";
  html += "<input type='text' name='mqtt_server' value='" + String(mqtt_server) + "' placeholder='pl. 192.168.0.253' required>";
  html += "<p style='text-align:left;margin-left:5%;'><b>Port:</b></p>";
  html += "<input type='text' name='mqtt_port' value='" + String(mqtt_port) + "'>";
  html += "<p style='text-align:left;margin-left:5%;'><b>MQTT Felhasználó (opcionális):</b></p>";
  html += "<input type='text' name='mqtt_user' value='" + String(mqtt_user) + "' placeholder='Felhasználónév'>";
  html += "<p style='text-align:left;margin-left:5%;'><b>MQTT Jelszó (opcionális):</b></p>";
  html += "<input type='password' name='mqtt_pass' value='" + String(mqtt_pass) + "' placeholder='Jelszó'>";

  html += "<p style='margin-top:20px;'><input type='submit' class='btn' value='Mentés & Csatlakozás'></p>";
  html += "</form>";
  html += "</div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// 3. RENDSZER STÁTUSZ OLDAL: Külön a technikai adatok és állapotok
void handleStatusPage() {
  float tempC = getChipTemperature();
  String html = getHTMLHeader("status");

  html += "<div class='card'>";
  html += "<h2>📊 Rendszer Státusz (Élő)</h2>";
  html += "<p><b>Otthoni Wi-Fi (STA):</b> " + (WiFi.status() == WL_CONNECTED ? "<span style='color:#00e676;'>" + String(wifi_ssid) + " (" + WiFi.localIP().toString() + ")</span>" : "<span style='color:#ff5252;'>Nincs kapcsolódva (" + String(wifi_ssid) + ")</span>") + "</p>";
  html += "<p><b>Saját Hotspot (AP):</b> " + WiFi.softAPIP().toString() + " (SSID: " + String(ap_ssid) + ")</p>";
  html += "<p><b>MQTT Broker Státusz:</b> <span id='mqtt_status' style='color:" + String(mqttClient.connected() ? "#00e676" : "#ff5252") + ";'>" + String(mqttClient.connected() ? "KAPCSOLÓDVA" : "Nem Elérhető") + "</span></p>";
  html += "<p><b>Belső Hőmérséklet:</b> <span id='temp_c'>" + String(tempC, 1) + " °C</span></p>";
  html += "<p><b>Szabad RAM:</b> <span id='free_ram'>" + String(ESP.getFreeHeap() / 1024) + " KB</span></p>";
  html += "<p><b>Wi-Fi Jelerősség:</b> <span id='wifi_rssi'>" + String(WiFi.RSSI()) + " dBm (" + String(getRssiPercent(WiFi.RSSI())) + "%)</span></p>";
  html += "<p><b>Reset Ok:</b> " + String(getResetReasonString()) + "</p>";
  html += "<p><b>Uptime:</b> <span id='uptime'>" + String(millis() / 1000) + "s</span></p>";
  html += "<p><b>MAC Cím:</b> " + WiFi.macAddress() + "</p>";
  html += "</div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleLedOn() {
  ledState = true;
  digitalWrite(LED_PIN, LOW);
  updateOLED();
  publishMQTTTelemetry();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleLedOff() {
  ledState = false;
  digitalWrite(LED_PIN, HIGH);
  updateOLED();
  publishMQTTTelemetry();
  server.sendHeader("Location", "/");
  server.send(303);
}

void triggerButtonPress(const char* source) {
  buttonPressCount++;
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState ? LOW : HIGH);
  Serial.printf("[%s] Gomb megnyomva! Összesen: %d | LED: %s\n", 
                source, buttonPressCount, ledState ? "BE" : "KI");
  updateOLED();
  publishMQTTTelemetry();
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(EXT_BUTTON_PIN, INPUT_PULLUP);

  pinMode(P1_RTS_PIN, OUTPUT);
  digitalWrite(P1_RTS_PIN, HIGH);

  Serial.begin(115200);

  loadSettings();

  Serial1.begin(115200, SERIAL_8N1, P1_RX_PIN, -1, true);
  Serial.println("⚡ P1 Port UART1 felállt a GPIO3 lábon!");

  initTempSensor();

  pinMode(OLED_SDA_PIN, INPUT_PULLUP);
  pinMode(OLED_SCL_PIN, INPUT_PULLUP);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("✅ OLED Kijelző elindult!");
    updateOLED();
  }

  WiFi.persistent(false);
  if (strlen(wifi_ssid) > 0) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_AP);
  }
  WiFi.setTxPower(WIFI_POWER_15dBm);

  WiFi.softAPConfig(ap_local_ip, ap_gateway, ap_subnet);
  bool apSuccess = WiFi.softAP(ap_ssid, ap_pass, 1, 0, 4);
  Serial.printf("📡 Saját Wi-Fi Hotspot: %s | IP: %s\n", 
                apSuccess ? "Sikeres" : "Hiba", WiFi.softAPIP().toString().c_str());

  if (strlen(wifi_ssid) > 0) {
    Serial.printf("📡 Kapcsolódási kísérlet a mentett Wi-Fi-re: '%s'...\n", wifi_ssid);
    WiFi.begin(wifi_ssid, wifi_pass);
  }

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  server.on("/", handleRoot);                  // 1. Főoldal: Kizárólag az élő adatok
  server.on("/settings", handleSettingsPage);  // 2. Beállítások oldal
  server.on("/status", handleStatusPage);      // 3. Rendszer Státusz oldal
  server.on("/api/data", handleApiData);
  server.on("/scan", handleScan);
  server.on("/config", HTTP_POST, handleConfigSave);
  server.on("/led/on", handleLedOn);
  server.on("/led/off", handleLedOff);
  server.begin();

  updateOLED();
}

void loop() {
  server.handleClient();
  readP1Serial();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    } else {
      mqttClient.loop();
    }
  }

  // MQTT Adatküldés 5 másodpercenként
  if (millis() - lastMqttPublish > 5000) {
    lastMqttPublish = millis();
    if (WiFi.status() == WL_CONNECTED) {
      publishMQTTTelemetry();
    }
  }

  // 1. Fedélzeti BOOT gomb (GPIO9)
  bool bootState = digitalRead(BOOT_BUTTON_PIN);
  if (bootState != lastBootState) {
    if ((millis() - lastDebounceTime) > 50) {
      if (bootState == LOW) {
        triggerButtonPress("BOOT GOMB");
      }
      lastDebounceTime = millis();
    }
  }
  lastBootState = bootState;

  // 2. Külső gomb (GPIO4)
  bool extState = digitalRead(EXT_BUTTON_PIN);
  if (extState != lastExtState) {
    if ((millis() - lastDebounceTime) > 50) {
      if (extState == LOW) {
        triggerButtonPress("KÜLSŐ GOMB");
      }
      lastDebounceTime = millis();
    }
  }
  lastExtState = extState;

  // OLED frissítés 1mp-enként
  static unsigned long lastOledRefresh = 0;
  if (millis() - lastOledRefresh > 1000) {
    lastOledRefresh = millis();
    updateOLED();
  }
}
