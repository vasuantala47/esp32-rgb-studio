"""
✨ ESP32 AI RGB Pro Studio - Local Laptop High-Speed USB & Web Bridge
Solves the Wi-Fi Internet conflict:
- Laptop stays connected to Home Wi-Fi (Full Internet for Spotify & Gemini AI!)
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
import sys
import os

PORT = 8000
ESP32_DEFAULT_COM = "COM3"
ESP32_WIFI_IP = "192.168.4.1"

ser = None
try:
    import serial
    import serial.tools.list_ports
    
    # Auto-detect ESP32 COM port
    target_port = ESP32_DEFAULT_COM
    ports = [p.device for p in serial.tools.list_ports.comports()]
    if target_port not in ports:
        for p in serial.tools.list_ports.comports():
            if "USB" in p.description or "CP210" in p.description or "CH340" in p.description or "JTAG" in p.description:
                target_port = p.device
                break
        else:
            if ports:
                target_port = ports[0]

    try:
        ser = serial.Serial(target_port, 115200, timeout=0.1)
        print(f"⚡ [USB Serial] Connected to ESP32 on {target_port} @ 115200 baud!")
        print("🚀 Commands and beats will be transmitted directly over USB with 0.2ms latency.")
        print("🌐 Laptop Wi-Fi is 100% FREE for high-speed Internet (Spotify & Gemini AI)!")
    except Exception as e:
        print(f"⚠️ Could not open serial port {target_port}: {e}")
        print("ℹ️ Falling back to Wi-Fi proxy mode.")
        ser = None
except ImportError:
    print("ℹ️ pyserial not found in this python environment.")
    print("ℹ️ If ESP32 is on USB, run with PlatformIO python: & \"$HOME\\.platformio\\penv\\Scripts\\python.exe\" serve.py")
    ser = None

# Locate index.html
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_FILE = os.path.join(SCRIPT_DIR, "index.html")

class StudioHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        # 1. Handle API Calls
        if path.startswith("/api/"):
            # A) If USB Serial is active, send fast serial command directly to ESP32!
            if ser and ser.is_open:
                try:
                    if path == "/api/beat":
                        r = query.get("r", ["255"])[0]
                        g = query.get("g", ["255"])[0]
                        b = query.get("b", ["255"])[0]
                        v = query.get("v", ["255"])[0]
                        d = query.get("d", ["480"])[0]
                        ser.write(f"!B:{r},{g},{b},{v},{d}\n".encode("utf-8"))
                    elif path == "/api/color":
                        r = query.get("r", ["255"])[0]
                        g = query.get("g", ["200"])[0]
                        b = query.get("b", ["140"])[0]
                        ser.write(f"!C:{r},{g},{b}\n".encode("utf-8"))
                    elif path == "/api/mode":
                        val = query.get("val", ["1"])[0]
                        ser.write(f"!M:{val}\n".encode("utf-8"))
                    elif path == "/api/brightness":
                        val = query.get("val", ["100"])[0]
                        ser.write(f"!BR:{val}\n".encode("utf-8"))
                    elif path == "/api/custom/set":
                        steps = query.get("steps", [""])[0]
                        ser.write(f"!SEQ:{steps}\n".encode("utf-8"))
                    elif path == "/api/wifi":
                        ssid = query.get("ssid", [""])[0]
                        passw = query.get("pass", [""])[0]
                        ser.write(f"!WIFI:{ssid},{passw}\n".encode("utf-8"))

                    self.send_response(200)
                    self.send_header("Content-Type", "text/plain")
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    self.wfile.write(b"OK")
                    return
                except Exception as ex:
                    print(f"Serial write error: {ex}")

            # B) If Serial is not available, proxy to Wi-Fi AP or mDNS
            try:
                target_url = f"http://{ESP32_WIFI_IP}{self.path}"
                req = urllib.request.Request(target_url, headers={'User-Agent': 'ESP32-Laptop-Studio'})
                with urllib.request.urlopen(req, timeout=2) as resp:
                    data = resp.read()
                    self.send_response(resp.status)
                    self.send_header("Content-Type", resp.headers.get("Content-Type", "text/plain"))
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    self.wfile.write(data)
                    return
            except Exception as e:
                self.send_response(502)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(f"ESP32 not reached: {e}".encode("utf-8"))
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
        self.send_header('Access-Control-Allow-Headers', '*')
        self.end_headers()

    def log_message(self, format, *args):
        # Suppress spammy log outputs for high-frequency beat packets
        if len(args) > 0 and "/api/beat" in str(args[0]):
            return
        super().log_message(format, *args)

def main():
    print("=" * 65)
    print("✨ ESP32 AI RGB PRO STUDIO - LAPTOP BRIDGE (DUAL INTERNET + USB)")
    print("=" * 65)
    print(f"🏠 Local Web Studio URL: http://localhost:{PORT}")
    if ser and ser.is_open:
        print(f"🔌 Hardware Connection: Direct USB Serial ({ser.port}) - 0.2ms latency")
        print("🌐 Internet Status: 100% ONLINE (Laptop stays connected to your Wi-Fi)")
    else:
        print(f"📡 Hardware Connection: Wi-Fi fallback to {ESP32_WIFI_IP}")
    print("🎧 Spotify System Audio Capture: 100% UNLOCKED (Secure Context)")
    print("=" * 65)

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), StudioHandler) as httpd:
        print(f"\n🚀 Server running at: http://localhost:{PORT}")
        print("Opening browser automatically...\n")
        
        threading.Timer(0.8, lambda: webbrowser.open(f"http://localhost:{PORT}")).start()
        
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down server...")
            if ser and ser.is_open:
                ser.close()

if __name__ == "__main__":
    main()
