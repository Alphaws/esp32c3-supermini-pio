#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <esp_wifi.h>

#define OLED_SDA_PIN     5
#define OLED_SCL_PIN     6
#define BOOT_BUTTON_PIN  9
#define EXT_BUTTON_PIN   4
#define LED_PIN          8

#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WebServer server(80);

const char* ssid = "aws01-24";
const char* password = "1qaw3ed-";

bool ledState = false;
int buttonPressCount = 0;
bool lastBootState = HIGH;
bool lastExtState = HIGH;
unsigned long lastDebounceTime = 0;

int getRssiPercent(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

void updateOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // 1. Fejléc
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("ESP32-C3 SuperMini");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int percent = getRssiPercent(rssi);

    // 2. IP Cím kiemelve
    display.setCursor(0, 13);
    display.print("IP: ");
    display.println(WiFi.localIP());

    // 3. Wi-Fi Jelerősség (dBm + %)
    display.setCursor(0, 25);
    display.printf("WiFi: %d dBm (%d%%)\n", rssi, percent);

    // 4. Gomb & LED állapot
    display.setCursor(0, 37);
    display.printf("Gomb: %d  |  LED: %s\n", buttonPressCount, ledState ? "BE" : "KI");

    // 5. SSID Név
    display.setCursor(0, 49);
    display.print("SSID: ");
    display.println(WiFi.SSID());
  } else {
    display.setCursor(0, 20);
    display.setTextSize(1);
    display.println("Stabilizalt Connect");
    display.setCursor(0, 34);
    display.print("SSID: ");
    display.println(ssid);
    display.setCursor(0, 46);
    display.printf("Statu: %d\n", WiFi.status());
  }

  // Alsó Uptime sáv
  display.setCursor(0, 57);
  display.printf("Uptime: %lu s\n", millis() / 1000);

  display.display();
}

void handleRoot() {
  String html = "<html><head><title>ESP32-C3 SuperMini Web Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;text-align:center;margin-top:40px;background:#121212;color:#fff;}";
  html += ".btn{padding:15px 30px;font-size:18px;background:#00e676;color:#000;border:none;border-radius:8px;cursor:pointer;text-decoration:none;}";
  html += ".btn-off{background:#ff5252;color:#fff;}</style></head><body>";
  html += "<h1>🚀 ESP32-C3 SuperMini Dashboard</h1>";
  html += "<p><b>Kapcsol&oacute;dva:</b> " + WiFi.SSID() + "</p>";
  html += "<p><b>IP c&iacute;m:</b> " + WiFi.localIP().toString() + "</p>";
  html += "<p><b>Jeler&odblac;ss&eacute;g:</b> " + String(WiFi.RSSI()) + " dBm (" + String(getRssiPercent(WiFi.RSSI())) + "%)</p>";
  html += "<p><b>Gomb megnyomva:</b> " + String(buttonPressCount) + " alkalommal</p>";
  html += "<p><b>Fed&eacute;lzeti LED:</b> " + String(ledState ? "BEKAPCSOLVA" : "KIKAPCSOLVA") + "</p>";
  if (ledState) {
    html += "<p><a href='/led/off' class='btn btn-off'>LED KIKAPCSOL&Aacute;SA</a></p>";
  } else {
    html += "<p><a href='/led/on' class='btn'>LED BEKAPCSOL&Aacute;SA</a></p>";
  }
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleLedOn() {
  ledState = true;
  digitalWrite(LED_PIN, LOW);
  updateOLED();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleLedOff() {
  ledState = false;
  digitalWrite(LED_PIN, HIGH);
  updateOLED();
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
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(EXT_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);

  pinMode(OLED_SDA_PIN, INPUT_PULLUP);
  pinMode(OLED_SCL_PIN, INPUT_PULLUP);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("✅ OLED Kijelző elindult!");
    updateOLED();
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_13dBm); // Teljesítmény tüske elhárítása USB tápon
  WiFi.begin(ssid, password);

  server.on("/", handleRoot);
  server.on("/led/on", handleLedOn);
  server.on("/led/off", handleLedOff);
  server.begin();

  updateOLED();
}

void loop() {
  server.handleClient();

  static bool lastWifiConnected = false;
  bool currentWifiConnected = (WiFi.status() == WL_CONNECTED);
  if (currentWifiConnected != lastWifiConnected) {
    if (currentWifiConnected) {
      Serial.println("🎉 SIKERES STABILIZÁLT CSATLAKOZÁS!");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    }
    lastWifiConnected = currentWifiConnected;
    updateOLED();
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
