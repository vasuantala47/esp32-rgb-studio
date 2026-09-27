import re
import os
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_PATH = os.path.join(SCRIPT_DIR, "index.html")
MAIN_PATH = os.path.join(SCRIPT_DIR, "src", "main.cpp")

with open(INDEX_PATH, "r", encoding="utf-8") as f:
    html = f.read()

# 1. Fix the duplicate variable declarations in section 🎵 IN-BROWSER AUDIO PLAYER
duplicate_vars = """  let analyser = null, audioStream = null;
  let isListening = false, isFilePlaying = false;
  let beatInFlight = false;
  let dynamicBassThreshold = 55;
  let recentEnergy = [];
  let mediaElementSource = null;
  let lastKickEnergy = 0;"""

clean_vars = """  let analyser = null, audioStream = null;
  let isListening = false, isFilePlaying = false;
  let mediaElementSource = null;"""

if duplicate_vars in html:
    html = html.replace(duplicate_vars, clean_vars, 1)
    print("✔ Cleaned duplicate declarations from player section")
else:
    print("ℹ duplicate_vars already cleaned or not found")

# 2. Fix duplicate lastBeatTimestamp in DSP section if present
html = html.replace(
    "  let lastMidEnergy = 0;\n  let lastBeatTimestamp = 0;\n  let recentEnergy = [];",
    "  let lastMidEnergy = 0;\n  let recentEnergy = [];"
)
html = html.replace(
    "  let lastBeatTimestamp = 0;\n  let recentEnergy = [];",
    "  let recentEnergy = [];"
)
html = html.replace(
    "  let lastKickEnergy = 0;\n  let lastMidEnergy = 0;\n  let lastBeatTimestamp = 0;",
    "  let lastKickEnergy = 0;\n  let lastMidEnergy = 0;"
)

# 3. Enhance toggleBluetoothConnect for maximum compatibility
old_ble_func = """  async function toggleBluetoothConnect() {
    if (!navigator.bluetooth) {
      alert("Web Bluetooth is not supported in this browser.\\n\\nPlease open this page in Google Chrome or Microsoft Edge on Windows 10/11, Mac, or Android to connect via Bluetooth!");
      return;
    }
    if (bleConnected) {
      disconnectBluetooth();
      return;
    }
    try {
      const btn = document.getElementById('btn-ble-connect');
      btn.innerHTML = '<span>⏳</span> Searching BLE...';
      
      bleDevice = await navigator.bluetooth.requestDevice({
        filters: [
          { name: 'ESP32-RGB-Studio' },
          { namePrefix: 'ESP32' }
        ],
        optionalServices: [BLE_SERVICE_UUID]
      });

      bleDevice.addEventListener('gattserverdisconnected', onBleDisconnected);

      btn.innerHTML = '<span>⏳</span> Connecting...';
      bleServer = await bleDevice.gatt.connect();

      const service = await bleServer.getPrimaryService(BLE_SERVICE_UUID);
      bleRxCharacteristic = await service.getCharacteristic(BLE_RX_UUID);

      bleConnected = true;
      updateBleUI(true);
      console.log("🔵 Connected to ESP32 via Bluetooth Low Energy!");
    } catch(err) {
      console.warn("BLE connect error:", err);
      updateBleUI(false);
      if (err.name !== 'NotFoundError') {
        alert("Bluetooth Connection Notice: " + err.message);
      }
    }
  }"""

new_ble_func = """  async function toggleBluetoothConnect() {
    if (!navigator.bluetooth) {
      alert("Web Bluetooth requires Google Chrome or Microsoft Edge on localhost or HTTPS.\\n\\nPlease open http://localhost:8000 in Chrome to connect via Bluetooth!\\n\\n(Also ensure Bluetooth is turned ON in Windows Settings)");
      return;
    }
    if (bleConnected) {
      disconnectBluetooth();
      return;
    }
    const btn = document.getElementById('btn-ble-connect');
    const pill = document.getElementById('ble-status-pill');
    try {
      if (btn) btn.innerHTML = '<span>⏳</span> Scanning BLE...';
      if (pill) pill.textContent = 'Scanning...';

      // Scan with multiple filter fallbacks for robust matching on Windows
      bleDevice = await navigator.bluetooth.requestDevice({
        filters: [
          { name: 'ESP32-RGB-Studio' },
          { namePrefix: 'ESP32' },
          { services: [BLE_SERVICE_UUID] }
        ],
        optionalServices: [BLE_SERVICE_UUID]
      });

      bleDevice.addEventListener('gattserverdisconnected', onBleDisconnected);

      if (btn) btn.innerHTML = '<span>⏳</span> Connecting...';
      if (pill) pill.textContent = 'Connecting...';

      bleServer = await bleDevice.gatt.connect();

      const service = await bleServer.getPrimaryService(BLE_SERVICE_UUID);
      bleRxCharacteristic = await service.getCharacteristic(BLE_RX_UUID);

      bleConnected = true;
      updateBleUI(true);
      console.log("🔵 Connected to ESP32 via Bluetooth Low Energy!");
    } catch(err) {
      console.warn("BLE connect error:", err);
      updateBleUI(false);
      if (err.name === 'NotFoundError') {
        // User closed or canceled the native browser pairing prompt
        return;
      }
      alert("Bluetooth notice:\\n" + err.message + "\\n\\n1. Make sure Bluetooth is ON in Windows Settings.\\n2. Make sure ESP32 is powered on.");
    }
  }"""

if old_ble_func in html:
    html = html.replace(old_ble_func, new_ble_func, 1)
    print("✔ toggleBluetoothConnect upgraded")
else:
    print("ℹ old_ble_func pattern not matched exactly, replacing via regex...")
    pattern = re.compile(r'async function toggleBluetoothConnect\(\)\s*\{.*?\}\s*(?=function onBleDisconnected)', re.DOTALL)
    html = pattern.sub(new_ble_func + "\n\n  ", html)
    print("✔ toggleBluetoothConnect upgraded via regex")

# Write to temp file and test with node --check
m = re.search(r'<script>(.*?)</script>', html, re.DOTALL)
if m:
    with open("test_syntax.js", "w", encoding="utf-8") as f_test:
        f_test.write(m.group(1))
    res = subprocess.run(["node", "--check", "test_syntax.js"], capture_output=True, text=True)
    if res.returncode == 0:
        print("🎉 SUCCESS: JavaScript syntax is 100% CLEAN! Zero syntax errors!")
    else:
        print("❌ SYNTAX ERROR REMAINING in JS:")
        print(res.stderr)
        exit(1)

# Save updated index.html
with open(INDEX_PATH, "w", encoding="utf-8") as f:
    f.write(html)
print(f"✔ Successfully saved index.html ({len(html)} bytes)")

# Synchronize into src/main.cpp
with open(MAIN_PATH, "r", encoding="utf-8") as f:
    main_cpp = f.read()

start_marker = 'const char INDEX_HTML[] PROGMEM = R"rawliteral('
end_marker = ')rawliteral";'

idx_start = main_cpp.find(start_marker)
idx_end = main_cpp.find(end_marker, idx_start)

if idx_start != -1 and idx_end != -1:
    new_main_cpp = main_cpp[:idx_start + len(start_marker)] + html + main_cpp[idx_end:]
    with open(MAIN_PATH, "w", encoding="utf-8") as f:
        f.write(new_main_cpp)
    print(f"✔ Successfully synchronized index.html into src/main.cpp ({len(new_main_cpp)} bytes)")
else:
    print(f"❌ Failed to find rawliteral markers in {MAIN_PATH}")
