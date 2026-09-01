# ESP32-C3 SuperMini OLED Display & Web Dashboard

C++/PlatformIO project for the ESP32-C3 SuperMini hardware module. Features real-time SSD1306 0.96" I2C OLED monitoring, Wi-Fi station connectivity, dual push button debouncing, onboard LED control, and a responsive web dashboard.

## Pinout Mapping

| Peripheral | ESP32-C3 SuperMini Pin | Function |
| :--- | :--- | :--- |
| **OLED SDA** | `GPIO5` | I2C Data Line (0.96" SSD1306) |
| **OLED SCL** | `GPIO6` | I2C Clock Line (Address 0x3C) |
| **External Button** | `GPIO4` | Active LOW (Pulls to GND) |
| **Onboard BOOT Button** | `GPIO9` | Active LOW (Pulls to GND) |
| **Onboard LED** | `GPIO8` | Active LOW LED Indicator |

## Key Implementation Highlights

- **SSD1306 0.96" OLED Display:** Renders IP address, Wi-Fi RSSI (dBm + %), button press count, LED state, SSID, and uptime.
- **RF Power Stabilization:** Set to `WiFi.setTxPower(WIFI_POWER_13dBm)` to prevent voltage drops on USB 3.3V CDC power line.
- **Embedded Web Server:** Hosted on port 80 (`http://<IP>/`) for remote LED control and system telemetry.
- **Dual Button Debouncing:** Software debouncing for both onboard `GPIO9` and external `GPIO4` buttons.
