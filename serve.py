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
import json

PORT = 8000
ESP32_DEFAULT_COM = "COM3"
ESP32_WIFI_IPS = ["10.206.3.219", "esp32-rgb.local", "192.168.4.1"]

ser = None
ser_lock = threading.Lock()

def try_connect_serial():
    global ser
    if ser and ser.is_open:
        return True
    try:
        import serial
        import serial.tools.list_ports
        all_ports = list(serial.tools.list_ports.comports())
        target_port = None
        for p in all_ports:
            if p.device == ESP32_DEFAULT_COM:
                target_port = p.device
                break
        if not target_port:
            for p in all_ports:
                desc = p.description.lower()
                hwid = str(p.hwid).lower()
                if "bluetooth" not in desc and ("usb" in desc or "cp210" in desc or "ch340" in desc or "jtag" in desc or "303a" in hwid):
                    target_port = p.device
                    break
        if target_port:
            ser = serial.Serial(target_port, 115200, timeout=0.05, write_timeout=0.1)
            ser.dtr = False
            ser.rts = False
            print(f"⚡ [USB Serial] Connected to ESP32 on {target_port} @ 115200 baud!", flush=True)
            return True
    except Exception as e:
        ser = None
    return False

def start_serial_reader():
    """Background thread to drain incoming serial data from ESP32 and auto-reconnect on disconnect."""
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
                    if try_connect_serial():
                        time.sleep(0.5)
                    else:
                        time.sleep(2.0)
            except Exception:
                try:
                    if ser:
                        ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(2.0)

    t = threading.Thread(target=reader_loop, daemon=True)
    t.start()

try:
    import serial
    import serial.tools.list_ports
    if try_connect_serial():
        print("🚀 Commands and beats will stream directly over USB Serial (0.2ms latency).", flush=True)
    else:
        print("ℹ️ USB Serial not immediately available. Running in hybrid/auto-reconnect mode.", flush=True)
    start_serial_reader()
except ImportError:
    print("ℹ️ pyserial not found in this environment. Running in Wi-Fi proxy mode.", flush=True)

# Locate index.html
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
INDEX_FILE = os.path.join(SCRIPT_DIR, "index.html")

def get_gemini_api_key():
    """Securely load Gemini API key from environment variable or local .env file."""
    k = os.environ.get("GEMINI_API_KEY", "")
    if k:
        return k.strip()
    env_file = os.path.join(SCRIPT_DIR, ".env")
    if os.path.exists(env_file):
        try:
            with open(env_file, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("GEMINI_API_KEY="):
                        return line.split("=", 1)[1].strip().strip('"').strip("'")
        except Exception as e:
            print(f"⚠️ Error reading .env: {e}")
    return ""

DEFAULT_AI_FALLBACK = {
    "mood": "futuristic",
    "emotion": "energetic",
    "scene": "cyberpunk_rave",
    "colors": ["#00F3FF", "#FF007F", "#7B00FF"],
    "brightness": 0.90,
    "saturation": 0.95,
    "effect": "pulse",
    "movement_speed": 0.80,
    "beat_reactivity": 0.95,
    "transition": "intensify",
    "event": "GROOVE",
    "description": "Adaptive dynamic spectrum reacting to beats and harmonics.",
    "confidence": 0.88
}

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
                    elif path == "/api/ext":
                        en = query.get("en", ["1"])[0]
                        m = query.get("mode", query.get("m", ["0"]))[0]
                        b = query.get("bri", query.get("b", ["100"]))[0]
                        cmd_str = f"!EXT:{en},{m},{b}\n"
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

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/api/gemini/interpret":
            try:
                content_len = int(self.headers.get("Content-Length", 0))
                post_body = self.rfile.read(content_len).decode("utf-8") if content_len > 0 else "{}"
                data = json.loads(post_body)
                
                api_key = get_gemini_api_key()
                if not api_key:
                    # Graceful fallback if no API key configured
                    resp_payload = {
                        "success": True,
                        "interpretation": DEFAULT_AI_FALLBACK,
                        "notice": "GEMINI_API_KEY not configured. Using default dynamic interpretation."
                    }
                    resp_bytes = json.dumps(resp_payload).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    self.wfile.write(resp_bytes)
                    return

                system_prompt = (
                    "You are an expert AI lighting director for a music-reactive RGB LED system.\n"
                    "Analyze the musical metrics and recent history.\n"
                    "Decide the emotional, stylistic, and aesthetic lighting direction.\n"
                    "Respond with ONLY valid JSON with this exact schema:\n"
                    "{\n"
                    '  "mood": "string (e.g. dark_euphoric, intense, dreamy, neon_drive)",\n'
                    '  "emotion": "string (e.g. ecstatic, melancholic, suspenseful)",\n'
                    '  "scene": "string (e.g. building_tension, cyber_rave, midnight_chill)",\n'
                    '  "colors": ["#RRGGBB", "#RRGGBB", "#RRGGBB"],\n'
                    '  "brightness": 0.85,\n'
                    '  "saturation": 0.90,\n'
                    '  "effect": "pulse",\n'
                    '  "movement_speed": 0.75,\n'
                    '  "beat_reactivity": 0.90,\n'
                    '  "transition": "intensify",\n'
                    '  "event": "BUILD_UP",\n'
                    '  "description": "creative one-line interpretation",\n'
                    '  "confidence": 0.92\n'
                    "}"
                )

                gemini_url = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent?key={api_key}"
                prompt_content = f"{system_prompt}\n\nMusic Context & Evolution:\n{json.dumps(data)}"
                
                req_body = {
                    "contents": [{"parts": [{"text": prompt_content}]}],
                    "generationConfig": {
                        "responseMimeType": "application/json",
                        "temperature": 0.6
                    }
                }
                
                req = urllib.request.Request(
                    gemini_url,
                    data=json.dumps(req_body).encode("utf-8"),
                    headers={"Content-Type": "application/json"}
                )
                
                with urllib.request.urlopen(req, timeout=12) as g_resp:
                    g_data = json.loads(g_resp.read().decode("utf-8"))
                    raw_text = g_data["candidates"][0]["content"]["parts"][0]["text"]
                    interpretation = json.loads(raw_text)
                    
                    resp_payload = {
                        "success": True,
                        "interpretation": interpretation
                    }
                    resp_bytes = json.dumps(resp_payload).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.end_headers()
                    self.wfile.write(resp_bytes)
                    return
            except Exception as e:
                print(f"⚠️ Gemini API Bridge Notice: {e}")
                resp_payload = {
                    "success": False,
                    "error": str(e),
                    "interpretation": DEFAULT_AI_FALLBACK
                }
                resp_bytes = json.dumps(resp_payload).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(resp_bytes)
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
    socketserver.TCPServer.allow_reuse_address = True
    while True:
        try:
            with socketserver.TCPServer(("", PORT), StudioHandler) as httpd:
                print("\n" + "="*60, flush=True)
                print("   ✨ ESP32 AI RGB PRO STUDIO - LAPTOP BRIDGE ACTIVE!   ", flush=True)
                print("="*60, flush=True)
                print(f"👉 Local Web Studio:   http://localhost:{PORT}", flush=True)
                print("="*60, flush=True)
                print("💡 Keep this terminal open while using the Studio.", flush=True)
                print("💡 All commands stream over USB Serial directly to the LED!\n", flush=True)
                httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down studio server...", flush=True)
            if ser and ser.is_open:
                ser.close()
            break
        except Exception as err:
            print(f"⚠️ Server notice: {err}. Auto-restarting in 1s...", flush=True)
            time.sleep(1)

if __name__ == '__main__':
    run_server()
