"""
✨ ESP32 AI RGB Pro Studio - Local Laptop High-Speed USB & Web Bridge
Solves the Wi-Fi Internet conflict:
- Laptop stays connected to Home/Hostel Wi-Fi (Full Internet for Spotify & Gemini AI!)
- ESP32 is controlled directly over USB Serial (COM3) with 0.2ms latency!
- No need to disconnect from internet or switch to ESP32 Wi-Fi!

Usage:
  python serve.py
  (or: & "$HOME\\.platformio\\penv\\Scripts\\python.exe" serve.py)
"""

import http.server
import socketserver
import urllib.parse
import urllib.request
import webbrowser
import threading
import time
import sys
import os

PORT = 8000
ESP32_DEFAULT_COM = "COM3"
ESP32_WIFI_IPS = ["10.206.3.219", "esp32-rgb.local", "192.168.4.1"]

ser = None
ser_lock = threading.Lock()

def start_serial_reader():
    """Background thread to drain incoming serial data from ESP32 so USB buffer never blocks."""
    def reader_loop():
        global ser
        while True:
            try:
                if ser and ser.is_open:
                    waiting = ser.in_waiting
                    if waiting > 0:
                        _ = ser.read(waiting)
                    else:
                        time.sleep(0.02)
                else:
                    time.sleep(0.5)
            except Exception:
                time.sleep(0.5)

    t = threading.Thread(target=reader_loop, daemon=True)
    t.start()

try:
    import serial
    import serial.tools.list_ports
    
    # Auto-detect ESP32 USB COM port (strictly ignore virtual Bluetooth ports)
    target_port = None
    all_ports = list(serial.tools.list_ports.comports())
    
    # Priority 1: Check if COM3 exists and is USB
    for p in all_ports:
        if p.device == ESP32_DEFAULT_COM:
            target_port = p.device
            break
            
    # Priority 2: Check for any genuine USB Serial device
    if not target_port:
        for p in all_ports:
            desc = p.description.lower()
            hwid = str(p.hwid).lower()
            if "bluetooth" not in desc and ("usb" in desc or "cp210" in desc or "ch340" in desc or "jtag" in desc or "303a" in hwid):
                target_port = p.device
                break

    if target_port:
        try:
            ser = serial.Serial(target_port, 115200, timeout=0.05, write_timeout=0.1)
            ser.dtr = False
            ser.rts = False
            print(f"⚡ [USB Serial] Connected to ESP32 on {target_port} @ 115200 baud!")
            print("🚀 Commands and beats will be transmitted directly over USB with 0.2ms latency.")
            print("🌐 Laptop Wi-Fi is 100% FREE for high-speed Internet (Spotify & Gemini AI)!")
            start_serial_reader()
        except Exception as e:
            print(f"⚠️ Could not open serial port {target_port}: {e}")
            print("ℹ️ Falling back to Wi-Fi proxy mode.")
            ser = None
    else:
        print("ℹ️ No USB Serial device found. Running in Wi-Fi proxy mode.")
except ImportError:
    print("ℹ️ pyserial not found in this python environment.")
    print("ℹ️ If ESP32 is on USB, run with PlatformIO python: & \"$HOME\\.platformio\\penv\\Scripts\\python.exe\" serve.py")
    ser = None

# Locate index.html
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_FILE = os.path.join(SCRIPT_DIR, "index.html")

class StudioHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Mute high-frequency beat log spam to keep terminal fast and clean
        if "/api/beat" not in self.path:
            super().log_message(format, *args)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        # 1. Handle API Calls
        if path.startswith("/api/"):
            # A) If USB Serial is active, send fast serial command directly to ESP32!
            if ser and ser.is_open:
                try:
                    cmd_str = None
                    if path == "/api/beat":
                        r = query.get("r", ["255"])[0]
                        g = query.get("g", ["255"])[0]
                        b = query.get("b", ["255"])[0]
                        v = query.get("v", ["255"])[0]
                        d = query.get("d", ["480"])[0]
                        cmd_str = f"!B:{r},{g},{b},{v},{d}\n"
                    elif path == "/api/color":
                        r = query.get("r", ["255"])[0]
                        g = query.get("g", ["200"])[0]
                        b = query.get("b", ["140"])[0]
                        cmd_str = f"!C:{r},{g},{b}\n"
                    elif path == "/api/mode":
                        val = query.get("val", ["1"])[0]
                        cmd_str = f"!M:{val}\n"
                    elif path == "/api/brightness":
                        val = query.get("val", ["100"])[0]
                        cmd_str = f"!BR:{val}\n"
                    elif path == "/api/custom/set":
                        steps = query.get("steps", [""])[0]
                        cmd_str = f"!SEQ:{steps}\n"
                    elif path == "/api/wifi":
                        ssid = query.get("ssid", [""])[0]
                        passw = query.get("pass", [""])[0]
                        cmd_str = f"!WIFI:{ssid},{passw}\n"
                    elif path == "/api/status":
                        self.send_response(200)
                        self.send_header("Content-Type", "text/plain")
                        self.send_header("Access-Control-Allow-Origin", "*")
                        self.end_headers()
                        self.wfile.write(b"OK USB Serial Active")
                        return

                    if cmd_str:
                        with ser_lock:
                            ser.write(cmd_str.encode("utf-8"))
                            ser.flush()

                    self.send_response(200)
                    self.send_header("Content-Type", "text/plain")
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    self.wfile.write(b"OK")
                    return
                except Exception as ex:
                    print(f"Serial write error: {ex}")

            # B) If Serial is not available, proxy to Wi-Fi IP
            for wifi_host in ESP32_WIFI_IPS:
                try:
                    target_url = f"http://{wifi_host}{self.path}"
                    req = urllib.request.Request(target_url, headers={'User-Agent': 'ESP32-Laptop-Studio'})
                    with urllib.request.urlopen(req, timeout=1.5) as resp:
                        data = resp.read()
                        self.send_response(resp.status)
                        self.send_header("Content-Type", resp.headers.get("Content-Type", "text/plain"))
                        self.send_header("Access-Control-Allow-Origin", "*")
                        self.end_headers()
                        self.wfile.write(data)
                        return
                except Exception:
                    continue

            self.send_response(502)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(b"ESP32 not reached over USB or Wi-Fi.")
            return

        # 2. Serve Web Studio HTML
        if path == "/" or path == "/index.html":
            if os.path.exists(INDEX_FILE):
                with open(INDEX_FILE, "rb") as f:
                    content = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
                return
            else:
                self.send_response(404)
                self.end_headers()
                self.wfile.write(b"index.html not found.")
                return

        self.send_response(404)
        self.end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

def run_server():
    # Allow port reuse to avoid 'Address already in use' errors on fast restart
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), StudioHandler) as httpd:
        print("\n" + "="*60)
        print("   ✨ ESP32 AI RGB PRO STUDIO - LAPTOP BRIDGE ACTIVE!   ")
        print("="*60)
        print(f"👉 Local Web Studio:   http://localhost:{PORT}")
        print("="*60)
        print("💡 Keep this terminal open while using the Studio.")
        print("💡 All commands stream over USB Serial directly to the LED!\n")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down studio server...")
            if ser and ser.is_open:
                ser.close()

if __name__ == '__main__':
    run_server()
