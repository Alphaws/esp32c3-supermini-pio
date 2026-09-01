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

// P1 Okosmérő (DSMR) Csatlakozás
#define P1_RX_PIN        3  // RJ12 Pin 5 (Inverted Data) -> ESP32-C3 GPIO3
#define P1_RTS_PIN       7  // RJ12 Pin 2 (Data Request)  -> ESP32-C3 GPIO7 (HIGH)

#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WebServer server(80);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

const char* ssid = "aws01-24";
const char* password = "1qaw3ed-";

// Raspberry Pi 24/7 Helyi MQTT Broker Beállítások
char mqtt_server[64] = "192.168.0.253"; // Raspberry Pi 24/7 MQTT Server
int mqtt_port = 1883;
char mqtt_user[32] = "";     // Felhasználónév (ha van)
char mqtt_pass[32] = "";     // Jelszó (ha van)

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
float p1_current_power_kw = 0.0;    // 1-0:1.7.0 (Aktuális fogyasztás kW)
float p1_total_energy_kwh = 0.0;    // 1-0:1.8.0 / 1.8.1 (Összes fogyasztás kWh)
float p1_current_export_kw = 0.0;   // 1-0:2.7.0 (Aktuális betáplálás kW)
int p1_telegram_count = 0;
String p1_raw_buffer = "";

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
  display.println("ESP32-C3 P1 Meter");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    float tempC = getChipTemperature();

    // 2. IP Cím & P1 Telegram számláló
    display.setCursor(0, 13);
    display.printf("IP:%s P1:#%d\n", WiFi.localIP().toString().c_str(), p1_telegram_count);

    // 3. P1 Aktuális Teljesítmény (kW / W)
    display.setCursor(0, 25);
    display.printf("P1 Pwr: %.3f kW\n", p1_current_power_kw);

    // 4. P1 Összes Fogyasztás (kWh)
    display.setCursor(0, 37);
    display.printf("P1 Sum: %.1f kWh\n", p1_total_energy_kwh);

    // 5. Hőmérséklet & Wi-Fi
    display.setCursor(0, 49);
    display.printf("Temp:%.1fC | RPi:%s\n", tempC, mqttClient.connected() ? "OK" : "AuthHiba");
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
  display.printf("Up: %lus | Broker: .253\n", millis() / 1000);

  display.display();
}

