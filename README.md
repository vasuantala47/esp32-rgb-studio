# 🌈 ESP32-S3 AI RGB Pro Studio

An advanced, multi-interface RGB lighting studio and audio visualizer engineered for the **ESP32-S3 DevKitM-1** (and compatible ESP32 boards). Features **20 built-in lighting moods**, **in-browser real-time DSP audio beat detection**, **Web Bluetooth Low Energy (BLE)**, **Wi-Fi web server**, and a high-speed **USB Serial Bridge** for hostel/enterprise network environments.

---

## ✨ Features

- **🎭 20 Built-In Lighting Modes & Presets**:
  - Aurora Borealis, Cyberpunk Pulse, Campfire Ember, Rainbow 360 Wave, Emergency Strobe, Breathing White, Traffic Light, Auto-Cycle, Lightning Storm, Party Rave Beat, Disco Beat Reactive, Thriller Suspense, Zen Ambient, Ocean Waves, Volcano Magma, Enchanted Forest, Candlelight, Neon Tokyo, Glacier Ice Frost, and Matrix Cyber Rain.
- **🎵 Real-Time Browser DSP Audio Visualizer**:
  - Web Audio API Fourier transform (`AnalyserNode`) with sub-bass, mid, and treble frequency separation.
  - Multi-input audio support:
    - **Microphone / Ambient Listening**: Low-latency room beat pickup.
    - **System / Tab Audio Sharing**: Spotify, YouTube, SoundCloud directly synced with beat-matching lighting.
    - **In-Browser Audio File Player**: Load MP3/WAV files directly with live visual frequency spectrum.
- **🔵 Web Bluetooth Low Energy (BLE)**:
  - Direct wireless control from Chrome/Edge via Nordic Semiconductor UART Service UUID.
  - No local Wi-Fi router needed!
- **🌐 Dual-Mode Wi-Fi Architecture**:
  - **SoftAP Mode**: Built-in access point (`ESP32-RGB-Studio`).
  - **Station Mode**: Connects to home or hostel Wi-Fi network.
  - **mDNS Support**: Accessible at `http://esp32-rgb.local`.
- **🔌 Laptop USB Serial Bridge (`serve.py`)**:
  - Solves hostel/campus captive-portal and isolation problems.
  - Keeps laptop connected to regular internet while streaming real-time beat packages over USB CDC Serial at 115200 baud with sub-5ms latency.
- **❄️ Thermal & Low-Power Engineering**:
  - Dynamic CPU scaling (160 MHz) reducing heat by ~40% vs 240 MHz.
  - RF transmit power trimmed to 13 dBm with 802.11 modem sleep enabled.
  - Automatic SoftAP shutdown when station connects to eliminate dual-radio heating.
  - FreeRTOS tick yields (250 Hz loop) allowing idle task sleep.

---

- **🚥 External 8-LED RGB Bar (Keyes ARTOU V2 Support)**:
  - Supports the 8x 5050 RGB multiplexed module with onboard S8550 PNP transistors.
  - Ultra-smooth 125 FPS FreeRTOS background multiplexing task on Core 1 (zero flicker, zero CPU slowdown).
  - 6 dedicated display modes: Mirror Main WS2812, 8-LED Audio VU Meter with falling peak dots, Frequency Spectrum Flow, Traveling Rainbow Wave, Cyber Scanner (Knight Rider), and Campfire Embers.
  - Interactive 8-LED virtual preview HUD in Web Studio.

---

## 🛠️ Hardware Requirements & Wiring

### 1. ESP32-S3 Board
- **Microcontroller**: ESP32-S3 DevKitM-1 (or standard ESP32 / S2 / C3)
- **Onboard WS2812 RGB Pin**: GPIO 48
- **USB Connection**: USB Type-C Cable

