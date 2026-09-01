#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <driver/temp_sensor.h>
#include <esp_system.h>

#define OLED_SDA_PIN     5
#define OLED_SCL_PIN     6
#define BOOT_BUTTON_PIN  9
#define EXT_BUTTON_PIN   4
#define LED_PIN          8

#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WebServer server(80);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

const char* ssid = "aws01-24";
const char* password = "1qaw3ed-";

// MQTT beállítások
char mqtt_server[64] = "192.168.0.140";
int mqtt_port = 1883;
const char* mqtt_topic_telemetry = "esp32c3/supermini/telemetry";
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

int getRssiPercent(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
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

  // 1. Fejléc
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("ESP32-C3 Telemetria");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    float tempC = getChipTemperature();

    // 2. IP Cím
    display.setCursor(0, 13);
    display.print("IP: ");
    display.println(WiFi.localIP());

    // 3. Chip Hőmérséklet & Szabad RAM
    display.setCursor(0, 25);
    display.printf("Temp: %.1fC | RAM:%dKB\n", tempC, ESP.getFreeHeap() / 1024);

    // 4. WiFi Jelerősség & MQTT Státusz
    display.setCursor(0, 37);
    display.printf("WiFi: %d%% | MQTT:%s\n", getRssiPercent(rssi), mqttClient.connected() ? "OK" : "KI");

    // 5. Gomb & LED állapot
    display.setCursor(0, 49);
    display.printf("Gomb: %d  |  LED: %s\n", buttonPressCount, ledState ? "BE" : "KI");
  } else {
    display.setCursor(0, 20);
    display.setTextSize(1);
    display.println("WiFi kapcsolodas...");
    display.setCursor(0, 34);
    display.print("SSID: ");
    display.println(ssid);
  }

  // Alsó Uptime sáv
  display.setCursor(0, 57);
  display.printf("Up: %lus | Reset: %s\n", millis() / 1000, getResetReasonString());

  display.display();
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
  payload += "\"ip_address\":\"" + WiFi.localIP().toString() + "\",";
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
    if (millis() - lastMqttReconnectAttempt > 10000) { // 10mp-enként próbálkozik háttérben
      lastMqttReconnectAttempt = millis();
      Serial.printf("🔌 Csatlakozási kísérlet MQTT brokerhez (%s:%d)...\n", mqtt_server, mqtt_port);
      String clientId = "ESP32C3-SuperMini-" + String(random(0xffff), HEX);
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println("✅ Sikeres MQTT csatlakozás!");
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
  Serial.printf("📩 MQTT Üzenet érkezett [%s]: %s\n", topic, messageTemp.c_str());

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

void handleRoot() {
  float tempC = getChipTemperature();
  String html = "<html><head><title>ESP32-C3 SuperMini Telemetry Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;text-align:center;margin-top:30px;background:#121212;color:#fff;}";
  html += ".card{background:#1e1e1e;border-radius:12px;padding:20px;margin:15px auto;max-width:400px;box-shadow:0 4px 10px rgba(0,0,0,0.5);}";
  html += ".btn{padding:15px 30px;font-size:18px;background:#00e676;color:#000;border:none;border-radius:8px;cursor:pointer;text-decoration:none;display:inline-block;}";
  html += ".btn-off{background:#ff5252;color:#fff;}</style></head><body>";
  html += "<h1>🚀 ESP32-C3 Telemetry Dashboard</h1>";

  html += "<div class='card'>";
  html += "<h2>📊 Rendszer Adatok</h2>";
  html += "<p><b>Belső Hőmérséklet:</b> " + String(tempC, 1) + " &deg;C</p>";
  html += "<p><b>Szabad RAM:</b> " + String(ESP.getFreeHeap() / 1024) + " KB / " + String(ESP.getMinFreeHeap() / 1024) + " KB (min)</p>";
  html += "<p><b>Wi-Fi Jelerősség:</b> " + String(WiFi.RSSI()) + " dBm (" + String(getRssiPercent(WiFi.RSSI())) + "%)</p>";
  html += "<p><b>Reset Ok:</b> " + String(getResetReasonString()) + "</p>";
  html += "<p><b>MAC Cím:</b> " + WiFi.macAddress() + "</p>";
  html += "<p><b>MQTT Broker:</b> " + String(mqtt_server) + ":" + String(mqtt_port) + " (" + (mqttClient.connected() ? "KAPCSOLÓDVA" : "LECSATLAKOZVA") + ")</p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>💡 Vezérlés</h2>";
  html += "<p><b>Gomb megnyomva:</b> " + String(buttonPressCount) + " alkalommal</p>";
  html += "<p><b>Fedélzeti LED:</b> " + String(ledState ? "BEKAPCSOLVA" : "KIKAPCSOLVA") + "</p>";
  if (ledState) {
    html += "<p><a href='/led/off' class='btn btn-off'>LED KIKAPCSOLÁSA</a></p>";
  } else {
    html += "<p><a href='/led/on' class='btn'>LED BEKAPCSOLÁSA</a></p>";
  }
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

  Serial.begin(115200);

  initTempSensor();

  pinMode(OLED_SDA_PIN, INPUT_PULLUP);
  pinMode(OLED_SCL_PIN, INPUT_PULLUP);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("✅ OLED Kijelző elindult!");
    updateOLED();
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_13dBm);
  WiFi.begin(ssid, password);

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  server.on("/", handleRoot);
  server.on("/led/on", handleLedOn);
  server.on("/led/off", handleLedOff);
  server.begin();

  updateOLED();
}

void loop() {
  server.handleClient();

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