void parseP1Line(String line) {
  // 1-0:1.7.0(00.450*kW) -> Aktuális fogyasztás
  if (line.indexOf("1-0:1.7.0") >= 0) {
    int start = line.indexOf('(');
    int end = line.indexOf("*kW");
    if (start >= 0 && end > start) {
      p1_current_power_kw = line.substring(start + 1, end).toFloat();
    }
  }
  // 1-0:1.8.1(001234.567*kWh) vagy 1-0:1.8.0 -> Összes fogyasztás
  else if (line.indexOf("1-0:1.8.1") >= 0 || line.indexOf("1-0:1.8.0") >= 0) {
    int start = line.indexOf('(');
    int end = line.indexOf("*kWh");
    if (start >= 0 && end > start) {
      p1_total_energy_kwh = line.substring(start + 1, end).toFloat();
    }
  }
  // 1-0:2.7.0(00.000*kW) -> Aktuális napelem/visszatáplálás
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
  payload += "\"mac_address\":\"" + WiFi.macAddress() + "\",";
  payload += "\"reset_reason\":\"" + String(getResetReasonString()) + "\",";
  payload += "\"uptime_sec\":" + String(millis() / 1000);
  payload += "}";

  mqttClient.publish(mqtt_topic_telemetry, payload.c_str());
  mqttClient.publish(mqtt_topic_state, ledState ? "ON" : "OFF");
  Serial.println("📤 Raspberry Pi MQTT Telemetria + P1 adatok kiküldve: " + payload);
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    if (millis() - lastMqttReconnectAttempt > 5000) {
      lastMqttReconnectAttempt = millis();
      Serial.printf("🔌 Csatlakozási kísérlet Raspberry Pi MQTT brokerhez (%s:%d)...\n", mqtt_server, mqtt_port);
      String clientId = "ESP32C3-P1Reader-" + String(random(0xffff), HEX);
      
      bool connected = false;
      if (strlen(mqtt_user) > 0) {
        connected = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass);
      } else {
        connected = mqttClient.connect(clientId.c_str());
      }

      if (connected) {
        Serial.println("✅ Sikeres csatlakozás a Raspberry Pi 24/7 MQTT Brokerhez!");
        mqttClient.subscribe(mqtt_topic_cmd);
        publishMQTTTelemetry();
      } else {
        Serial.printf("❌ Raspberry Pi MQTT csatlakozási hiba (rc=%d)\n", mqttClient.state());
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

void handleConfigSave() {
  if (server.hasArg("mqtt_server")) strncpy(mqtt_server, server.arg("mqtt_server").c_str(), sizeof(mqtt_server));
  if (server.hasArg("mqtt_port")) mqtt_port = server.arg("mqtt_port").toInt();
  if (server.hasArg("mqtt_user")) strncpy(mqtt_user, server.arg("mqtt_user").c_str(), sizeof(mqtt_user));
  if (server.hasArg("mqtt_pass")) strncpy(mqtt_pass, server.arg("mqtt_pass").c_str(), sizeof(mqtt_pass));

  mqttClient.disconnect();
  mqttClient.setServer(mqtt_server, mqtt_port);
  reconnectMQTT();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  float tempC = getChipTemperature();
  String html = "<html><head><title>ESP32-C3 P1 Smart Meter Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;text-align:center;margin-top:30px;background:#121212;color:#fff;}";
  html += ".card{background:#1e1e1e;border-radius:12px;padding:20px;margin:15px auto;max-width:420px;box-shadow:0 4px 10px rgba(0,0,0,0.5);}";
  html += "input{width:80%;padding:10px;margin:5px;border-radius:6px;border:none;}";
  html += ".btn{padding:15px 30px;font-size:18px;background:#00e676;color:#000;border:none;border-radius:8px;cursor:pointer;text-decoration:none;display:inline-block;}";
  html += ".btn-off{background:#ff5252;color:#fff;}</style></head><body>";
  html += "<h1>⚡ ESP32-C3 P1 Okosm&eacute;r&odblac; Dashboard</h1>";

  html += "<div class='card'>";
  html += "<h2>⚡ P1 Okosm&eacute;r&odblac; Adatok</h2>";
  html += "<p><b>Aktu&aacute;lis Fogyaszt&aacute;s:</b> <span style='font-size:24px;color:#00e676;'>" + String(p1_current_power_kw, 3) + " kW</span></p>";
  html += "<p><b>&Ouml;sszes Fogyaszt&aacute;s:</b> " + String(p1_total_energy_kwh, 1) + " kWh</p>";
  html += "<p><b>Visszat&aacute;pl&aacute;l&aacute;s (Napelem):</b> " + String(p1_current_export_kw, 3) + " kW</p>";
  html += "<p><b>Fogadott Telegramok:</b> #" + String(p1_telegram_count) + "</p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>⚙️ MQTT Broker Be&aacute;ll&iacute;t&aacute;sok</h2>";
  html += "<form method='POST' action='/config'>";
  html += "<p><b>Szerver IP:</b><br><input type='text' name='mqtt_server' value='" + String(mqtt_server) + "'></p>";
  html += "<p><b>Port:</b><br><input type='text' name='mqtt_port' value='" + String(mqtt_port) + "'></p>";
  html += "<p><b>Felha&scedil;n&aacute;l&oacute;n&eacute;v (ha van):</b><br><input type='text' name='mqtt_user' value='" + String(mqtt_user) + "'></p>";
  html += "<p><b>Jelsz&oacute; (ha van):</b><br><input type='password' name='mqtt_pass' value='" + String(mqtt_pass) + "'></p>";
  html += "<p><input type='submit' class='btn' value='Ment&eacute;s & Csatlakoz&aacute;s'></p>";
  html += "</form>";
  html += "<p><b>St&aacute;tusz:</b> " + String(mqtt_server) + ":" + String(mqtt_port) + " (" + (mqttClient.connected() ? "<span style='color:#00e676;'>KAPCSOL&Oacute;DVA</span>" : "<span style='color:#ff5252;'>Auth / Hiteles&iacute;t&eacute;s Hiba</span>") + ")</p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>📊 Rendszer Adatok</h2>";
  html += "<p><b>Belső Hőm&eacute;rs&eacute;klet:</b> " + String(tempC, 1) + " &deg;C</p>";
  html += "<p><b>Szabad RAM:</b> " + String(ESP.getFreeHeap() / 1024) + " KB</p>";
  html += "<p><b>Wi-Fi Jeler&odblac;ss&eacute;g:</b> " + String(WiFi.RSSI()) + " dBm (" + String(getRssiPercent(WiFi.RSSI())) + "%)</p>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h2>💡 Vez&eacute;rl&eacute;s</h2>";
  html += "<p><b>Gomb megnyomva:</b> " + String(buttonPressCount) + " alkalommal</p>";
  html += "<p><b>Fed&eacute;lzeti LED:</b> " + String(ledState ? "BEKAPCSOLVA" : "KIKAPCSOLVA") + "</p>";
  if (ledState) {
    html += "<p><a href='/led/off' class='btn btn-off'>LED KIKAPCSOL&Aacute;SA</a></p>";
  } else {
    html += "<p><a href='/led/on' class='btn'>LED BEKAPCSOL&Aacute;SA</a></p>";
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

  pinMode(P1_RTS_PIN, OUTPUT);
  digitalWrite(P1_RTS_PIN, HIGH);

  Serial.begin(115200);

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
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_13dBm);
  WiFi.begin(ssid, password);

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  server.on("/", handleRoot);
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