### 2. External 8-LED RGB Bar (Keyes ARTOU LED RGB V2)
> [!CAUTION]
> **CRITICAL VOLTAGE RULE: CONNECT `VCC` TO 3.3V (NOT 5V)!**
> The module uses PNP transistors with emitters tied to `VCC`. If connected to 5V, the ESP32's 3.3V logic will never turn off the transistors. Connecting `VCC` to the ESP32 **3.3V pin** guarantees complete shutoff (0V $V_{be}$) when GPIO is HIGH and full turn-on when LOW. **No external GND wire is needed** because color cathodes sink directly through ESP32 GPIOs to ground!

| Module Pin | ESP32-S3 Pin | Function / Type |
|---|---|---|
| **VCC** | **3.3V Pin** | Power Supply (MUST be 3.3V!) |
| **R** | **GPIO 4** | Red Cathode (20 kHz PWM Channel 0, Active LOW) |
| **G** | **GPIO 5** | Green Cathode (20 kHz PWM Channel 1, Active LOW) |
| **B** | **GPIO 6** | Blue Cathode (20 kHz PWM Channel 2, Active LOW) |
| **D0** | **GPIO 7** | Digit 0 Anode PNP (Active LOW) |
| **D1** | **GPIO 8** | Digit 1 Anode PNP (Active LOW) |
| **D2** | **GPIO 9** | Digit 2 Anode PNP (Active LOW) |
| **D3** | **GPIO 10** | Digit 3 Anode PNP (Active LOW) |
| **D4** | **GPIO 11** | Digit 4 Anode PNP (Active LOW) |
| **D5** | **GPIO 12** | Digit 5 Anode PNP (Active LOW) |
| **D6** | **GPIO 13** | Digit 6 Anode PNP (Active LOW) |
| **D7** | **GPIO 14** | Digit 7 Anode PNP (Active LOW) |

---

## 🚀 Quick Start Guide

### 1. Flash Firmware (PlatformIO)
Prerequisites: [PlatformIO](https://platformio.org/) installed in VS Code or CLI.

```bash
# Clone the repository
git clone https://github.com/vasuantala47/esp32-rgb-studio.git
cd esp32-rgb-studio

# Build and flash to ESP32
pio run -t upload
```

### 2. Control Interfaces

#### Option A: Localhost USB Bridge (Recommended for Hostels / Captive Portals)
Double-click `run_local_studio.bat` or run:
```bash
python serve.py
```
Open your browser to:
👉 **`http://localhost:8000`**

Your laptop stays connected to your normal Wi-Fi / Internet, and all music sync & controls stream directly over USB Serial to the ESP32!

#### Option B: Web Bluetooth (Chrome / Edge)
1. Open `http://localhost:8000` or the hosted studio page in Google Chrome.
2. Click **Connect Bluetooth**.
3. Select **ESP32-RGB-Studio** from the pairing window.

#### Option C: Wi-Fi Web Server
- Connect to Wi-Fi SSID `ESP32-RGB-Studio` (Password: `12345678`).
- Navigate to `http://192.168.4.1` or `http://esp32-rgb.local`.

---

## 📡 Serial Protocol & REST API

### USB / BLE ASCII Commands
| Command | Action |
|---|---|
| `!M:<mode>` | Set active lighting mode (0 = OFF, 1..20 = Modes, -1 = Solid Color, -2 = Custom Sequence) |
| `!RGB:<r>,<g>,<b>` | Set solid lamp color (0-255 each) |
| `!BR:<0-100>` | Adjust master brightness percentage |
| `!B:<r>,<g>,<b>,<intensity>,<decayMs>` | Send audio beat pulse trigger |
| `!SEQ:<r,g,b,dur,fade;...>` | Set up to 32 custom sequence steps |

### HTTP REST Endpoints
- `GET /api/mode?val=<1..20>`
- `GET /api/color?r=<0-255>&g=<0-255>&b=<0-255>`
- `GET /api/brightness?val=<0-100>`
- `GET /api/beat?r=<r>&g=<g>&b=<b>&i=<intensity>&d=<decayMs>`
- `GET /api/wifi?ssid=<ssid>&pass=<password>`

---

## 📄 License
MIT License. Free to use, modify, and distribute.
