#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Forward declaration
void processIncomingChar(char c);

// ====================================================================
// 🔵 BLUETOOTH LOW ENERGY (NORDIC UART SERVICE)
// ====================================================================
#define BLE_SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer *pBleServer = nullptr;
BLECharacteristic *pBleTxCharacteristic = nullptr;
bool bleDeviceConnected = false;
bool oldBleDeviceConnected = false;

class MyBleServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        bleDeviceConnected = true;
        Serial.println("\r\n[BLE] Web Client Connected via Bluetooth!");
    };

    void onDisconnect(BLEServer* pServer) {
        bleDeviceConnected = false;
        Serial.println("\r\n[BLE] Web Client Disconnected from Bluetooth!");
    }
};

class MyBleRxCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0) {
            for (size_t i = 0; i < rxValue.length(); i++) {
                processIncomingChar(rxValue[i]);
            }
            processIncomingChar('\n');
        }
    }
};

// ====================================================================
// 🛠️ HARDWARE & NETWORK CONFIGURATION
// ====================================================================
#define PHYSICAL_RGB_PIN 48
uint8_t rgbPin = PHYSICAL_RGB_PIN; // Physical GPIO 48 for onboard WS2812 RGB LED
uint8_t masterBrightness = 100;    // 0 - 100% (Default 100% for maximum vivid punch!)

// ====================================================================
// 🌈 EXTERNAL 8-LED RGB BAR (KEYES LED RGB V2 ARTOU MULTIPLEXED MODULE)
// ====================================================================
#define PIN_EXT_R   4   // Color Cathode Red (Active LOW)
#define PIN_EXT_G   5   // Color Cathode Green (Active LOW)
#define PIN_EXT_B   6   // Color Cathode Blue (Active LOW)

const uint8_t extDigitPins[8] = { 7, 8, 9, 10, 11, 12, 13, 14 }; // Digits D0-D7 Anode PNP (Active LOW)

#define EXT_LEDC_CH_R 0
#define EXT_LEDC_CH_G 1
#define EXT_LEDC_CH_B 2
#define EXT_LEDC_FREQ 20000 // 20 kHz ultrasonic PWM (Flicker-free & silent)
#define EXT_LEDC_RES  8     // 8-bit resolution (0 - 255)

struct ExtRGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

ExtRGB extLeds[8] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}
};

bool extRgbEnabled = true;
uint8_t extRgbMode = 0;        // 0: Mirror Main, 1: VU Meter, 2: Spectrum Flow, 3: Rainbow Wave, 4: Cyber Scanner, 5: Fire Embers, 6: Direct Framebuffer
uint8_t extBrightness = 100;   // 0 - 100%

// VU Meter peak decay state
float extVuLevel = 0.0f;
float extPeakDot = 0.0f;
unsigned long lastPeakDropTime = 0;

// Wi-Fi Access Point Configuration
const char* ap_ssid = "ESP32-RGB-Studio";
const char* ap_pass = "12345678"; // At least 8 characters

WebServer server(80);
Preferences preferences;

// ====================================================================
// 🎭 20 LIGHTING MODES & STATE ENGINE
// ====================================================================
int currentMode = 1;
bool autoCycle = false;
unsigned long lastAutoSwitch = 0;
const unsigned long AUTO_CYCLE_INTERVAL = 10000;

// Static Solid Color Storage (Free Light / Lamp)
uint8_t staticR = 255, staticG = 200, staticB = 140;

// Dynamic Audio Beat Reactive State
volatile unsigned long lastBeatTime = 0;
volatile uint8_t beatR = 255, beatG = 255, beatB = 255;
volatile uint8_t beatIntensity = 255;
volatile uint16_t beatDecayMs = 480;

// Custom User / AI Step Sequence Structure
struct SequenceStep {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t durationMs;
    bool fade;
};

const int MAX_CUSTOM_STEPS = 32;
SequenceStep customSteps[MAX_CUSTOM_STEPS];
int customStepCount = 0;
int currentCustomIndex = 0;
unsigned long stepStartTime = 0;

String terminalBuffer = "";
bool isSerialCommand = false;
String serialCmdBuffer = "";
bool menuPrinted = false;

// ====================================================================
// 🎨 COLOR & LED DRIVER
// ====================================================================
// High-Speed 1ms Multiplexer Step for 8-LED Keyes Module
void stepExtRgbMultiplex() {
    static uint8_t curDigit = 0;

    // 1. Ghosting prevention: Disable all digits first (Active LOW -> write HIGH)
    for (int i = 0; i < 8; i++) {
        digitalWrite(extDigitPins[i], HIGH);
    }

    // 2. Set colors and enable current digit
    if (extRgbEnabled && masterBrightness > 0 && extBrightness > 0) {
        float bri = ((float)masterBrightness / 100.0f) * ((float)extBrightness / 100.0f);
        uint8_t r = (uint8_t)(extLeds[curDigit].r * bri);
        uint8_t g = (uint8_t)(extLeds[curDigit].g * bri);
        uint8_t b = (uint8_t)(extLeds[curDigit].b * bri);

        // Active LOW cathodes on 20kHz LEDC channels (255 - level)
        ledcWrite(EXT_LEDC_CH_R, 255 - r);
        ledcWrite(EXT_LEDC_CH_G, 255 - g);
        ledcWrite(EXT_LEDC_CH_B, 255 - b);

        // Turn ON current digit (Active LOW -> write LOW)
        digitalWrite(extDigitPins[curDigit], LOW);
    } else {
        ledcWrite(EXT_LEDC_CH_R, 255);
        ledcWrite(EXT_LEDC_CH_G, 255);
        ledcWrite(EXT_LEDC_CH_B, 255);
    }

    curDigit = (curDigit + 1) % 8;
}

void extRgbMultiplexTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1); // 1 millisecond
    for (;;) {
        stepExtRgbMultiplex();
        vTaskDelayUntil(&xLastWakeTime, xFrequency > 0 ? xFrequency : 1);
    }
}

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
    float bri = (float)masterBrightness / 100.0f;
    uint8_t adjR = (uint8_t)(r * bri);
    uint8_t adjG = (uint8_t)(g * bri);
    uint8_t adjB = (uint8_t)(b * bri);
    neopixelWrite(rgbPin, adjR, adjG, adjB);

    if (extRgbMode == 0) { // Mirror Mode: all 8 external LEDs mirror main RGB
        for (int i = 0; i < 8; i++) {
            extLeds[i].r = r;
            extLeds[i].g = g;
            extLeds[i].b = b;
        }
    }
}

void updateExternalRgbEffects(unsigned long now) {
    if (!extRgbEnabled) {
        for (int i = 0; i < 8; i++) {
            extLeds[i].r = 0; extLeds[i].g = 0; extLeds[i].b = 0;
        }
        return;
    }

    if (extRgbMode == 0) {
        // Mode 0 (Mirror): synchronized directly in setRGB()
        return;
    }

    if (extRgbMode == 1) {
        // 📊 Mode 1: 8-LED Audio VU Meter & Falling Peak Indicator
        float factor = 0.0f;
        if (currentMode == 11) {
            unsigned long elapsed = now - lastBeatTime;
            if (elapsed < beatDecayMs && beatIntensity >= 10) {
                float progress = (float)elapsed / (float)beatDecayMs;
                factor = 0.5f * (1.0f + cosf(3.14159265f * progress)) * ((float)beatIntensity / 255.0f);
            }
        } else {
            factor = (sinf(now / 350.0f) + 1.0f) * 0.4f;
        }

        float targetVu = factor * 8.0f;
        if (targetVu > extVuLevel) {
            extVuLevel = targetVu;
        } else {
            extVuLevel -= 0.18f;
            if (extVuLevel < 0) extVuLevel = 0;
        }

        if (extVuLevel >= extPeakDot) {
            extPeakDot = extVuLevel;
            lastPeakDropTime = now;
        } else if (now - lastPeakDropTime > 45) {
            extPeakDot -= 0.12f;
            if (extPeakDot < 0) extPeakDot = 0;
            lastPeakDropTime = now;
        }

        const ExtRGB vuColors[8] = {
            {0, 255, 60},   // 0: Emerald Green
            {40, 255, 20},  // 1: Lime Green
            {120, 255, 0},  // 2: Yellow-Green
            {230, 220, 0},  // 3: Golden Yellow
            {255, 140, 0},  // 4: Amber Orange
            {255, 60, 0},   // 5: Fiery Orange
            {255, 15, 20},  // 6: Crimson Red
            {255, 0, 110}   // 7: Peak Magenta
        };

        for (int i = 0; i < 8; i++) {
            float ledThreshold = (float)i;
            if (extVuLevel >= ledThreshold + 1.0f) {
                extLeds[i] = vuColors[i];
            } else if (extVuLevel > ledThreshold) {
                float frac = extVuLevel - ledThreshold;
                extLeds[i].r = (uint8_t)(vuColors[i].r * frac);
                extLeds[i].g = (uint8_t)(vuColors[i].g * frac);
                extLeds[i].b = (uint8_t)(vuColors[i].b * frac);
            } else {
                extLeds[i] = {0, 0, 0};
            }

            int peakIdx = (int)extPeakDot;
            if (peakIdx == i && extPeakDot > 0.5f) {
                extLeds[i] = {255, 255, 255}; // Peak Dot
            }
        }
        return;
    }

    if (extRgbMode == 2) {
        // 🌈 Mode 2: 8-LED Frequency Spectrum Flow
        float wave = (sinf(now / 400.0f) + 1.0f) / 2.0f;
        float beatBoost = (now - lastBeatTime < beatDecayMs) ? ((float)beatIntensity / 255.0f) : 0.2f;

        // Bass zone
        extLeds[0] = { (uint8_t)(255 * beatBoost), 0, (uint8_t)(180 * beatBoost) };
        extLeds[1] = { (uint8_t)(220 * beatBoost), 0, (uint8_t)(220 * beatBoost) };

        // Vocal / Mid zone
        float midFactor = 0.4f + 0.6f * wave;
        extLeds[2] = { (uint8_t)(255 * midFactor), (uint8_t)(40 * midFactor), (uint8_t)(140 * midFactor) };
        extLeds[3] = { (uint8_t)(255 * midFactor), (uint8_t)(160 * midFactor), 0 };
        extLeds[4] = { (uint8_t)(200 * midFactor), (uint8_t)(255 * midFactor), 0 };

        // Treble zone
        float trebFactor = 0.5f + 0.5f * cosf(now / 300.0f);
        extLeds[5] = { 0, (uint8_t)(240 * trebFactor), (uint8_t)(255 * trebFactor) };
        extLeds[6] = { (uint8_t)(80 * trebFactor), (uint8_t)(180 * trebFactor), 255 };
        extLeds[7] = { (uint8_t)(255 * trebFactor), (uint8_t)(255 * trebFactor), 255 };
        return;
    }

    if (extRgbMode == 3) {
        // ⚡ Mode 3: 8-LED Traveling Rainbow Wave
        for (int i = 0; i < 8; i++) {
            float hue = fmodf((now / 10.0f) + (i * 45.0f), 360.0f);
            float c = 1.0f;
            float x = c * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
            float r = 0, g = 0, b = 0;
            if (hue < 60)       { r = c; g = x; b = 0; }
            else if (hue < 120) { r = x; g = c; b = 0; }
            else if (hue < 180) { r = 0; g = c; b = x; }
            else if (hue < 240) { r = 0; g = x; b = c; }
            else if (hue < 300) { r = x; g = 0; b = c; }
            else                { r = c; g = 0; b = x; }
            extLeds[i] = { (uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255) };
        }
        return;
    }

    if (extRgbMode == 4) {
        // 🏎️ Mode 4: 8-LED Cyber Scanner / Knight Rider
        float pos = 3.5f + 3.5f * sinf(now / 220.0f);
        for (int i = 0; i < 8; i++) {
            float dist = fabsf((float)i - pos);
            float bri = expf(-dist * 1.8f);
            if (bri < 0.02f) bri = 0;
            extLeds[i] = { (uint8_t)(255 * bri), (uint8_t)(15 * bri), (uint8_t)(40 * bri) };
        }
        return;
    }

    if (extRgbMode == 5) {
        // 🔥 Mode 5: 8-LED Organic Campfire Embers
        for (int i = 0; i < 8; i++) {
            float flicker = (random(55, 100) / 100.0f);
            float wave = (sinf((now + i * 140) / 180.0f) + 1.0f) / 2.0f;
            uint8_t r = (uint8_t)(255 * flicker);
            uint8_t g = (uint8_t)((35 + wave * 65) * flicker);
            uint8_t b = (uint8_t)((wave > 0.88f ? 12 : 0) * flicker);
            extLeds[i] = { r, g, b };
        }
        return;
    }
}

void setHSV(float h, float s, float v) {
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    setRGB((uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255), (uint8_t)((b + m) * 255));
}

// ====================================================================
// 🌟 20 HANDCRAFTED PRESET ALGORITHMS
// ====================================================================
void modeAurora(unsigned long now) {
    float wave1 = (sinf(now / 1200.0f) + 1.0f) / 2.0f;
    float wave2 = (cosf(now / 1900.0f) + 1.0f) / 2.0f;
    float hue = 120.0f + (wave1 * 60.0f) + (wave2 * 50.0f);
    setHSV(hue, 0.95f, 0.5f + 0.5f * wave1);
}

void modeCyberpunk(unsigned long now) {
    unsigned long cycle = now % 2400;
    if (cycle < 1200) {
        float p = (float)cycle / 1200.0f;
        float pulse = sinf(p * 3.14159f);
        setRGB((uint8_t)(255 * pulse), 0, (uint8_t)(140 * pulse));
    } else {
        float p = (float)(cycle - 1200) / 1200.0f;
        float pulse = sinf(p * 3.14159f);
        setRGB(0, (uint8_t)(240 * pulse), (uint8_t)(255 * pulse));
    }
}

void modeCampfire(unsigned long now) {
    static float flameBri = 0.8f;
    float flicker = (random(70, 100) / 100.0f);
    flameBri = flameBri * 0.7f + flicker * 0.3f;
    float wave = (sinf(now / 160.0f) + 1.0f) / 2.0f;
    uint8_t r = (uint8_t)(255 * flameBri);
    uint8_t g = (uint8_t)((40 + wave * 55) * flameBri);
    uint8_t b = (uint8_t)((wave > 0.85f ? 15 : 0) * flameBri);
    setRGB(r, g, b);
}

void modeRainbow(unsigned long now) {
    float hue = fmodf((now / 12.0f), 360.0f);
    setHSV(hue, 1.0f, 1.0f);
}

void modeStrobe(unsigned long now) {
    unsigned long cycle = now % 900;
    if (cycle < 120) {
        setRGB((cycle % 40 < 20) ? 255 : 0, 0, 0);
    } else if (cycle >= 120 && cycle < 200) {
        setRGB(0, 0, 0);
    } else if (cycle >= 200 && cycle < 320) {
        setRGB(0, 0, (cycle % 40 < 20) ? 255 : 0);
    } else {
        setRGB(0, 0, 0);
    }
}

void modeBreathingWhite(unsigned long now) {
    float wave = (sinf(now / 800.0f) + 1.0f) / 2.0f;
    float bri = 0.05f + 0.95f * (wave * wave);
    setRGB((uint8_t)(255 * bri), (uint8_t)(245 * bri), (uint8_t)(230 * bri));
}

void modeTrafficLight(unsigned long now) {
    unsigned long cycle = now % 6000;
    if (cycle < 2500) setRGB(255, 0, 0);
    else if (cycle < 3500) setRGB(255, 160, 0);
    else setRGB(0, 255, 0);
}

void modeLightning(unsigned long now) {
    static unsigned long nextStrike = 0;
    static unsigned long flashDuration = 0;
    static int burstCount = 0;
    static bool inFlash = false;

    if (now > nextStrike && !inFlash) {
        burstCount = random(2, 6);
        inFlash = true;
        flashDuration = now + random(20, 60);
        nextStrike = now + random(2500, 6500);
    }
    if (inFlash) {
        if (now < flashDuration) setRGB(255, 255, 255);
        else {
            burstCount--;
            if (burstCount > 0) {
                flashDuration = now + random(30, 80);
                nextStrike = now + random(50, 150);
            } else inFlash = false;
            setRGB(5, 10, 30);
        }
    } else setRGB(5, 10, 30);
}

void modePartyBeat(unsigned long now) {
    unsigned long cycle = now % 450;
    float decay = expf(-((float)cycle / 120.0f));
    static float baseHue = 0;
    if (cycle < 20) baseHue = fmodf(baseHue + 67.0f, 360.0f);
    setHSV(baseHue, 1.0f, decay);
}

// 🪩 Dynamic Multi-Intensity Beat Reactive with Analog Cosine Ease-Out Glow
void modeBeatReactive(unsigned long now) {
    if (beatIntensity < 10) {
        setRGB(0, 0, 0); // Sudden Beat Drop / Pause / Silence: 100% Instant Blackout!
        return;
    }
    unsigned long elapsed = now - lastBeatTime;
    if (elapsed < beatDecayMs) {
        float progress = (float)elapsed / (float)beatDecayMs;
        // Cosine ease-out: bold, punchy impact that gently tapers with a warm, lingering analog glow!
        float factor = 0.5f * (1.0f + cosf(3.14159265f * progress)) * ((float)beatIntensity / 255.0f);
        if (factor < 0.08f && beatIntensity >= 50) factor = 0.08f;
        setRGB((uint8_t)(beatR * factor), (uint8_t)(beatG * factor), (uint8_t)(beatB * factor));
    } else {
        if (now - lastBeatTime < 2500 && beatIntensity >= 40) {
            setRGB((uint8_t)(beatR * 0.06f), (uint8_t)(beatG * 0.06f), (uint8_t)(beatB * 0.06f));
        } else {
            setRGB(0, 0, 0);
        }
    }
}

void modeThriller(unsigned long now) {
    unsigned long cycle = now % 8000;
    if (cycle < 4500) {
        float b = 0.08f + 0.25f * ((sinf(now / 1000.0f) + 1.0f) / 2.0f);
        setRGB((uint8_t)(220 * b), 0, (uint8_t)(40 * b));
    } else if (cycle >= 4500 && cycle < 4650) {
        setRGB(255, 255, 255); // Jump-scare flash
    } else if (cycle >= 4650 && cycle < 4800) {
        setRGB(0, 0, 0);       // Pitch-black silence
    } else if (cycle >= 4800 && cycle < 4950) {
        setRGB(255, 10, 0);    // Blood-red pulse
    } else {
        float fade = 1.0f - ((float)(cycle - 4950) / 3050.0f);
        setRGB((uint8_t)(140 * fade), 0, (uint8_t)(20 * fade));
    }
}

void modePeace(unsigned long now) {
    float wave = (sinf(now / 3500.0f) + 1.0f) / 2.0f;
    float hue = 175.0f + (wave * 90.0f);
    float bri = 0.45f + (0.40f * sinf(now / 2000.0f));
    setHSV(hue, 0.65f, bri);
}

void modeOcean(unsigned long now) {
    float wave = (sinf(now / 2200.0f) + 1.0f) / 2.0f;
    float hue = 190.0f + (wave * 45.0f);
    float bri = 0.4f + 0.6f * wave;
    setHSV(hue, 0.9f, bri);
}

void modeVolcano(unsigned long now) {
    float wave = (sinf(now / 1100.0f) + 1.0f) / 2.0f;
    uint8_t r = 255;
    uint8_t g = (uint8_t)(35 + wave * 65);
    uint8_t b = (random(0, 100) > 96) ? 90 : 0;
    setRGB(r, g, b);
}

void modeForest(unsigned long now) {
    float wave = (sinf(now / 2400.0f) + 1.0f) / 2.0f;
    float hue = 110.0f + (wave * 35.0f);
    float firefly = (random(0, 100) > 94) ? 0.35f : 0.0f;
    setHSV(hue, 0.95f, 0.45f + 0.35f * wave + firefly);
}

void modeCandle(unsigned long now) {
    float f1 = sinf(now / 150.0f);
    float f2 = sinf(now / 67.0f);
    float f3 = (random(85, 100) / 100.0f);
    float bri = (0.65f + 0.22f * f1 + 0.13f * f2) * f3;
    if (bri > 1.0f) bri = 1.0f;
    setRGB((uint8_t)(255 * bri), (uint8_t)(140 * bri), (uint8_t)(20 * bri));
}

void modeNeonTokyo(unsigned long now) {
    unsigned long cycle = now % 3000;
    float p = (float)cycle / 3000.0f;
    if (p < 0.33f) setRGB(255, 0, 140);
    else if (p < 0.66f) setRGB(140, 0, 255);
    else setRGB(0, 245, 255);
}

void modeGlacier(unsigned long now) {
    float wave = (sinf(now / 2000.0f) + 1.0f) / 2.0f;
    uint8_t g = (uint8_t)(180 + wave * 75);
    uint8_t r = (uint8_t)(120 + wave * 135);
    setRGB(r, g, 255);
}

void modeMatrix(unsigned long now) {
    static unsigned long nextGlitch = 0;
    if (now > nextGlitch) {
        setRGB(0, 255, 60);
        nextGlitch = now + random(150, 600);
    } else {
        setRGB(0, random(15, 55), 8);
    }
}

void runCustomSequence(unsigned long now) {
    if (customStepCount == 0) return;

    SequenceStep cur = customSteps[currentCustomIndex];
    unsigned long elapsed = now - stepStartTime;

    if (elapsed >= cur.durationMs) {
        currentCustomIndex = (currentCustomIndex + 1) % customStepCount;
        stepStartTime = now;
        cur = customSteps[currentCustomIndex];
        elapsed = 0;
    }

    if (!cur.fade) {
        setRGB(cur.r, cur.g, cur.b);
    } else {
        int nextIdx = (currentCustomIndex + 1) % customStepCount;
        SequenceStep nxt = customSteps[nextIdx];
        float progress = (float)elapsed / (float)cur.durationMs;
        uint8_t r = (uint8_t)(cur.r + (nxt.r - cur.r) * progress);
        uint8_t g = (uint8_t)(cur.g + (nxt.g - cur.g) * progress);
        uint8_t b = (uint8_t)(cur.b + (nxt.b - cur.b) * progress);
        setRGB(r, g, b);
    }
}

// ====================================================================
// 🌐 EMBEDDED WEB DASHBOARD HTML / CSS / JAVASCRIPT
// ====================================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>ESP32 AI RGB Pro Studio</title>
  <style>
    :root {
      --bg: #0b0e17;
      --card-bg: rgba(22, 27, 44, 0.85);
      --card-border: rgba(255, 255, 255, 0.08);
      --accent: #6366f1;
      --accent-glow: rgba(99, 102, 241, 0.4);
      --pink: #ec4899;
      --cyan: #06b6d4;
      --red: #ef4444;
      --green: #10b981;
      --amber: #f59e0b;
      --text: #f8fafc;
      --subtext: #94a3b8;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); padding-bottom: 70px; min-height: 100vh; overflow-x: hidden; }

    /* Glass Header */
    header {
      position: sticky; top: 0; z-index: 100;
      background: rgba(11, 14, 23, 0.88); backdrop-filter: blur(12px);
      padding: 12px 16px; border-bottom: 1px solid var(--card-border);
      display: flex; align-items: center; justify-content: space-between;
    }
    .logo-group { display: flex; align-items: center; gap: 8px; }
    .logo-group h1 { font-size: 1.05rem; font-weight: 800; background: linear-gradient(135deg, #a855f7, #6366f1, #06b6d4); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
    .status-pill { font-size: 0.68rem; padding: 3px 8px; border-radius: 12px; background: rgba(16, 185, 129, 0.15); color: #34d399; font-weight: 700; border: 1px solid rgba(16, 185, 129, 0.3); }

    /* Live Virtual LED Preview Orb */
    .preview-bar {
      display: flex; align-items: center; justify-content: center; gap: 12px;
      padding: 14px 16px; background: rgba(18, 22, 36, 0.6); border-bottom: 1px solid var(--card-border);
    }
    .led-orb {
      width: 48px; height: 48px; border-radius: 50%;
      background: #ffffff;
      box-shadow: 0 0 24px rgba(255,255,255,0.7), inset 0 0 10px rgba(0,0,0,0.5);
      transition: background 0.04s ease, box-shadow 0.04s ease;
      border: 2px solid rgba(255,255,255,0.25);
    }
    .led-info { font-size: 0.8rem; }
    .led-info b { display: block; font-size: 0.9rem; color: #fff; }

    /* Navigation Tabs */
    .nav-tabs {
      display: flex; background: rgba(18, 22, 36, 0.95);
      border-bottom: 1px solid var(--card-border); padding: 4px; gap: 4px;
      position: sticky; top: 57px; z-index: 99;
    }
    .tab-btn {
      flex: 1; padding: 9px 4px; font-size: 0.72rem; font-weight: 700;
      background: transparent; color: var(--subtext); border: none; border-radius: 8px;
      cursor: pointer; transition: all 0.2s; text-align: center;
    }
    .tab-btn.active { background: var(--accent); color: #fff; box-shadow: 0 2px 8px var(--accent-glow); }

    /* Container & Card Layout */
    .container { max-width: 580px; margin: 0 auto; padding: 12px; }
    .tab-content { display: none; }
    .tab-content.active { display: block; }
    .card {
      background: var(--card-bg); border: 1px solid var(--card-border);
      border-radius: 14px; padding: 14px; margin-bottom: 12px;
      backdrop-filter: blur(8px);
    }
    .card-title {
      font-size: 0.8rem; font-weight: 800; text-transform: uppercase;
      letter-spacing: 0.05em; color: var(--subtext); margin-bottom: 10px;
      display: flex; align-items: center; justify-content: space-between;
    }

    /* Master Power & Brightness */
    .power-row { display: flex; gap: 8px; margin-bottom: 12px; }
    .btn-power {
      flex: 1; padding: 10px; border-radius: 10px; font-weight: 700; font-size: 0.8rem;
      border: none; cursor: pointer; display: flex; align-items: center; justify-content: center; gap: 6px;
    }
    .btn-off { background: rgba(239, 68, 68, 0.2); color: #fca5a5; border: 1px solid rgba(239, 68, 68, 0.3); }

    .slider-box { display: flex; flex-direction: column; gap: 6px; }
    .slider-header { display: flex; justify-content: space-between; font-size: 0.75rem; color: var(--subtext); }
    input[type=range] {
      -webkit-appearance: none; width: 100%; height: 7px; border-radius: 5px;
      background: #1e2538; outline: none;
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none; width: 18px; height: 18px; border-radius: 50%;
      background: var(--accent); cursor: pointer; box-shadow: 0 0 10px var(--accent);
    }

    /* 💡 Free Ambient Light (Solid Lamp) */
    .lamp-presets-grid {
      display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin-bottom: 10px;
    }
    .lamp-preset-btn {
      padding: 9px 4px; border-radius: 8px; border: 1px solid var(--card-border);
      background: rgba(255, 255, 255, 0.04); color: var(--text); font-size: 0.72rem;
      font-weight: 700; cursor: pointer; text-align: center; transition: all 0.15s;
    }
    .lamp-preset-btn:active { transform: scale(0.96); filter: brightness(1.2); }

    /* 📊 Real-Time Sound & Beat Monitor HUD */
    .hud-header {
      display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;
    }
    .btn-hud-stop {
      background: rgba(239, 68, 68, 0.2); color: #fca5a5; border: 1px solid rgba(239, 68, 68, 0.4);
      padding: 4px 12px; border-radius: 6px; font-size: 0.72rem; font-weight: 700; cursor: pointer;
    }
    .btn-hud-stop:hover { background: #ef4444; color: #fff; }

    #visualizer-canvas {
      width: 100%; height: 60px; background: #05070c; border-radius: 8px;
      border: 1px solid rgba(255, 255, 255, 0.08); display: block; margin-bottom: 8px;
    }
    .meter-container { display: flex; flex-direction: column; gap: 5px; }
    .meter-row { display: flex; align-items: center; gap: 8px; font-size: 0.7rem; }
    .meter-name { width: 95px; color: var(--subtext); font-weight: 600; }
    .meter-bar-bg { flex: 1; height: 6px; background: #131724; border-radius: 3px; overflow: hidden; }
    .meter-bar-fill { height: 100%; width: 0%; border-radius: 3px; transition: width 0.04s ease; }
    .meter-val { width: 36px; text-align: right; font-family: monospace; font-size: 0.68rem; color: #fff; }

    .beat-indicator-box {
      margin-top: 8px; padding: 6px 10px; border-radius: 6px;
      background: rgba(0, 0, 0, 0.3); border: 1px solid var(--card-border);
      display: flex; align-items: center; justify-content: space-between;
    }
    .beat-badge {
      font-size: 0.7rem; font-weight: 800; padding: 3px 8px; border-radius: 4px;
    }
    .beat-badge.normal { background: rgba(99, 102, 241, 0.2); color: #a5b4fc; }
    .beat-badge.peak { background: #ef4444; color: #fff; box-shadow: 0 0 10px #ef4444; animation: flashHit 0.25s ease-out; }
    .beat-badge.drop { background: rgba(100, 116, 139, 0.2); color: #94a3b8; }
    @keyframes flashHit { from { transform: scale(1.08); } to { transform: scale(1); } }

    /* 🎨 12 Music Light Modes & Moods */
    .music-modes-grid {
      display: grid; grid-template-columns: repeat(2, 1fr); gap: 6px; margin-top: 8px;
    }
    .music-mode-btn {
      padding: 8px; border-radius: 8px; border: 1px solid var(--card-border);
      background: rgba(255, 255, 255, 0.03); color: var(--text); font-size: 0.72rem;
      font-weight: 700; cursor: pointer; text-align: left; transition: all 0.15s;
    }
    .music-mode-btn.active {
      background: linear-gradient(135deg, rgba(99, 102, 241, 0.3), rgba(236, 72, 153, 0.3));
      border-color: var(--accent); color: #fff; box-shadow: 0 0 8px var(--accent-glow);
    }

    /* Earbuds Beat Engine */
    .sync-card { border: 1px solid rgba(99, 102, 241, 0.35); background: linear-gradient(180deg, rgba(30, 27, 75, 0.4), var(--card-bg)); }
    .btn-sync-toggle {
      width: 100%; padding: 12px; border-radius: 10px; font-weight: 800; font-size: 0.85rem;
      background: linear-gradient(135deg, var(--accent), var(--pink)); color: #fff;
      border: none; cursor: pointer; box-shadow: 0 4px 14px var(--accent-glow);
      display: flex; align-items: center; justify-content: center; gap: 8px; transition: transform 0.1s;
    }
    .btn-sync-toggle:active { transform: scale(0.98); }
    .btn-sync-toggle.active { background: linear-gradient(135deg, #10b981, #059669); box-shadow: 0 4px 14px rgba(16,185,129,0.4); }

    .genre-chips { display: flex; gap: 6px; flex-wrap: wrap; margin-top: 8px; }
    .genre-chip {
      font-size: 0.7rem; font-weight: 700; padding: 5px 10px; border-radius: 6px;
      background: rgba(255, 255, 255, 0.05); color: var(--subtext); border: 1px solid var(--card-border);
      cursor: pointer; transition: all 0.15s;
    }
    .genre-chip.active { background: rgba(99, 102, 241, 0.25); color: #c7d2fe; border-color: var(--accent); }

    .btn-tap {
      width: 100%; margin-top: 10px; padding: 10px; border-radius: 8px;
      background: rgba(255, 255, 255, 0.06); border: 1px dashed rgba(255, 255, 255, 0.2);
      color: var(--text); font-weight: 700; font-size: 0.75rem; cursor: pointer;
    }
    .btn-tap:active { background: rgba(99, 102, 241, 0.3); border-color: var(--accent); }

    /* Audio Sources & MP3 Player */
    .player-card { border: 1px solid rgba(6, 182, 212, 0.3); }
    .player-controls { display: flex; gap: 8px; margin-top: 6px; }
    .file-label, .btn-play-song {
      flex: 1; padding: 8px; border-radius: 8px; font-size: 0.72rem; font-weight: 700;
      text-align: center; cursor: pointer; border: 1px solid var(--card-border);
    }
    .file-label { background: rgba(6, 182, 212, 0.15); color: #67e8f9; }
    .btn-play-song { background: rgba(168, 85, 247, 0.15); color: #d8b4fe; }

    .btn-src {
      width: 100%; display: flex; align-items: center; gap: 10px; padding: 10px; border-radius: 8px;
      background: rgba(255, 255, 255, 0.03); border: 1px solid var(--card-border);
      color: var(--text); cursor: pointer; font-size: 0.75rem; text-align: left; margin-bottom: 6px;
    }
    .btn-src:hover { background: rgba(255, 255, 255, 0.07); }

    /* Gemini AI Chat */
    .gemini-card { border: 1px solid rgba(168, 85, 247, 0.3); background: linear-gradient(180deg, rgba(88, 28, 135, 0.2), var(--card-bg)); }
    .quick-chips { display: flex; gap: 5px; flex-wrap: wrap; margin-bottom: 8px; }
    .ai-chip {
      font-size: 0.68rem; padding: 3px 7px; border-radius: 5px;
      background: rgba(168, 85, 247, 0.15); color: #e9d5ff; border: 1px solid rgba(168, 85, 247, 0.25);
      cursor: pointer;
    }
    .ai-chat-box { display: flex; gap: 6px; }
    .ai-input {
      flex: 1; padding: 8px 10px; border-radius: 8px; border: 1px solid var(--card-border);
      background: #0b0e17; color: #fff; font-size: 0.78rem; outline: none;
    }
    .btn-gemini-send {
      padding: 8px 14px; border-radius: 8px; border: none; font-weight: 700; font-size: 0.78rem;
      background: linear-gradient(135deg, #a855f7, #6366f1); color: #fff; cursor: pointer;
    }
    .ai-bubble {
      margin-top: 8px; padding: 10px; border-radius: 8px;
      background: rgba(0, 0, 0, 0.4); border: 1px solid var(--card-border);
      font-size: 0.75rem; line-height: 1.4; display: none;
    }

    /* 20 Presets Grid */
    .preset-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 6px; }
    .preset-btn {
      padding: 9px 8px; border-radius: 8px; border: 1px solid var(--card-border);
      background: rgba(255, 255, 255, 0.03); color: var(--text); font-size: 0.72rem;
      font-weight: 600; cursor: pointer; text-align: left; transition: all 0.15s;
    }
    .preset-btn:hover { background: rgba(99, 102, 241, 0.2); border-color: var(--accent); }

    /* Custom Palette & Timeline */
    .palette-grid { display: grid; grid-template-columns: repeat(7, 1fr); gap: 6px; margin-bottom: 10px; }
    .swatch { height: 32px; border-radius: 6px; cursor: pointer; border: 1px solid rgba(255,255,255,0.15); }
    .timeline {
      display: flex; gap: 6px; overflow-x: auto; padding: 8px 0; min-height: 50px;
    }
    .timeline-item {
      flex: 0 0 54px; height: 42px; border-radius: 6px; position: relative;
      display: flex; flex-direction: column; align-items: center; justify-content: center;
      font-size: 0.65rem; font-weight: 700; color: #fff; text-shadow: 0 1px 2px #000;
    }
    .timeline-item .del {
      position: absolute; top: -4px; right: -4px; width: 14px; height: 14px;
      background: #ef4444; border-radius: 50%; font-size: 9px; line-height: 14px;
      text-align: center; cursor: pointer;
    }
    .form-row { display: flex; gap: 6px; margin-bottom: 8px; }
    .btn-add {
      width: 100%; padding: 8px; border-radius: 8px; font-weight: 700; font-size: 0.75rem;
      background: rgba(16, 185, 129, 0.2); color: #6ee7b7; border: 1px solid rgba(16, 185, 129, 0.3);
      cursor: pointer; margin-bottom: 6px;
    }
    .seq-actions { display: flex; gap: 6px; }
    .btn-play {
      flex: 2; padding: 9px; border-radius: 8px; font-weight: 700; font-size: 0.78rem;
      background: var(--accent); color: #fff; border: none; cursor: pointer;
    }
    .btn-clear {
      flex: 1; padding: 9px; border-radius: 8px; font-weight: 700; font-size: 0.78rem;
      background: rgba(239, 68, 68, 0.2); color: #fca5a5; border: 1px solid rgba(239, 68, 68, 0.3); cursor: pointer;
    }

    /* Modal */
    .modal-overlay {
      position: fixed; inset: 0; background: rgba(0,0,0,0.8); z-index: 1000;
      display: none; align-items: center; justify-content: center; padding: 16px;
    }
    .modal-box {
      background: #161b2c; border: 1px solid var(--accent); border-radius: 12px;
      padding: 16px; max-width: 440px; width: 100%;
    }
  </style>
</head>
<body>

<header>
  <div class="logo-group">
    <span style="font-size:1.3rem;">✨</span>
    <div>
      <h1 style="font-size:1.02rem; line-height:1.1;">ESP32 RGB PRO</h1>
      <div style="font-size:0.62rem; color:var(--subtext);">Bluetooth &bull; USB &bull; Wi-Fi</div>
    </div>
  </div>
  <div style="display:flex; gap:6px; align-items:center;">
    <span class="status-pill" id="bg-status-pill" style="background:rgba(16,185,129,0.15); color:#34d399; border-color:rgba(16,185,129,0.3); font-size:0.65rem;" title="Background Tab Audio Sync: Lights continue dancing when this tab is minimized or hidden!">⚡ Background Ready</span>
    <span class="status-pill" id="ble-status-pill" style="background:rgba(99,102,241,0.15); color:#a5b4fc; border-color:rgba(99,102,241,0.3);">⚪ BLE Ready</span>
    <button id="btn-ble-connect" onclick="toggleBluetoothConnect()" style="background:linear-gradient(135deg,#4f46e5,#06b6d4); color:#fff; border:none; border-radius:10px; font-size:0.68rem; font-weight:700; padding:6px 10px; cursor:pointer; display:flex; align-items:center; gap:4px; box-shadow:0 2px 8px rgba(79,70,229,0.35);">
      <span>🔵</span> Connect Bluetooth
    </button>
  </div>
</header>

<!-- Live Virtual LED Preview Orb & External 8-LED Bar -->
<div class="preview-bar" style="flex-direction:column; gap:8px;">
  <div style="display:flex; align-items:center; justify-content:center; gap:12px; width:100%;">
    <div class="led-orb" id="virtual-led"></div>
    <div class="led-info" style="flex:1;">
      <div style="display:flex; justify-content:space-between; align-items:center; width:100%;">
        <b id="active-mode-title">Mode: Aurora Borealis</b>
        <span class="status-pill" id="status-pill">● Mode 1 (ON)</span>
      </div>
      <span id="active-rgb-val" style="color:var(--subtext); font-size:0.75rem;">RGB(0, 0, 0)</span>
    </div>
  </div>

  <!-- 🚥 External 8-LED Bar Live HUD Strip -->
  <div class="ext-bar-hud" style="width:100%; display:flex; align-items:center; justify-content:space-between; background:rgba(0,0,0,0.4); padding:6px 12px; border-radius:8px; border:1px solid rgba(255,255,255,0.06);">
    <div style="display:flex; align-items:center; gap:6px;">
      <span style="font-size:0.68rem; font-weight:800; color:#38bdf8; text-transform:uppercase; letter-spacing:0.04em;">🚥 8-LED Bar:</span>
      <span id="ext-bar-mode-badge" class="status-pill" style="font-size:0.62rem; padding:2px 6px; background:rgba(56,189,248,0.15); color:#7dd3fc; border-color:rgba(56,189,248,0.3);">Mirror</span>
    </div>
    <div class="ext-leds-strip" style="display:flex; gap:6px; align-items:center;">
      <div class="ext-led-dot" id="ext-dot-0" title="LED 0 (D0 / GPIO 7)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
      <div class="ext-led-dot" id="ext-dot-1" title="LED 1 (D1 / GPIO 8)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
      <div class="ext-led-dot" id="ext-dot-2" title="LED 2 (D2 / GPIO 9)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
      <div class="ext-led-dot" id="ext-dot-3" title="LED 3 (D3 / GPIO 10)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
      <div class="ext-led-dot" id="ext-dot-4" title="LED 4 (D4 / GPIO 11)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
      <div class="ext-led-dot" id="ext-dot-5" title="LED 5 (D5 / GPIO 12)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
      <div class="ext-led-dot" id="ext-dot-6" title="LED 6 (D6 / GPIO 13)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
      <div class="ext-led-dot" id="ext-dot-7" title="LED 7 (D7 / GPIO 14)" style="width:13px; height:13px; border-radius:50%; background:#151926; border:1px solid #334155; transition:all 0.05s ease;"></div>
    </div>
  </div>
</div>

<!-- Navigation Tabs -->
<div class="nav-tabs">
  <button class="tab-btn active" onclick="switchTab('tab-sync', this)">🎧 Earbuds Sync</button>
  <button class="tab-btn" onclick="switchTab('tab-gemini', this)">🤖 Gemini AI</button>
  <button class="tab-btn" onclick="switchTab('tab-presets', this)">🎭 20 Presets</button>
  <button class="tab-btn" onclick="switchTab('tab-custom', this)">🛠️ Studio & Wi-Fi</button>
</div>

<div class="container">

  <!-- ==================== TAB 1: 🎧 EARBUDS & MUSIC SYNC ==================== -->
  <div id="tab-sync" class="tab-content active">

    <!-- Master Power & Brightness -->
    <div class="card">
      <div class="power-row" style="display:flex; gap:10px; margin-bottom:12px;">
        <button class="btn-power btn-on" onclick="turnLedOn()" style="flex:1; background:linear-gradient(135deg, #10b981, #059669); color:white; font-weight:700; border:none; padding:12px; border-radius:10px; cursor:pointer; font-size:0.95rem; box-shadow:0 0 15px rgba(16,185,129,0.35);">⏻ Turn LED ON</button>
        <button class="btn-power btn-off" onclick="setMode(0)" style="flex:1;">⏻ Turn LED OFF</button>
      </div>
      <div style="font-size:0.72rem; color:var(--subtext); margin-bottom:6px; font-weight:700;">⚡ Quick 1-Tap Moods (Instant ON):</div>
      <div style="display:grid; grid-template-columns:repeat(4, 1fr); gap:6px; margin-bottom:14px;">
        <button class="lamp-preset-btn" onclick="setMode(1)" style="border-color:#10b981; background:rgba(16,185,129,0.15);">🌌 Aurora</button>
        <button class="lamp-preset-btn" onclick="setMode(4)" style="border-color:#ec4899; background:rgba(236,72,153,0.15);">🌈 Rainbow</button>
        <button class="lamp-preset-btn" onclick="setMode(3)" style="border-color:#f59e0b; background:rgba(245,158,11,0.15);">🔥 Campfire</button>
        <button class="lamp-preset-btn" onclick="setLampColor(255,200,140)" style="border-color:#eab308; background:rgba(234,179,8,0.15);">💡 Cozy Lamp</button>
      </div>
      <div class="slider-box">
        <div class="slider-header">
          <span>Master Brightness (Punchy Output)</span>
          <span id="bright-val">100%</span>
        </div>
        <input type="range" id="bright-slider" min="0" max="100" value="100" oninput="onBrightness(this.value)">
      </div>
    </div>

    <!-- 🚥 EXTERNAL 8-LED RGB BAR (KEYES ARTOU V2) CONTROLLER -->
    <div class="card" style="border-color: rgba(56, 189, 248, 0.4); background: linear-gradient(180deg, rgba(14, 116, 144, 0.15), var(--card-bg));">
      <div class="card-title" style="color:#38bdf8; display:flex; justify-content:space-between; align-items:center; margin-bottom:10px;">
        <span>🚥 External 8-LED RGB Bar (ARTOU V2)</span>
        <span class="status-pill" id="ext-card-status" style="background:rgba(56,189,248,0.2); color:#38bdf8; border-color:rgba(56,189,248,0.4);">Active (125 FPS)</span>
      </div>

      <!-- Power & Mode Quick Toggle -->
      <div style="display:flex; gap:8px; margin-bottom:10px;">
        <button id="btn-ext-power" onclick="toggleExtPower()" style="flex:1; padding:9px; border-radius:8px; font-weight:800; font-size:0.75rem; border:none; cursor:pointer; background:linear-gradient(135deg, #0284c7, #06b6d4); color:#fff; box-shadow:0 0 10px rgba(6,182,212,0.35);">
          ⏻ Bar Power: ON
        </button>
        <button onclick="setExtBarMode(0)" class="lamp-preset-btn active" id="btn-quick-mirror" style="flex:1; padding:9px; border-radius:8px; font-weight:700; font-size:0.75rem; border-color:#38bdf8;">
          🔄 Mirror Main
        </button>
        <button onclick="setExtBarMode(1)" class="lamp-preset-btn" id="btn-quick-vu" style="flex:1; padding:9px; border-radius:8px; font-weight:700; font-size:0.75rem;">
          📊 8-LED VU Meter
        </button>
      </div>

      <!-- 6 Lighting Modes Grid -->
      <div style="font-size:0.7rem; color:var(--subtext); font-weight:700; margin-bottom:5px;">8-LED DISPLAY MODES:</div>
      <div style="display:grid; grid-template-columns:repeat(3, 1fr); gap:6px; margin-bottom:12px;">
        <button class="lamp-preset-btn active" id="ext-mode-btn-0" onclick="setExtBarMode(0)" style="border-color:#38bdf8;">🔄 Mirror Main</button>
        <button class="lamp-preset-btn" id="ext-mode-btn-1" onclick="setExtBarMode(1)">📊 Audio VU Meter</button>
        <button class="lamp-preset-btn" id="ext-mode-btn-2" onclick="setExtBarMode(2)">🌈 Spectrum Flow</button>
        <button class="lamp-preset-btn" id="ext-mode-btn-3" onclick="setExtBarMode(3)">⚡ Rainbow Wave</button>
        <button class="lamp-preset-btn" id="ext-mode-btn-4" onclick="setExtBarMode(4)">🏎️ Cyber Scanner</button>
        <button class="lamp-preset-btn" id="ext-mode-btn-5" onclick="setExtBarMode(5)">🔥 Campfire Embers</button>
      </div>

      <!-- Brightness Slider for External Bar -->
      <div class="slider-box" style="margin-bottom:10px;">
        <div class="slider-header">
          <span>Bar Brightness</span>
          <b id="ext-bright-val" style="color:#38bdf8;">100%</b>
        </div>
        <input type="range" id="ext-bright-slider" min="0" max="100" value="100" oninput="onExtBrightness(this.value)">
      </div>

      <!-- Quick Color Swatch Tests -->
      <div style="margin-bottom:10px;">
        <span style="font-size:0.68rem; color:var(--subtext); font-weight:700; display:block; margin-bottom:4px;">QUICK BAR COLOR TEST:</span>
        <div style="display:grid; grid-template-columns:repeat(5, 1fr); gap:5px;">
          <button onclick="testBarColor(255,0,0)" style="padding:6px 2px; border-radius:6px; background:rgba(239,68,68,0.2); border:1px solid #ef4444; color:#fca5a5; font-size:0.68rem; font-weight:700; cursor:pointer;">🔴 Red</button>
          <button onclick="testBarColor(0,255,0)" style="padding:6px 2px; border-radius:6px; background:rgba(16,185,129,0.2); border:1px solid #10b981; color:#6ee7b7; font-size:0.68rem; font-weight:700; cursor:pointer;">🟢 Green</button>
          <button onclick="testBarColor(0,100,255)" style="padding:6px 2px; border-radius:6px; background:rgba(6,182,212,0.2); border:1px solid #06b6d4; color:#67e8f9; font-size:0.68rem; font-weight:700; cursor:pointer;">🔵 Blue</button>
          <button onclick="testBarColor(255,255,255)" style="padding:6px 2px; border-radius:6px; background:rgba(255,255,255,0.15); border:1px solid #fff; color:#fff; font-size:0.68rem; font-weight:700; cursor:pointer;">⚪ White</button>
          <button onclick="testBarChaser()" style="padding:6px 2px; border-radius:6px; background:rgba(168,85,247,0.2); border:1px solid #a855f7; color:#d8b4fe; font-size:0.68rem; font-weight:700; cursor:pointer;">⚡ Chaser</button>
        </div>
      </div>

      <!-- Hardware Wiring & Pinout Guide (Collapsible) -->
      <details style="background:rgba(0,0,0,0.3); border:1px solid rgba(255,255,255,0.08); border-radius:8px; padding:8px 10px; font-size:0.72rem;">
        <summary style="font-weight:700; color:#38bdf8; cursor:pointer;">🔌 Module Wiring & Pinout Guide (Click to View)</summary>
        <div style="margin-top:8px; line-height:1.5; color:var(--subtext);">
          <div style="background:rgba(239,68,68,0.15); border:1px solid rgba(239,68,68,0.3); padding:6px 8px; border-radius:6px; color:#fca5a5; font-weight:700; margin-bottom:8px;">
            ⚠️ CRITICAL: Connect VCC to 3.3V (NOT 5V)! No GND wire is required.
          </div>
          <table style="width:100%; border-collapse:collapse; font-family:monospace; font-size:0.68rem; text-align:left; color:#f1f5f9;">
            <tr style="border-bottom:1px solid rgba(255,255,255,0.1); color:#38bdf8;">
              <th style="padding:4px;">Module Pin</th>
              <th style="padding:4px;">ESP32-S3 Pin</th>
              <th style="padding:4px;">Description</th>
            </tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);"><td style="padding:4px; font-weight:700; color:#f59e0b;">VCC</td><td style="padding:4px; font-weight:700; color:#10b981;">3.3V Pin</td><td>Module Power (MUST be 3.3V!)</td></tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);"><td style="padding:4px; color:#ef4444;">R (Cathode)</td><td style="padding:4px;">GPIO 4</td><td>Red Color Channel</td></tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);"><td style="padding:4px; color:#10b981;">G (Cathode)</td><td style="padding:4px;">GPIO 5</td><td>Green Color Channel</td></tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);"><td style="padding:4px; color:#38bdf8;">B (Cathode)</td><td style="padding:4px;">GPIO 6</td><td>Blue Color Channel</td></tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);"><td style="padding:4px;">D0 - D7</td><td style="padding:4px;">GPIO 7 - 14</td><td>Digit 0 to 7 (D0=7, D1=8, ..., D7=14)</td></tr>
          </table>
          <p style="margin-top:6px; font-size:0.65rem; color:#94a3b8;">
            * All 8 LEDs share the 330Ω onboard resistors. High-speed 125 FPS multiplexing keeps all LEDs at full brightness without flicker.
          </p>
        </div>
      </details>
    </div>

    <!-- 💡 FREE AMBIENT LIGHT (SOLID LAMP - CONTINUOUS ON) -->
    <div class="card" style="border: 1px solid rgba(245, 158, 11, 0.4); background: linear-gradient(180deg, rgba(120, 53, 15, 0.2), var(--card-bg));">
      <div class="card-title" style="color:#fde68a;">
        <span>💡 Free Ambient Light (Solid Lamp)</span>
        <span style="font-size:0.68rem; color:#f59e0b; font-weight:700;">STAYS 100% ON</span>
      </div>
      <div style="font-size:0.72rem; color:var(--subtext); margin-bottom:8px;">
        Solid steady glow without blinking or audio pulsing:
      </div>
      <div class="lamp-presets-grid">
        <button class="lamp-preset-btn" onclick="setLampColor(255, 140, 40)" style="background:rgba(255, 140, 40, 0.2); border-color:#ff9800;">🕯️ Candle</button>
        <button class="lamp-preset-btn" onclick="setLampColor(255, 205, 130)" style="background:rgba(255, 205, 130, 0.2); border-color:#f59e0b;">🛋️ 3000K Warm</button>
        <button class="lamp-preset-btn" onclick="setLampColor(220, 240, 255)" style="background:rgba(220, 240, 255, 0.2); border-color:#06b6d4;">❄️ 6000K Cool</button>
        <button class="lamp-preset-btn" onclick="setLampColor(255, 80, 20)" style="background:rgba(255, 80, 20, 0.2); border-color:#ef4444;">🌅 Sunset</button>
        <button class="lamp-preset-btn" onclick="setLampColor(255, 60, 150)" style="background:rgba(255, 60, 150, 0.2); border-color:#ec4899;">🌸 Pink</button>
        <button class="lamp-preset-btn" onclick="setLampColor(0, 200, 255)" style="background:rgba(0, 200, 255, 0.2); border-color:#0284c7;">🌊 Ocean</button>
        <button class="lamp-preset-btn" onclick="setLampColor(0, 255, 120)" style="background:rgba(0, 255, 120, 0.2); border-color:#10b981;">🌿 Emerald</button>
        <button class="lamp-preset-btn" onclick="setLampColor(180, 0, 255)" style="background:rgba(180, 0, 255, 0.2); border-color:#a855f7;">🔮 Purple</button>
      </div>
      <div style="display:flex; justify-content:space-between; align-items:center; background:rgba(0,0,0,0.3); padding:6px 10px; border-radius:8px; border:1px solid var(--card-border);">
        <span style="font-size:0.75rem; color:var(--text); font-weight:700;">Custom Free Lamp Color:</span>
        <div style="display:flex; align-items:center; gap:8px;">
          <input type="color" id="lamp-free-picker" value="#ffc882" oninput="onLampHexColor(this.value)" style="border:none; width:36px; height:28px; border-radius:6px; cursor:pointer;">
          <span id="lamp-hex-display" style="font-size:0.75rem; font-family:monospace; color:var(--subtext);">#FFC882</span>
        </div>
      </div>
    </div>

    <!-- 📊 REAL-TIME SOUND & BEAT MONITOR HUD -->
    <div class="card" style="border-color: rgba(99, 102, 241, 0.35);">
      <div class="hud-header">
        <div class="card-title" style="margin-bottom:0;">📊 Real-Time Sound & Beat Monitor</div>
        <button class="btn-hud-stop" onclick="stopAllAudio()">⏹️ STOP</button>
      </div>
      <div style="font-size:0.72rem; color:var(--subtext); margin-bottom:6px;" id="hud-status">
        Ready • Start Earbuds Beat or Play Song below
      </div>

      <canvas id="visualizer-canvas"></canvas>

      <!-- 4-Band Multi-Instrument Spectrum Meters -->
      <div class="meter-container">
        <div class="meter-row">
          <div class="meter-name">🥁 Kick / Bass</div>
          <div class="meter-bar-bg"><div class="meter-bar-fill" id="meter-bass" style="background:#ef4444;"></div></div>
          <div class="meter-val" id="val-bass">0%</div>
        </div>
        <div class="meter-row">
          <div class="meter-name">🎙️ Vocals & Lead</div>
          <div class="meter-bar-bg"><div class="meter-bar-fill" id="meter-vocals" style="background:#ec4899;"></div></div>
          <div class="meter-val" id="val-vocals">0%</div>
        </div>
        <div class="meter-row">
          <div class="meter-name">🎸 Rhythm Synths</div>
          <div class="meter-bar-bg"><div class="meter-bar-fill" id="meter-mids" style="background:#06b6d4;"></div></div>
          <div class="meter-val" id="val-mids">0%</div>
        </div>
        <div class="meter-row">
          <div class="meter-name">🎺 Snare / Treble</div>
          <div class="meter-bar-bg"><div class="meter-bar-fill" id="meter-treble" style="background:#f59e0b;"></div></div>
          <div class="meter-val" id="val-treble">0%</div>
        </div>
      </div>

      <!-- Beat Status, Auto Feeling, Spike Phase & Pitch Note HUD -->
      <div class="beat-indicator-box" style="flex-wrap:wrap; gap:8px;">
        <div style="flex:1; min-width:140px;">
          <span style="font-size:0.68rem; color:var(--subtext); display:block; margin-bottom:2px;">BEAT & SPIKE STATUS:</span>
          <div class="beat-badge normal" id="beat-status-badge">Waiting for sound...</div>
          <div style="margin-top:4px; display:flex; gap:4px; flex-wrap:wrap;">
            <span class="status-pill" id="auto-feeling-badge" style="font-size:0.65rem; background:rgba(236,72,153,0.15); color:#f472b6; border-color:rgba(236,72,153,0.3);">🎭 Vibe: Adaptive</span>
            <span class="status-pill" id="live-pitch-note" style="font-size:0.65rem; background:rgba(6,182,212,0.15); color:#67e8f9; border-color:rgba(6,182,212,0.3);">🎵 Note: --</span>
          </div>
        </div>
        <div style="text-align:right;">
          <span style="font-size:0.68rem; color:var(--subtext); display:block; margin-bottom:2px;">ACTIVE COLOR:</span>
          <div style="display:flex; align-items:center; justify-content:flex-end; gap:6px; margin-top:2px;">
            <div id="live-color-chip" style="width:16px; height:16px; border-radius:50%; background:#6366f1; border:1px solid #fff;"></div>
            <span id="live-color-hex" style="font-size:0.75rem; font-weight:700;">#6366F1</span>
          </div>
          <div id="spike-phase-badge" style="font-size:0.65rem; color:#a5b4fc; margin-top:3px; font-weight:700;">⚡ Spike: Normal</div>
        </div>
      </div>

      <!-- ⏱️ Audio Timing Tuning (Earbuds Bluetooth Latency Compensation) -->
      <div class="slider-box" style="margin-top:10px;">
        <div class="slider-header">
          <span>⏱️ Earbuds Audio Sync Tuning (Offset)</span>
          <b id="offset-val" style="color:var(--cyan);">0 ms (Instant)</b>
        </div>
        <input type="range" id="offset-slider" min="-150" max="150" value="0" step="10" oninput="onOffsetChange(this.value)">
      </div>

      <!-- ⏱️ Beat Speed & Pacing -->
      <div style="margin-top:8px;">
        <span style="font-size:0.7rem; color:var(--subtext); font-weight:700;">⏱️ BEAT PACING (Steady & Musical Rhythm):</span>
        <div class="genre-chips" style="margin-top:4px;">
          <span class="genre-chip active" id="pace-clean" onclick="setPace('clean')">🥁 Clean Beat (Steady ~120 BPM)</span>
          <span class="genre-chip" id="pace-fast" onclick="setPace('fast')">⚡ Fast Club (180 BPM)</span>
          <span class="genre-chip" id="pace-chill" onclick="setPace('chill')">🧘 Mellow Pulse (80 BPM)</span>
        </div>
      </div>

      <!-- 🎨 13 DIVERSE MUSIC LIGHT MODES + FREE CUSTOM COLOR -->
      <div style="margin-top:10px;">
        <span style="font-size:0.7rem; color:var(--subtext); font-weight:700;">🎨 MUSIC LIGHT MODES (13 MOODS + FREE CUSTOM COLOR):</span>
        <div class="music-modes-grid">
          <button class="music-mode-btn active" id="mode-cyber" onclick="setMusicMode('cyber')">🌆 Cyberpunk</button>
          <button class="music-mode-btn" id="mode-autofeeling" onclick="setMusicMode('autofeeling')" style="background:linear-gradient(135deg, rgba(236,72,153,0.25), rgba(168,85,247,0.25)); border-color:#ec4899; color:#fce7f3;">🎭 Auto Feeling (AI Vibe)</button>
          <button class="music-mode-btn" id="mode-fire" onclick="setMusicMode('fire')">🔥 Fire & Bass</button>
          <button class="music-mode-btn" id="mode-disco" onclick="setMusicMode('disco')">🪩 Disco Party</button>
          <button class="music-mode-btn" id="mode-edm" onclick="setMusicMode('edm')">⚡ EDM White Burst</button>
          <button class="music-mode-btn" id="mode-thrill" onclick="setMusicMode('thrill')">🦇 Thriller Red</button>
          <button class="music-mode-btn" id="mode-zen" onclick="setMusicMode('zen')">🧘 Zen Ambient</button>
          <button class="music-mode-btn" id="mode-pitch" onclick="setMusicMode('pitch')">🌈 Chroma Pitch</button>
          <button class="music-mode-btn" id="mode-ocean" onclick="setMusicMode('ocean')">🌊 Deep Ocean</button>
          <button class="music-mode-btn" id="mode-acid" onclick="setMusicMode('acid')">🎆 Acid Rave</button>
          <button class="music-mode-btn" id="mode-volcano" onclick="setMusicMode('volcano')">🌋 Volcano Lava</button>
          <button class="music-mode-btn" id="mode-gold" onclick="setMusicMode('gold')">💡 Gold Acoustic</button>
          <button class="music-mode-btn" id="mode-ice" onclick="setMusicMode('ice')">❄️ Glacier Ice</button>
          <button class="music-mode-btn" id="mode-ai" onclick="setMusicMode('ai')" style="background:linear-gradient(135deg, rgba(16,185,129,0.25), rgba(99,102,241,0.25)); border-color:#34d399; color:#a7f3d0; font-weight:700;">✨ Gemini AI Director</button>
          <button class="music-mode-btn" id="mode-free" onclick="setMusicMode('free')" style="background:linear-gradient(135deg, rgba(236,72,153,0.25), rgba(99,102,241,0.25)); border-color:var(--accent);">🎨 Free Custom Beat Color</button>
        </div>
      </div>

      <!-- 🎨 FREE MUSIC BEAT COLOR PICKER & QUICK SWATCHES -->
      <div id="free-music-color-panel" style="margin-top:10px; padding:10px; background:rgba(255,255,255,0.03); border:1px solid rgba(255,255,255,0.08); border-radius:10px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
          <span style="font-size:0.75rem; font-weight:700; color:var(--accent);">🎨 Free Music Beat Color</span>
          <div style="display:flex; align-items:center; gap:6px;">
            <input type="color" id="free-music-picker" value="#00f5ff" oninput="setFreeMusicColor(this.value)" style="border:none; width:34px; height:26px; border-radius:6px; cursor:pointer;">
            <span id="free-color-preview-hex" style="font-size:0.72rem; font-weight:700; color:var(--text);">#00F5FF</span>
          </div>
        </div>
        <!-- Quick 1-touch Color Swatches -->
        <div style="display:grid; grid-template-columns:repeat(8, 1fr); gap:5px; margin-bottom:8px;">
          <div onclick="setFreeMusicColor('#ff0055')" style="height:22px; border-radius:4px; background:#ff0055; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Neon Pink"></div>
          <div onclick="setFreeMusicColor('#ff4500')" style="height:22px; border-radius:4px; background:#ff4500; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Orange Flame"></div>
          <div onclick="setFreeMusicColor('#ffcc00')" style="height:22px; border-radius:4px; background:#ffcc00; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Warm Gold"></div>
          <div onclick="setFreeMusicColor('#00ff66')" style="height:22px; border-radius:4px; background:#00ff66; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Lime Neon"></div>
          <div onclick="setFreeMusicColor('#00f5ff')" style="height:22px; border-radius:4px; background:#00f5ff; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Electric Cyan"></div>
          <div onclick="setFreeMusicColor('#0066ff')" style="height:22px; border-radius:4px; background:#0066ff; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Royal Blue"></div>
          <div onclick="setFreeMusicColor('#a855f7')" style="height:22px; border-radius:4px; background:#a855f7; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Deep Purple"></div>
          <div onclick="setFreeMusicColor('#ffffff')" style="height:22px; border-radius:4px; background:#ffffff; cursor:pointer; border:1px solid rgba(255,255,255,0.2);" title="Pure Strobe White"></div>
        </div>
        <div style="display:flex; justify-content:space-between; align-items:center; font-size:0.7rem; color:var(--subtext);">
          <span>⚡ White Kick Accent:</span>
          <label style="display:flex; align-items:center; gap:4px; cursor:pointer;">
            <input type="checkbox" id="chk-free-white-kick" checked>
            <span style="color:var(--text);">Flash white on heavy kick drops</span>
          </label>
        </div>
      </div>
    </div>

    <!-- 🧠 GEMINI AI MUSIC DIRECTOR (INTELLIGENT EMOTIONAL LIGHTING) -->
    <div class="card" id="gemini-ai-card" style="border-color: rgba(52, 211, 153, 0.4); background: linear-gradient(180deg, rgba(6, 78, 59, 0.25), var(--card-bg));">
      <div class="card-title" style="color:#34d399; display:flex; justify-content:space-between; align-items:center;">
        <div style="display:flex; align-items:center; gap:8px;">
          <span>✨ Gemini AI Music Director</span>
          <span class="status-pill" id="ai-director-status-pill" style="background:rgba(16,185,129,0.2); color:#34d399; border-color:rgba(16,185,129,0.4); font-size:0.65rem;">🟢 AI READY</span>
        </div>
        <button class="music-mode-btn" id="btn-activate-ai-mode" onclick="setMusicMode('ai')" style="font-size:0.7rem; padding:4px 10px; border-color:#34d399; background:rgba(16,185,129,0.2); color:#a7f3d0;">
          ▶ Engage AI Mode
        </button>
      </div>

      <div style="font-size:0.72rem; color:var(--subtext); margin-bottom:10px;">
        Gemini decodes musical context, momentum, and tension/drop evolution to dictate high-level emotion, scene, and color palette, while your existing RGB engine executes real-time 60 FPS hardware reactions.
      </div>

      <!-- Live AI Emotional Intelligence HUD -->
      <div style="padding:10px; background:rgba(0,0,0,0.3); border-radius:10px; border:1px solid rgba(52,211,153,0.2); margin-bottom:12px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px; flex-wrap:wrap; gap:6px;">
          <div style="display:flex; align-items:center; gap:6px;">
            <span style="font-weight:700; color:#6ee7b7; font-size:0.75rem;">🎭 Vibe & Mood:</span>
            <b id="ai-mood-display" style="color:#ffffff; font-size:0.8rem; background:rgba(255,255,255,0.08); padding:2px 8px; border-radius:6px;">Futuristic • Excited</b>
          </div>
          <div style="display:flex; align-items:center; gap:6px;">
            <span class="beat-badge peak" id="ai-event-badge" style="font-size:0.65rem; padding:2px 6px;">⚡ BUILD_UP</span>
            <span id="ai-confidence-pill" style="font-size:0.68rem; color:#94a3b8;">Confidence: 95%</span>
          </div>
        </div>

        <div style="font-size:0.75rem; color:#e2e8f0; font-style:italic; margin-bottom:8px;" id="ai-scene-description">
          "Futuristic neon pulses of cyan, magenta, and purple reacting intensely to rising energy."
        </div>

        <!-- 3-Color Dynamic Palette Swatch -->
        <div style="display:flex; align-items:center; gap:8px;">
          <span style="font-size:0.68rem; color:var(--subtext); font-weight:700;">AI Emotional Palette:</span>
          <div style="display:flex; gap:6px; flex:1;">
            <div id="ai-color-swatch-0" style="flex:1; height:20px; border-radius:6px; background:#00F3FF; box-shadow:0 0 8px rgba(0,243,255,0.5); display:flex; align-items:center; justify-content:center; font-size:0.6rem; color:#000; font-weight:bold;">#00F3FF</div>
            <div id="ai-color-swatch-1" style="flex:1; height:20px; border-radius:6px; background:#FF007F; box-shadow:0 0 8px rgba(255,0,127,0.5); display:flex; align-items:center; justify-content:center; font-size:0.6rem; color:#fff; font-weight:bold;">#FF007F</div>
            <div id="ai-color-swatch-2" style="flex:1; height:20px; border-radius:6px; background:#7B00FF; box-shadow:0 0 8px rgba(123,0,255,0.5); display:flex; align-items:center; justify-content:center; font-size:0.6rem; color:#fff; font-weight:bold;">#7B00FF</div>
          </div>
        </div>
      </div>

      <!-- Natural-Language User Command Input -->
      <div style="margin-bottom:8px;">
        <span style="font-size:0.7rem; color:#a7f3d0; font-weight:700;">💬 Natural-Language Style Commands (Direct Gemini):</span>
        <div style="display:flex; gap:6px; margin-top:4px;">
          <input type="text" id="ai-user-prompt-input" placeholder="e.g. 'Make it darker', 'Feel the bass more', 'Cinematic sunset'..." 
                 style="flex:1; padding:8px 12px; background:rgba(0,0,0,0.4); border:1px solid rgba(52,211,153,0.3); border-radius:8px; color:#fff; font-size:0.75rem;"
                 onkeydown="if(event.key==='Enter') sendUserAiCommand()">
          <button onclick="sendUserAiCommand()" style="padding:8px 14px; background:linear-gradient(135deg, #10b981, #059669); border:none; border-radius:8px; color:#fff; font-weight:700; font-size:0.75rem; cursor:pointer;">
            Ask AI
          </button>
        </div>
      </div>

      <!-- Quick AI Style Chips -->
      <div style="display:flex; gap:5px; flex-wrap:wrap; margin-bottom:8px;">
        <span class="genre-chip" onclick="sendUserAiCommand('Make it darker and moody')">🌙 Darker</span>
        <span class="genre-chip" onclick="sendUserAiCommand('Make it emotional and melodic')">💖 Emotional</span>
        <span class="genre-chip" onclick="sendUserAiCommand('Feel the heavy bass and kick drops')">🥁 Heavy Bass</span>
        <span class="genre-chip" onclick="sendUserAiCommand('Make it cinematic like a movie score')">🎬 Cinematic</span>
        <span class="genre-chip" onclick="sendUserAiCommand('Make it futuristic cyberpunk rave')">🚀 Futuristic</span>
        <span class="genre-chip" onclick="sendUserAiCommand('Surprise me with something creative')">🎲 Surprise Me</span>
        <span class="genre-chip" onclick="sendUserAiCommand('Follow the music naturally')">🌊 Natural Flow</span>
      </div>

      <div style="display:flex; justify-content:space-between; align-items:center; font-size:0.7rem; color:var(--subtext);">
        <label style="display:flex; align-items:center; gap:5px; cursor:pointer;">
          <input type="checkbox" id="chk-ai-auto-evolve" checked>
          <span>Auto-evolve lighting on drops & section transitions</span>
        </label>
        <span id="ai-last-sync-time" style="color:#6ee7b7; font-size:0.68rem;">Updated just now</span>
      </div>
    </div>

    <!-- 🎛️ DYNAMIC AUDIO CUSTOMIZATION STUDIO (BASS, VOCALS, RANGE, SPIKE & CHROMA) -->
    <div class="card" style="border-color: rgba(236, 72, 153, 0.35); background: linear-gradient(180deg, rgba(80, 7, 36, 0.25), var(--card-bg));">
      <div class="card-title" style="color:#f472b6;">
        <span>🎛️ Audio Customization Studio</span>
        <span class="status-pill" id="studio-master-status" style="background:rgba(16,185,129,0.15); color:#34d399; border-color:rgba(16,185,129,0.3); font-size:0.65rem;">● CUSTOM ACTIVE</span>
      </div>

      <!-- MASTER ON/OFF BUTTON FOR CUSTOMIZE -->
      <button class="btn-sync-toggle active" id="btn-studio-master" onclick="toggleStudioMaster()" style="margin-bottom:12px; background:linear-gradient(135deg, #10b981, #059669); font-size:0.8rem; padding:10px;">
        <span id="studio-master-icon">🎛️</span>
        <span id="studio-master-text">CUSTOM STUDIO ENGINE: ON (Click to Bypass)</span>
      </button>

      <!-- 🥁 1. BASS & SUB-BASS STUDIO (CUSTOMIZABLE BASS) -->
      <div style="margin-bottom:12px; padding:10px; background:rgba(0,0,0,0.25); border-radius:10px; border:1px solid var(--card-border);">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px;">
          <span style="font-weight:700; color:#ef4444; font-size:0.75rem;">🥁 Custom Bass & Sub-Bass Tuning</span>
          <label style="display:flex; align-items:center; gap:4px; font-size:0.7rem; color:var(--text); cursor:pointer;">
            <input type="checkbox" id="chk-bass-enable" checked onchange="onBassEnableChange(this.checked)">
            <span style="color:#fca5a5; font-weight:700;">Active</span>
          </label>
        </div>
        <div class="slider-header" style="margin-bottom:6px;">
          <span style="color:var(--subtext); font-size:0.7rem;">Bass Weight (0.5x - 3.5x Kick Multiplier):</span>
          <b id="bass-weight-display" style="color:#ef4444;">1.5x (Punchy Sub-Bass)</b>
        </div>
        <input type="range" id="bass-weight-slider" min="5" max="35" value="15" step="1" oninput="onBassWeightChange(this.value)">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px; font-size:0.72rem;">
          <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
            <input type="checkbox" id="chk-bass-boost" onchange="onBassBoostChange(this.checked)">
            <span style="color:#fca5a5; font-weight:700;">⚡ 808 Sub-Bass Punch (&lt;90Hz extra depth)</span>
          </label>
          <span style="color:var(--subtext); font-size:0.68rem;">Deep kick resonance</span>
        </div>
      </div>

      <!-- 🎙️ 2. VOCAL WEIGHT & LEAD SINGER ISOLATOR -->
      <div style="margin-bottom:12px; padding:10px; background:rgba(0,0,0,0.25); border-radius:10px; border:1px solid var(--card-border);">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px;">
          <span style="font-weight:700; color:#f472b6; font-size:0.75rem;">🎙️ Custom Vocal Tuning</span>
          <label style="display:flex; align-items:center; gap:4px; font-size:0.7rem; color:var(--text); cursor:pointer;">
            <input type="checkbox" id="chk-vocal-enable" checked onchange="onVocalEnableChange(this.checked)">
            <span style="color:#fce7f3; font-weight:700;">Active</span>
          </label>
        </div>
        <div class="slider-header" style="margin-bottom:6px;">
          <span style="color:var(--subtext); font-size:0.7rem;">Vocal Weight (0.5x - 3.5x Voice Prominence):</span>
          <b id="vocal-weight-display" style="color:#f472b6;">1.8x (Punchy Vocals)</b>
        </div>
        <input type="range" id="vocal-weight-slider" min="5" max="35" value="18" step="1" oninput="onVocalWeightChange(this.value)">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px; font-size:0.72rem;">
          <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
            <input type="checkbox" id="chk-vocal-spotlight" onchange="onVocalSpotlightChange(this.checked)">
            <span style="color:#fce7f3; font-weight:700;">🌟 Vocalist Spotlight Mode</span>
          </label>
          <span style="color:var(--subtext); font-size:0.68rem;">Singer voice drives light bloom</span>
        </div>
      </div>

      <!-- 📈 3. DYNAMIC BRIGHTNESS RANGE (FLOOR & CEILING) -->
      <div style="margin-bottom:12px; padding:10px; background:rgba(0,0,0,0.25); border-radius:10px; border:1px solid var(--card-border);">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px;">
          <span style="font-weight:700; color:#38bdf8; font-size:0.75rem;">📈 Song Reaction Brightness Range</span>
          <div style="display:flex; align-items:center; gap:6px;">
            <b id="range-summary-display" style="color:#38bdf8; font-size:0.75rem;">0% - 100%</b>
            <label style="display:flex; align-items:center; gap:4px; font-size:0.7rem; color:var(--text); cursor:pointer;">
              <input type="checkbox" id="chk-range-enable" checked onchange="onRangeEnableChange(this.checked)">
              <span style="color:#bae6fd; font-weight:700;">Active</span>
            </label>
          </div>
        </div>
        <div style="font-size:0.7rem; color:var(--subtext); margin-bottom:8px;">
          Set minimum ambient low and maximum peak spike (e.g. <b>70% to 98%</b>):
        </div>

        <!-- Live Range Gauge Visualizer Bar -->
        <div style="position:relative; width:100%; height:14px; background:#111827; border-radius:7px; overflow:hidden; border:1px solid rgba(255,255,255,0.1); margin-bottom:10px;">
          <div id="range-active-window" style="position:absolute; left:0%; width:100%; height:100%; background:linear-gradient(90deg, rgba(56,189,248,0.3), rgba(236,72,153,0.4)); border-left:2px solid #38bdf8; border-right:2px solid #ec4899;"></div>
          <div id="range-needle" style="position:absolute; left:0%; top:0; width:4px; height:100%; background:#fff; box-shadow:0 0 8px #fff; transition:left 0.04s ease;"></div>
        </div>

        <div style="display:grid; grid-template-columns:1fr 1fr; gap:10px;">
          <div class="slider-box">
            <div class="slider-header">
              <span>Low Floor:</span>
              <b id="range-floor-display" style="color:#38bdf8;">0%</b>
            </div>
            <input type="range" id="range-floor-slider" min="0" max="90" value="0" step="1" oninput="onRangeFloorChange(this.value)">
          </div>
          <div class="slider-box">
            <div class="slider-header">
              <span>High Ceiling:</span>
              <b id="range-ceiling-display" style="color:#ec4899;">100%</b>
            </div>
            <input type="range" id="range-ceiling-slider" min="40" max="100" value="100" step="1" oninput="onRangeCeilingChange(this.value)">
          </div>
        </div>

        <!-- 1-Tap Quick Range Presets -->
        <div style="display:flex; gap:6px; flex-wrap:wrap; margin-top:8px;">
          <span class="genre-chip active" id="range-chip-full" onclick="setRangePreset(0, 100)">Full Range (0% - 100%)</span>
          <span class="genre-chip" id="range-chip-user" onclick="setRangePreset(70, 98)">⭐ Custom (70% - 98%)</span>
          <span class="genre-chip" id="range-chip-night" onclick="setRangePreset(40, 85)">🌙 Night (40% - 85%)</span>
          <span class="genre-chip" id="range-chip-club" onclick="setRangePreset(20, 100)">⚡ Club (20% - 100%)</span>
        </div>

        <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px; font-size:0.7rem; color:var(--subtext);">
          <span>Maintain floor glow during song pause:</span>
          <label style="display:flex; align-items:center; gap:4px; cursor:pointer;">
            <input type="checkbox" id="chk-keep-floor-silence" onchange="onKeepFloorSilenceChange(this.checked)">
            <span style="color:var(--text);">Keep Ambient Floor</span>
          </label>
        </div>
      </div>

      <!-- ⚡ 4. 3-STAGE SPIKE DYNAMICS & MULTICOLOUR PROTECTION -->
      <div style="margin-bottom:12px; padding:10px; background:rgba(0,0,0,0.25); border-radius:10px; border:1px solid var(--card-border);">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:4px;">
          <span style="font-weight:700; color:#fbbf24; font-size:0.75rem;">⚡ 3-Stage Spike Dynamics</span>
          <label style="display:flex; align-items:center; gap:4px; font-size:0.7rem; color:var(--text); cursor:pointer;">
            <input type="checkbox" id="chk-spike-enable" checked onchange="onSpikeEnableChange(this.checked)">
            <span style="color:#fde68a; font-weight:700;">Active</span>
          </label>
        </div>
        <div style="font-size:0.7rem; color:var(--subtext); margin-bottom:8px;">
          Tension build-up radar, climax explosive strike, and customizable harmonic decay:
        </div>

        <div style="display:flex; flex-direction:column; gap:6px; font-size:0.72rem; margin-bottom:8px;">
          <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
            <input type="checkbox" id="chk-spike-anticipation" checked onchange="onSpikeAnticipationChange(this.checked)">
            <span style="color:#fde68a;"><b>Stage 1 (Before Spike):</b> Tension Radar Shimmer (Pre-drop build-up pulse)</span>
          </label>
          <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
            <input type="checkbox" id="chk-spike-force-multicolor" checked onchange="onSpikeMulticolorChange(this.checked)">
            <span style="color:#67e8f9; font-weight:700;"><b>Stage 2 (During Spike):</b> 🎨 Rich Multicolor Priority (Keeps colors alive on beats)</span>
          </label>
          <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
            <input type="checkbox" id="chk-spike-white-strobe" onchange="onSpikeWhiteStrobeChange(this.checked)">
            <span style="color:#ffffff;">✨ Rare Strobe White on Extreme Climax Only (&gt;95% mega drops)</span>
          </label>
        </div>

        <div style="font-size:0.7rem; color:#cbd5e1; font-weight:700; margin-bottom:4px;">
          Stage 3 (After Spike Reaction - Reverb Tail Curve):
        </div>
        <div class="genre-chips" style="margin-top:2px;">
          <span class="genre-chip active" id="spike-curve-harmonic" onclick="setSpikeCurve('harmonic')">🌊 Harmonic Reverb</span>
          <span class="genre-chip" id="spike-curve-snappy" onclick="setSpikeCurve('snappy')">⚡ Cyber Snappy</span>
          <span class="genre-chip" id="spike-curve-ripple" onclick="setSpikeCurve('ripple')">💫 Twin Echo Ripple</span>
          <span class="genre-chip" id="spike-curve-bloom" onclick="setSpikeCurve('bloom')">🌸 Vocal Bloom</span>
        </div>
      </div>

      <!-- 🌈 5. CHROMA PITCH & MULTICOLOUR STUDIO -->
      <div style="padding:10px; background:rgba(0,0,0,0.25); border-radius:10px; border:1px solid var(--card-border);">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px;">
          <span style="font-weight:700; color:#a78bfa; font-size:0.75rem;">🌈 Chroma Pitch & Multicolor Studio</span>
          <div style="display:flex; align-items:center; gap:6px;">
            <span id="chroma-note-live" class="status-pill" style="background:rgba(167,139,250,0.2); color:#c4b5fd; border-color:rgba(167,139,250,0.4); font-size:0.68rem;">🎵 A4 • 440 Hz</span>
            <label style="display:flex; align-items:center; gap:4px; font-size:0.7rem; color:var(--text); cursor:pointer;">
              <input type="checkbox" id="chk-chroma-enable" checked onchange="onChromaEnableChange(this.checked)">
              <span style="color:#ddd6fe; font-weight:700;">Active</span>
            </label>
          </div>
        </div>
        <div style="font-size:0.7rem; color:var(--subtext); margin-bottom:8px;">
          Maps every note & melody to rich multicolors, including pure white diamond sparkle:
        </div>

        <div style="margin-bottom:8px;">
          <label style="display:flex; align-items:center; gap:6px; font-size:0.72rem; cursor:pointer;">
            <input type="checkbox" id="chk-chroma-white-sparkle" checked onchange="onChromaWhiteSparkleChange(this.checked)">
            <span style="color:#ffffff; font-weight:700;">✨ Include Diamond White Accents with Multicolors (Treble & Claps)</span>
          </label>
        </div>

        <div style="display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-bottom:8px;">
          <div class="slider-box">
            <div class="slider-header">
              <span>Root Hue Offset:</span>
              <b id="chroma-offset-display" style="color:#c4b5fd;">0° (C = Red)</b>
            </div>
            <input type="range" id="chroma-offset-slider" min="0" max="360" value="0" step="5" oninput="onChromaOffsetChange(this.value)">
          </div>
          <div class="slider-box">
            <div class="slider-header">
              <span>Octave Spread:</span>
              <b id="chroma-spread-display" style="color:#c4b5fd;">1.0x Normal</b>
            </div>
            <input type="range" id="chroma-spread-slider" min="5" max="25" value="10" step="1" oninput="onChromaSpreadChange(this.value)">
          </div>
        </div>

        <div style="font-size:0.7rem; color:#cbd5e1; font-weight:700; margin-bottom:4px;">
          Chroma Color Palette Themes:
        </div>
        <div class="genre-chips" style="margin-top:2px;">
          <span class="genre-chip active" id="chroma-pal-flow" onclick="setChromaPalette('flow')">🌈 Multicolor Flow (Rainbow + White)</span>
          <span class="genre-chip" id="chroma-pal-vocal_spectrum" onclick="setChromaPalette('vocal_spectrum')">🎙️ Deep Violet & Vocal Spectrum</span>
          <span class="genre-chip" id="chroma-pal-spectrum" onclick="setChromaPalette('spectrum')">🎼 Full 360° Spectrum</span>
          <span class="genre-chip" id="chroma-pal-cyber" onclick="setChromaPalette('cyber')">🌆 Cyber Neon</span>
          <span class="genre-chip" id="chroma-pal-sunset" onclick="setChromaPalette('sunset')">🌅 Sunset Heat</span>
          <span class="genre-chip" id="chroma-pal-fifths" onclick="setChromaPalette('fifths')">🎹 Circle of Fifths</span>
        </div>
      </div>
    </div>

    <!-- 🎧 EARBUDS BEAT SYNC ENGINE (Zero Lag, Works Without Mic) -->
    <div class="card sync-card">
      <div class="card-title">🎧 Earbuds Beat Sync Engine (Spotify / BT Earbuds)</div>
      <button class="btn-sync-toggle" id="btn-sync-toggle" onclick="toggleEarbudsSync()">
        <span id="sync-icon">▶</span>
        <span id="sync-text">START EARBUDS BEAT: ON</span>
      </button>

      <!-- Genre Rhythms -->
      <div class="genre-chips">
        <span class="genre-chip active" id="chip-edm" onclick="setGenre('edm')">🪩 4/4 EDM Club</span>
        <span class="genre-chip" id="chip-trap" onclick="setGenre('trap')">⚡ Trap 808 Bass</span>
        <span class="genre-chip" id="chip-thrill" onclick="setGenre('thrill')">🦇 Thriller Pulse</span>
        <span class="genre-chip" id="chip-lofi" onclick="setGenre('lofi')">🧘 Lo-Fi Chill</span>
      </div>

      <!-- BPM Slider & Tap Tempo -->
      <div class="slider-box" style="margin-top:10px;">
        <div class="slider-header">
          <span>Tempo Speed (BPM)</span>
          <b id="bpm-display" style="color:var(--accent);">124 BPM</b>
        </div>
        <input type="range" id="bpm-slider" min="50" max="200" value="124" oninput="onBpmSlider(this.value)">
      </div>

      <button class="btn-tap" onclick="onTapTempo()">🥁 TAP WITH EARBUDS BEAT (Auto-Calculates Tempo)</button>

      <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px; font-size:0.75rem;">
        <span style="color:var(--subtext);">🔊 Play Beat Click in Earbuds:</span>
        <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
          <input type="checkbox" id="earbuds-click-chk" onchange="toggleClickSound(this.checked)">
          <span style="color:var(--text); font-weight:700;">Audible Click</span>
        </label>
      </div>
    </div>

    <!-- 🎵 IN-BROWSER SONG & BEAT PLAYER (100% Works on HTTP) -->
    <div class="card player-card">
      <div class="card-title">🎵 Play Audio / MP3 File (Direct FFT to Earbuds)</div>
      <div style="font-size:0.72rem; color:var(--subtext); margin-bottom:6px;">
        Plays any MP3/music file in your earbuds with <b>100% full FFT audio analysis</b> into the Sound Monitor & LED:
      </div>
      <div class="player-controls">
        <label class="file-label" for="audio-file-input">
          📁 Load Any Music / MP3 File
          <input type="file" id="audio-file-input" accept="audio/*" style="display:none;" onchange="onAudioFileSelected(event)">
        </label>
        <button class="btn-play-song" onclick="playDemoBeatTrack()">▶ Play Demo EDM Beat</button>
      </div>
      <audio id="media-audio-player" controls style="width:100%; height:32px; margin-top:8px; display:none;"></audio>
    </div>

    <!-- 💻 OTHER AUDIO CAPTURE SOURCES -->
    <div class="card">
      <div class="card-title">💻 Direct Audio Capture Sources</div>
      <button class="btn-src" onclick="startLaptopSpotifyCapture()">
        <span>💻</span>
        <div>
          <b>Sync Laptop Spotify Directly (System Audio)</b>
          <div style="font-size:0.7rem; color:var(--subtext);">Digital audio share from Spotify window or tab</div>
        </div>
      </button>
      <button class="btn-src" onclick="toggleMicrophone()">
        <span>🎙️</span>
        <div>
          <b id="mic-btn-label">Start External Microphone Scanner</b>
          <div style="font-size:0.7rem; color:var(--subtext);">Listens to ambient room audio with dynamic beat reactivity</div>
        </div>
      </button>

      <!-- 🎚️ Dynamic Beat Sensitivity Slider -->
      <div class="slider-box" style="margin-top:12px;">
        <div class="slider-header">
          <span>🎚️ Beat Sensitivity (Low & High Beats)</span>
          <b id="sens-display" style="color:var(--accent);">3 - Normal Adaptive (Recommended)</b>
        </div>
        <input type="range" id="sens-slider" min="1" max="5" value="3" step="1" oninput="onSensitivityChange(this.value)">
      </div>

      <!-- 💡 Spotify System Audio Helper Banner -->
      <div style="margin-top:10px; padding:9px 12px; background:rgba(99,102,241,0.12); border-left:3px solid var(--accent); border-radius:6px; font-size:0.71rem; color:#c7d2fe; line-height:1.45;">
        💡 <b>How to share Spotify audio in Chrome properly:</b><br>
        1. When Chrome pops up the share dialog, choose <b>Entire Screen</b> (or Spotify tab).<br>
        2. <b>IMPORTANT:</b> Check the box <b>"Also share system audio"</b> at bottom-left, then click Share!<br>
        3. <b>⚡ Background Tab Mode:</b> You can minimize Chrome or switch to any other tab/game/code — the LED beat sync runs continuously in the background!<br>
        <i>(Tip: If using Bluetooth BLE, you don't even need USB cable or hostel Wi-Fi!)</i>
      </div>
    </div>
  </div>

  <!-- ==================== TAB 2: 🤖 GEMINI AI ==================== -->
  <div id="tab-gemini" class="tab-content">
    <div class="card gemini-card">
      <div class="card-title">🤖 Gemini AI Director (3.5 Flash)</div>
      <input type="password" id="gemini-key" class="ai-input" value="" placeholder="Gemini API Key (optional)" style="height:30px; font-size:0.72rem; margin-bottom:8px;">

      <div style="font-size:0.72rem; color:var(--subtext); margin-bottom:4px;">1-Click Instant Concept Prompts:</div>
      <div class="quick-chips">
        <span class="ai-chip" onclick="setAndAskGemini('Michael Jackson Thriller suspense with dramatic lightning strikes')">🦇 Thriller Suspense</span>
        <span class="ai-chip" onclick="setAndAskGemini('Peaceful relaxing lo-fi rain and calm meditation')">🧘 Peace & Calm</span>
        <span class="ai-chip" onclick="setAndAskGemini('Daft Punk retro 80s disco funk party beat')">🪩 Disco Funk</span>
        <span class="ai-chip" onclick="setAndAskGemini('High-energy Skrillex EDM rave bass drop')">⚡ EDM Drop</span>
        <span class="ai-chip" onclick="setAndAskGemini('Cyberpunk Tokyo matrix hacker neon glow')">🌆 Cyberpunk</span>
        <span class="ai-chip" onclick="setAndAskGemini('Warm romantic candle dinner date acoustic')">🕯️ Romantic Glow</span>
      </div>

      <div class="ai-chat-box">
        <input type="text" id="ai-prompt-input" class="ai-input" placeholder="Type ANY song, mood, scene or vibe...">
        <button class="btn-gemini-send" id="btn-ai-send" onclick="askGeminiPrompt()">Ask 🚀</button>
      </div>

      <div id="ai-bubble" class="ai-bubble">
        <b id="ai-title" style="color:#c084fc;">Generating...</b>
        <div id="ai-desc" style="color:var(--subtext); margin-top:3px;"></div>
      </div>
    </div>
  </div>

  <!-- ==================== TAB 3: 🎭 20 PRESETS ==================== -->
  <div id="tab-presets" class="tab-content">
    <div class="card">
      <div class="card-title">🎭 20 Handcrafted Mood Presets</div>
      <div class="preset-grid">
        <button class="preset-btn" onclick="setMode(12)">🦇 Thriller / Suspense</button>
        <button class="preset-btn" onclick="setMode(13)">🧘 Peace / Zen Ambient</button>
        <button class="preset-btn" onclick="setMode(11)">🪩 Disco Beat Reactive</button>
        <button class="preset-btn" onclick="setMode(10)">🎆 Rave Beat Pulse</button>
        <button class="preset-btn" onclick="setMode(9)">⚡ Lightning Storm</button>
        <button class="preset-btn" onclick="setMode(18)">🌆 Neon Tokyo Synth</button>
        <button class="preset-btn" onclick="setMode(20)">👾 Matrix Cyber Rain</button>
        <button class="preset-btn" onclick="setMode(14)">🌊 Ocean Waves</button>
        <button class="preset-btn" onclick="setMode(15)">🌋 Volcano Magma</button>
        <button class="preset-btn" onclick="setMode(16)">🌲 Enchanted Forest</button>
        <button class="preset-btn" onclick="setMode(17)">🕯️ Candlelight Flicker</button>
        <button class="preset-btn" onclick="setMode(19)">🧊 Glacier Ice Frost</button>
        <button class="preset-btn" onclick="setMode(1)">🌌 Aurora Borealis</button>
        <button class="preset-btn" onclick="setMode(2)">⚡ Cyberpunk Heart</button>
        <button class="preset-btn" onclick="setMode(3)">🔥 Campfire Ember</button>
        <button class="preset-btn" onclick="setMode(4)">🌈 Rainbow 360°</button>
        <button class="preset-btn" onclick="setMode(5)">🚨 Police Strobe</button>
        <button class="preset-btn" onclick="setMode(6)">💡 Mood White</button>
        <button class="preset-btn" onclick="setMode(7)">🚥 Traffic Light</button>
        <button class="preset-btn" onclick="setMode(8)" style="grid-column:span 2; background:linear-gradient(135deg, rgba(99,102,241,0.2), rgba(168,85,247,0.2)); border-color:var(--accent);">🔄 Auto-Cycle All 20 Moods</button>
      </div>
    </div>
  </div>

  <!-- ==================== TAB 4: 🛠️ STUDIO & WI-FI ==================== -->
  <div id="tab-custom" class="tab-content">
    <!-- Home Wi-Fi Setup Card (Allows Internet + ESP32 at the same time!) -->
    <div class="card" style="border-color: rgba(16, 185, 129, 0.4); background: linear-gradient(180deg, rgba(6, 78, 59, 0.2), var(--card-bg));">
      <div class="card-title" style="color:#6ee7b7;">
        <span>🌐 Connect ESP32 to Home Wi-Fi</span>
        <span id="wifi-sta-badge" style="font-size:0.68rem; color:#10b981; font-weight:700;">● Dual AP+STA</span>
      </div>
      <div style="font-size:0.72rem; color:var(--subtext); margin-bottom:8px;">
        Connect ESP32 to your Home Wi-Fi / Hotspot so your laptop keeps full internet while controlling lighting:
      </div>
      <div style="display:flex; flex-direction:column; gap:6px;">
        <input type="text" id="wifi-ssid-input" class="ai-input" placeholder="Home Wi-Fi Name (SSID)">
        <input type="password" id="wifi-pass-input" class="ai-input" placeholder="Home Wi-Fi Password">
        <button class="btn-play" onclick="saveAndConnectHomeWifi()" style="background:#10b981;">Connect ESP32 to Home Wi-Fi</button>
      </div>
      <div id="wifi-status-msg" style="font-size:0.72rem; color:var(--subtext); margin-top:6px;"></div>
    </div>

    <!-- Palette & Custom Picker -->
    <div class="card">
      <div class="card-title">🎨 Individual Colors & Picker</div>
      <div class="palette-grid">
        <div class="swatch" style="background:#ff0000" onclick="setColor(255,0,0)"></div>
        <div class="swatch" style="background:#ff5500" onclick="setColor(255,85,0)"></div>
        <div class="swatch" style="background:#ffaa00" onclick="setColor(255,170,0)"></div>
        <div class="swatch" style="background:#ffff00" onclick="setColor(255,255,0)"></div>
        <div class="swatch" style="background:#80ff00" onclick="setColor(128,255,0)"></div>
        <div class="swatch" style="background:#00ff00" onclick="setColor(0,255,0)"></div>
        <div class="swatch" style="background:#00ffaa" onclick="setColor(0,255,170)"></div>
        <div class="swatch" style="background:#00ffff" onclick="setColor(0,255,255)"></div>
        <div class="swatch" style="background:#0077ff" onclick="setColor(0,119,255)"></div>
        <div class="swatch" style="background:#0000ff" onclick="setColor(0,0,255)"></div>
        <div class="swatch" style="background:#8800ff" onclick="setColor(136,0,255)"></div>
        <div class="swatch" style="background:#ff00aa" onclick="setColor(255,0,170)"></div>
        <div class="swatch" style="background:#ffeedd" onclick="setColor(255,230,200)"></div>
        <div class="swatch" style="background:#ffffff" onclick="setColor(255,255,255)"></div>
      </div>
      <div style="display:flex; gap:8px; align-items:center;">
        <input type="color" id="custom-picker" value="#ff0080" oninput="onHexColor(this.value)" style="border:none; width:44px; height:32px; border-radius:6px; cursor:pointer;">
        <span style="font-size:0.75rem; color:var(--subtext);">Pick any exact hex color for live output</span>
      </div>
    </div>

    <!-- Custom Step Sequence Builder -->
    <div class="card">
      <div class="card-title">🛠️ Custom Sequence Builder (Self Function)</div>
      <div class="form-row">
        <input type="color" id="step-color" value="#00ffff" style="width:40px; height:32px; border-radius:6px;">
        <input type="number" id="step-ms" placeholder="Duration (ms)" value="1500" min="50" max="60000" step="100" style="flex:1;">
        <select id="step-fade" style="flex:1;">
          <option value="1">Smooth Fade</option>
          <option value="0">Instant Snap</option>
        </select>
      </div>
      <button class="btn-add" onclick="addCurrentStep()">+ Add Step to Timeline</button>
      <div class="timeline" id="timeline-list"></div>
      <div class="seq-actions">
        <button class="btn-play" onclick="playCustomSequence()">▶ Play Custom Sequence</button>
        <button class="btn-clear" onclick="clearTimeline()">Clear</button>
      </div>
    </div>
  </div>
</div>

<!-- Modal: Secure Context Guide -->
<div class="modal-overlay" id="secure-modal">
  <div class="modal-box">
    <h3 style="color:#67e8f9; margin-bottom:8px;">🎧 How to Run Localhost with Spotify</h3>
    <p style="font-size:0.78rem; color:var(--subtext); line-height:1.5; margin-bottom:12px;">
      To capture Spotify audio directly with Bluetooth earbuds on your laptop:
      <br>1. Run <code>python serve.py</code> in your project directory.
      <br>2. Open <b>http://localhost:8000</b> in your browser.
      <br>3. Click <b>Sync Laptop Spotify Audio</b>!
    </p>
    <button class="btn-play" style="width:100%;" onclick="document.getElementById('secure-modal').style.display='none'">Got It!</button>
  </div>
</div>

<script>
  function switchTab(tabId, el) {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
    const targetContent = document.getElementById(tabId);
    if (targetContent) targetContent.classList.add('active');
    
    if (el) {
      el.classList.add('active');
    } else {
      const btn = document.querySelector(`button[onclick*="'${tabId}'"]`);
      if (btn) btn.classList.add('active');
    }
  }

  function setStatusPill(text) {
    const el = document.getElementById('status-pill');
    if (el) el.textContent = text;
  }

  function setActiveModeTitle(text) {
    const el = document.getElementById('active-mode-title');
    if (el) el.textContent = text;
  }

  function turnLedOn() {
    setMode(1);
    updateVirtualLed(0, 255, 120, 255);
    setStatusPill('● Mode 1 (ON)');
    setActiveModeTitle('Mode: Aurora Borealis');
  }

  function updateVirtualLed(r, g, b, alpha=255) {
    let orb = document.getElementById('virtual-led');
    let a = alpha / 255.0;
    if (orb) {
      orb.style.background = `rgba(${r}, ${g}, ${b}, ${Math.max(0.15, a)})`;
      orb.style.boxShadow = `0 0 ${Math.round(24 * a)}px rgba(${r}, ${g}, ${b}, ${a * 0.9}), inset 0 0 10px rgba(0,0,0,0.5)`;
    }
    let rgbVal = document.getElementById('active-rgb-val');
    if (rgbVal) rgbVal.textContent = `RGB(${r}, ${g}, ${b}) • ${Math.round(a * 100)}%`;
    let hex = '#' + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1).toUpperCase();
    let chip = document.getElementById('live-color-chip');
    if (chip) chip.style.background = hex;
    let hexVal = document.getElementById('live-color-hex');
    if (hexVal) hexVal.textContent = hex;

    // 🚥 Keep External 8-LED Bar Live HUD synced!
    updateVirtualExtBar(r, g, b, alpha);
  }

  // ====================================================================
  // 🚥 EXTERNAL 8-LED RGB BAR (KEYES ARTOU V2) CONTROLLER
  // ====================================================================
  let isExtBarEnabled = true;
  let activeExtBarMode = 0; // 0: Mirror, 1: VU Meter, 2: Spectrum, 3: Rainbow, 4: Scanner, 5: Fire
  let extBarBrightness = 100;
  let extVirtualVu = 0.0;
  let extVirtualPeak = 0.0;
  let lastExtPeakTime = 0;

  function toggleExtPower() {
    isExtBarEnabled = !isExtBarEnabled;
    let btn = document.getElementById('btn-ext-power');
    let status = document.getElementById('ext-card-status');
    if (btn) {
      if (isExtBarEnabled) {
        btn.textContent = '⏻ Bar Power: ON';
        btn.style.background = 'linear-gradient(135deg, #0284c7, #06b6d4)';
      } else {
        btn.textContent = '⏻ Bar Power: OFF';
        btn.style.background = 'rgba(239, 68, 68, 0.2)';
      }
    }
    if (status) {
      status.textContent = isExtBarEnabled ? 'Active (125 FPS)' : 'OFF (Standby)';
      status.style.color = isExtBarEnabled ? '#38bdf8' : '#ef4444';
    }
    syncExtBarHardware();
    if (!isExtBarEnabled) {
      renderVirtualExtBarDirect([0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,0,0, 0,0,0]);
    }
  }

  function setExtBarMode(mode) {
    activeExtBarMode = parseInt(mode);
    for (let i = 0; i <= 5; i++) {
      let b = document.getElementById('ext-mode-btn-' + i);
      if (b) {
        if (i === activeExtBarMode) {
          b.classList.add('active');
          b.style.borderColor = '#38bdf8';
        } else {
          b.classList.remove('active');
          b.style.borderColor = 'var(--card-border)';
        }
      }
    }
    let mirrorQuick = document.getElementById('btn-quick-mirror');
    if (mirrorQuick) {
      if (activeExtBarMode === 0) mirrorQuick.classList.add('active');
      else mirrorQuick.classList.remove('active');
    }
    let vuQuick = document.getElementById('btn-quick-vu');
    if (vuQuick) {
      if (activeExtBarMode === 1) vuQuick.classList.add('active');
      else vuQuick.classList.remove('active');
    }
    let modeNames = ['Mirror', 'VU Meter', 'Spectrum', 'Rainbow', 'Scanner', 'Fire'];
    let badge = document.getElementById('ext-bar-mode-badge');
    if (badge) badge.textContent = modeNames[activeExtBarMode] || 'Mode ' + activeExtBarMode;
    syncExtBarHardware();
  }

  function onExtBrightness(val) {
    extBarBrightness = parseInt(val);
    let el = document.getElementById('ext-bright-val');
    if (el) el.textContent = extBarBrightness + '%';
    syncExtBarHardware();
  }

  function syncExtBarHardware() {
    let en = isExtBarEnabled ? 1 : 0;
    let cmd = `!EXT:${en},${activeExtBarMode},${extBarBrightness}`;
    let url = `/api/ext?en=${en}&mode=${activeExtBarMode}&bri=${extBarBrightness}`;
    sendHardwareCommand(cmd, url);
  }

  function testBarColor(r, g, b) {
    if (!isExtBarEnabled) toggleExtPower();
    let data = [];
    for (let i = 0; i < 8; i++) data.push(`${r},${g},${b}`);
    let cmd = `!EXTD:${data.join(';')}`;
    sendHardwareCommand(cmd, `/api/ext?mode=6`);
    let arr = [];
    for (let i = 0; i < 8; i++) arr.push(r, g, b);
    renderVirtualExtBarDirect(arr);
    let badge = document.getElementById('ext-bar-mode-badge');
    if (badge) badge.textContent = 'Test Color';
  }

  function testBarChaser() {
    if (!isExtBarEnabled) toggleExtPower();
    setExtBarMode(4); // Cyber Scanner mode
    let badge = document.getElementById('ext-bar-mode-badge');
    if (badge) badge.textContent = 'Cyber Scanner';
  }

  function updateVirtualExtBar(r, g, b, intensity) {
    if (!isExtBarEnabled) return;
    let now = performance.now();
    let normBri = (extBarBrightness / 100.0);

    if (activeExtBarMode === 0) {
      // 0: Mirror main LED across all 8 dots
      let dotR = Math.round(r * (intensity / 255.0) * normBri);
      let dotG = Math.round(g * (intensity / 255.0) * normBri);
      let dotB = Math.round(b * (intensity / 255.0) * normBri);
      let colStr = `rgb(${dotR}, ${dotG}, ${dotB})`;
      let glowStr = (intensity > 20) ? `0 0 10px rgba(${dotR}, ${dotG}, ${dotB}, 0.8)` : 'none';
      for (let i = 0; i < 8; i++) {
        let dot = document.getElementById('ext-dot-' + i);
        if (dot) {
          dot.style.background = (dotR === 0 && dotG === 0 && dotB === 0) ? '#151926' : colStr;
          dot.style.boxShadow = glowStr;
          dot.style.borderColor = (intensity > 20) ? colStr : '#334155';
        }
      }
    } else if (activeExtBarMode === 1) {
      // 1: 8-LED Audio VU Meter
      let targetVu = (intensity / 255.0) * 8.0;
      if (targetVu > extVirtualVu) {
        extVirtualVu = targetVu;
      } else {
        extVirtualVu = Math.max(0, extVirtualVu - 0.22);
      }
      if (extVirtualVu >= extVirtualPeak) {
        extVirtualPeak = extVirtualVu;
        lastExtPeakTime = now;
      } else if (now - lastExtPeakTime > 50) {
        extVirtualPeak = Math.max(0, extVirtualPeak - 0.15);
        lastExtPeakTime = now;
      }

      const vuCols = [
        [0, 255, 60], [40, 255, 20], [120, 255, 0], [230, 220, 0],
        [255, 140, 0], [255, 60, 0], [255, 15, 20], [255, 0, 110]
      ];

      for (let i = 0; i < 8; i++) {
        let dot = document.getElementById('ext-dot-' + i);
        if (!dot) continue;
        let ledThresh = i;
        let c = vuCols[i];
        let isPeak = (Math.floor(extVirtualPeak) === i && extVirtualPeak > 0.5);

        if (isPeak) {
          dot.style.background = '#ffffff';
          dot.style.boxShadow = '0 0 12px #ffffff';
          dot.style.borderColor = '#ffffff';
        } else if (extVirtualVu >= ledThresh + 1.0) {
          let cr = Math.round(c[0] * normBri);
          let cg = Math.round(c[1] * normBri);
          let cb = Math.round(c[2] * normBri);
          dot.style.background = `rgb(${cr}, ${cg}, ${cb})`;
          dot.style.boxShadow = `0 0 8px rgba(${cr}, ${cg}, ${cb}, 0.7)`;
          dot.style.borderColor = `rgb(${cr}, ${cg}, ${cb})`;
        } else if (extVirtualVu > ledThresh) {
          let frac = (extVirtualVu - ledThresh) * normBri;
          let cr = Math.round(c[0] * frac);
          let cg = Math.round(c[1] * frac);
          let cb = Math.round(c[2] * frac);
          dot.style.background = `rgb(${cr}, ${cg}, ${cb})`;
          dot.style.boxShadow = `0 0 5px rgba(${cr}, ${cg}, ${cb}, 0.5)`;
          dot.style.borderColor = `rgba(${c[0]}, ${c[1]}, ${c[2]}, 0.5)`;
        } else {
          dot.style.background = '#151926';
          dot.style.boxShadow = 'none';
          dot.style.borderColor = '#334155';
        }
      }
    } else {
      // 2, 3, 4, 5: Dynamic animation preview
      for (let i = 0; i < 8; i++) {
        let dot = document.getElementById('ext-dot-' + i);
        if (!dot) continue;
        let h = (activeExtBarMode === 3) ? (now / 15 + i * 45) % 360 : (200 + i * 20) % 360;
        let s = (intensity > 20) ? `hsl(${h}, 100%, 50%)` : '#151926';
        dot.style.background = s;
        dot.style.boxShadow = (intensity > 20) ? `0 0 8px ${s}` : 'none';
        dot.style.borderColor = (intensity > 20) ? s : '#334155';
      }
    }
  }

  function renderVirtualExtBarDirect(rgbArray) {
    for (let i = 0; i < 8; i++) {
      let dot = document.getElementById('ext-dot-' + i);
      if (dot) {
        let r = rgbArray[i * 3] || 0;
        let g = rgbArray[i * 3 + 1] || 0;
        let b = rgbArray[i * 3 + 2] || 0;
        dot.style.background = (r === 0 && g === 0 && b === 0) ? '#151926' : `rgb(${r},${g},${b})`;
        dot.style.boxShadow = (r > 10 || g > 10 || b > 10) ? `0 0 8px rgba(${r},${g},${b},0.7)` : 'none';
        dot.style.borderColor = (r > 10 || g > 10 || b > 10) ? `rgb(${r},${g},${b})` : '#334155';
      }
    }
  }

  // ====================================================================
  // 🔵 WEB BLUETOOTH (BLE 5.0) NORDIC UART ENGINE
  // ====================================================================
  const BLE_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
  const BLE_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
  const BLE_TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

  let bleDevice = null;
  let bleServer = null;
  let bleRxCharacteristic = null;
  let bleConnected = false;

  async function toggleBluetoothConnect() {
    if (!navigator.bluetooth) {
      alert("Web Bluetooth requires Google Chrome or Microsoft Edge on localhost or HTTPS.\n\nPlease open http://localhost:8000 in Chrome to connect via Bluetooth!\n\n(Also ensure Bluetooth is turned ON in Windows Settings)");
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

      // acceptAllDevices ensures Windows Chrome lists ESP32-RGB-Studio without strict filter rejection
      bleDevice = await navigator.bluetooth.requestDevice({
        acceptAllDevices: true,
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
      alert("Bluetooth notice:\n" + err.message + "\n\n1. Make sure Bluetooth is ON in Windows Settings.\n2. Make sure ESP32 is powered on.");
    }
  }

  function onBleDisconnected() {
    bleConnected = false;
    bleRxCharacteristic = null;
    updateBleUI(false);
    console.log("🔵 BLE Disconnected");
  }

  function disconnectBluetooth() {
    if (bleDevice && bleDevice.gatt.connected) {
      bleDevice.gatt.disconnect();
    }
    bleConnected = false;
    bleRxCharacteristic = null;
    updateBleUI(false);
  }

  function updateBleUI(connected) {
    const btn = document.getElementById('btn-ble-connect');
    const pill = document.getElementById('ble-status-pill');
    if (connected) {
      btn.innerHTML = '<span>🔵</span> Disconnect BLE';
      btn.style.background = 'linear-gradient(135deg, #10b981, #06b6d4)';
      if (pill) {
        pill.textContent = '● BLE Connected';
        pill.style.background = 'rgba(16,185,129,0.15)';
        pill.style.color = '#34d399';
        pill.style.borderColor = 'rgba(16,185,129,0.3)';
      }
    } else {
      btn.innerHTML = '<span>🔵</span> Connect Bluetooth';
      btn.style.background = 'linear-gradient(135deg, #4f46e5, #06b6d4)';
      if (pill) {
        pill.textContent = '⚪ BLE Ready';
        pill.style.background = 'rgba(99,102,241,0.15)';
        pill.style.color = '#a5b4fc';
        pill.style.borderColor = 'rgba(99,102,241,0.3)';
      }
    }
  }

  async function sendBleCommand(cmd) {
    if (!bleConnected || !bleRxCharacteristic) return false;
    try {
      const encoder = new TextEncoder();
      await bleRxCharacteristic.writeValueWithoutResponse(encoder.encode(cmd));
      return true;
    } catch(e) {
      console.warn("BLE write error:", e);
      return false;
    }
  }

  function sendHardwareCommand(cmd, fallbackUrl) {
    if (bleConnected && bleRxCharacteristic) {
      sendBleCommand(cmd + '\n');
    }
    if (fallbackUrl) {
      fetch(fallbackUrl).catch(() => {});
    }
  }

  function setMode(modeNum) {
    sendHardwareCommand('!M:' + modeNum, '/api/mode?val=' + modeNum);
    let titles = {
      1: "Aurora Borealis", 2: "Cyberpunk Pulse", 3: "Campfire Ember", 4: "Rainbow Wave",
      5: "Emergency Strobe", 6: "Breathing White", 7: "Traffic Light", 8: "Auto-Cycle 20 Moods",
      9: "Lightning Storm", 10: "Party Rave Beat", 11: "Dynamic Beat Reactive", 12: "Thriller Suspense",
      13: "Peace / Zen Ambient", 14: "Ocean Waves", 15: "Volcano Magma", 16: "Enchanted Forest",
      17: "Candlelight Flicker", 18: "Neon Tokyo Synth", 19: "Glacier Ice Frost", 20: "Matrix Cyber Rain",
      0: "LED OFF", "-1": "Solid Free Light", "-2": "Custom Sequence"
    };
    setActiveModeTitle("Mode: " + (titles[modeNum] || ("Mode " + modeNum)));
    setStatusPill((modeNum === 0) ? '● OFF' : '● Mode ' + modeNum);
    
    // Live Virtual LED Preview Orb sync
    const previewColors = {
      0: [0, 0, 0, 0],
      1: [0, 255, 128, 255],
      2: [255, 0, 140, 255],
      3: [255, 90, 20, 255],
      4: [255, 50, 220, 255],
      5: [255, 255, 255, 255],
      6: [255, 255, 255, 255],
      7: [255, 180, 0, 255],
      8: [0, 220, 255, 255],
      9: [255, 255, 255, 255],
      10: [255, 0, 80, 255],
      11: [99, 102, 241, 255],
      12: [220, 0, 40, 255],
      13: [64, 224, 208, 255],
      14: [0, 120, 255, 255],
      15: [255, 40, 0, 255],
      16: [34, 197, 94, 255],
      17: [255, 140, 20, 255],
      18: [244, 63, 94, 255],
      19: [186, 230, 253, 255],
      20: [34, 197, 94, 255]
    };
    let c = previewColors[modeNum] || [99, 102, 241, 255];
    updateVirtualLed(c[0], c[1], c[2], c[3]);
  }

  // 💡 FREE SOLID LAMP MODE (CONTINUOUS ON)
  function setLampColor(r, g, b) {
    stopAllAudio(); // Ensure audio react is completely stopped
    sendHardwareCommand(`!C:${r},${g},${b}`, `/api/color?r=${r}&g=${g}&b=${b}`);
    updateVirtualLed(r, g, b, 255);
    setActiveModeTitle("Mode: Solid Ambient Lamp");
    setStatusPill("● Lamp (Solid ON)");
  }

  function onLampHexColor(hex) {
    let r = parseInt(hex.slice(1, 3), 16);
    let g = parseInt(hex.slice(3, 5), 16);
    let b = parseInt(hex.slice(5, 7), 16);
    let hexDisp = document.getElementById('lamp-hex-display');
    if (hexDisp) hexDisp.textContent = hex.toUpperCase();
    setLampColor(r, g, b);
  }

  function setColor(r, g, b) {
    setLampColor(r, g, b);
  }

  function onHexColor(hex) {
    onLampHexColor(hex);
  }

  function onBrightness(val) {
    let bv = document.getElementById('bright-val');
    if (bv) bv.textContent = val + '%';
    sendHardwareCommand('!BR:' + val, '/api/brightness?val=' + val);
  }

  // ⏱️ Bluetooth Latency Offset Slider (-150ms to +150ms)
  let syncOffsetMs = 0;
  function onOffsetChange(val) {
    syncOffsetMs = parseInt(val);
    let label = (syncOffsetMs === 0) ? '0 ms (Instant)' : ((syncOffsetMs > 0 ? '+' : '') + syncOffsetMs + ' ms');
    let ov = document.getElementById('offset-val');
    if (ov) ov.textContent = label;
  }

  // ⏱️ Pacing / Minimum Beat Interval & Decay
  let minBeatInterval = 320;
  let activeDecay = 480;
  function setPace(p) {
    document.querySelectorAll('[id^="pace-"]').forEach(el => el.classList.remove('active'));
    document.getElementById('pace-' + p).classList.add('active');
    if (p === 'clean') {
      minBeatInterval = 320;
      activeDecay = 480;
    } else if (p === 'fast') {
      minBeatInterval = 200;
      activeDecay = 340;
    } else if (p === 'chill') {
      minBeatInterval = 550;
      activeDecay = 680;
    }
  }

  // ====================================================================
  // 🎛️ DYNAMIC AUDIO CUSTOMIZATION STUDIO STATE & MASTER CONTROLS
  // ====================================================================
  let isCustomStudioActive = true;
  let isBassCustomActive = true;
  let isVocalCustomActive = true;
  let isRangeCustomActive = true;
  let isSpikeCustomActive = true;
  let isChromaCustomActive = true;

  // Bass Studio
  let bassWeight = 1.5;
  let isBassBoost = false;

  // Vocal Studio
  let vocalWeight = 1.8;
  let isVocalSpotlight = false;

  // Range Studio
  let brightnessFloor = 0;       // 0% to 90%
  let brightnessCeiling = 100;    // 40% to 100%
  let keepFloorOnSilence = false;

  // Spike Dynamics
  let enableSpikeAnticipation = true;
  let enableSpikeMulticolor = true;  // 🎨 True: Keeps rich colors alive!
  let enableSpikeWhiteStrobe = false; // ✨ False: Prevents flat white drowning
  let activeSpikeCurve = 'harmonic'; // 'harmonic', 'snappy', 'ripple', 'bloom'

  // Chroma Pitch & Multicolor Studio
  let chromaRootOffset = 0;       // 0° to 360°
  let chromaOctaveSpread = 1.0;   // 0.5x to 2.5x
  let activeChromaPalette = 'flow'; // 'flow' (multicolor flow), 'spectrum', 'cyber', 'sunset', 'fifths'
  let includeWhiteSparkles = true; // ✨ Include diamond white accents with multicolors!
  let detectedPitchNote = '--';
  let detectedPitchFreq = 0;
  let detectedPitchMidi = 60;
  let detectedPitchNoteIndex = 0;

  // Auto Feeling AI Vibe Engine State
  let detectedFeeling = 'Chill Lo-Fi';
  let energyHistory = [];
  let vocalHistory = [];
  let kickHistory = [];

  // ====================================================================
  // 🧠 GEMINI AI MUSIC DIRECTOR INTELLIGENCE LAYER
  // ====================================================================
  let aiLightingState = {
    active: false,
    mood: 'futuristic',
    emotion: 'energetic',
    scene: 'cyberpunk_rave',
    colors: ['#00F3FF', '#FF007F', '#7B00FF'],
    colorsRGB: [[0, 243, 255], [255, 0, 127], [123, 0, 255]],
    brightness: 0.90,
    saturation: 0.95,
    effect: 'pulse',
    movement_speed: 0.80,
    beat_reactivity: 0.95,
    transition: 'intensify',
    event: 'BUILD_UP',
    description: 'Futuristic neon pulses of cyan, magenta, and purple reacting intensely to rising energy.',
    confidence: 95,
    lastUpdated: 0
  };

  let isAiRequestPending = false;
  let lastAiRequestTime = 0;
  let aiMusicHistory = [];
  let lastSnapshotTime = 0;

  function hexToRgb(hex) {
    if (!hex) return [0, 245, 255];
    hex = hex.replace('#', '');
    if (hex.length === 3) hex = hex.split('').map(c => c + c).join('');
    let num = parseInt(hex, 16);
    if (isNaN(num)) return [0, 245, 255];
    return [(num >> 16) & 255, (num >> 8) & 255, num & 255];
  }

  function applyAiLightingInterpretation(interp) {
    if (!interp) return;
    if (interp.mood) aiLightingState.mood = interp.mood;
    if (interp.emotion) aiLightingState.emotion = interp.emotion;
    if (interp.scene) aiLightingState.scene = interp.scene;
    if (Array.isArray(interp.colors) && interp.colors.length >= 3) {
      aiLightingState.colors = interp.colors.slice(0, 3);
      aiLightingState.colorsRGB = aiLightingState.colors.map(hexToRgb);
    }
    if (typeof interp.brightness === 'number') aiLightingState.brightness = interp.brightness;
    if (typeof interp.saturation === 'number') aiLightingState.saturation = interp.saturation;
    if (interp.effect) aiLightingState.effect = interp.effect;
    if (typeof interp.movement_speed === 'number') aiLightingState.movement_speed = interp.movement_speed;
    if (typeof interp.beat_reactivity === 'number') aiLightingState.beat_reactivity = interp.beat_reactivity;
    if (interp.transition) aiLightingState.transition = interp.transition;
    if (interp.event) aiLightingState.event = interp.event;
    if (interp.description) aiLightingState.description = interp.description;
    if (typeof interp.confidence === 'number') aiLightingState.confidence = Math.round(interp.confidence * 100);
    aiLightingState.lastUpdated = Date.now();

    updateAiDirectorUI();
  }

  function updateAiDirectorUI() {
    let moodEl = document.getElementById('ai-mood-display');
    if (moodEl) moodEl.textContent = `${aiLightingState.mood.toUpperCase()} • ${aiLightingState.emotion}`;
    
    let eventEl = document.getElementById('ai-event-badge');
    if (eventEl) {
      eventEl.textContent = `⚡ ${aiLightingState.event}`;
      if (['DROP', 'PEAK', 'BUILD_UP'].includes(aiLightingState.event)) {
        eventEl.className = 'beat-badge peak';
      } else {
        eventEl.className = 'beat-badge normal';
      }
    }

    let confEl = document.getElementById('ai-confidence-pill');
    if (confEl) confEl.textContent = `Confidence: ${aiLightingState.confidence}%`;

    let descEl = document.getElementById('ai-scene-description');
    if (descEl) descEl.textContent = `"${aiLightingState.description || aiLightingState.scene}"`;

    if (aiLightingState.colors && aiLightingState.colors.length >= 3) {
      for (let i = 0; i < 3; i++) {
        let sw = document.getElementById('ai-color-swatch-' + i);
        if (sw) {
          let hex = aiLightingState.colors[i];
          sw.style.background = hex;
          sw.textContent = hex.toUpperCase();
          sw.style.boxShadow = `0 0 10px ${hex}88`;
          let rgb = aiLightingState.colorsRGB[i] || [255, 255, 255];
          let lum = (0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]) / 255;
          sw.style.color = lum > 0.55 ? '#000000' : '#ffffff';
        }
      }
    }

    let syncEl = document.getElementById('ai-last-sync-time');
    if (syncEl) syncEl.textContent = 'Synced ' + new Date().toLocaleTimeString();
  }

  async function requestGeminiAiUpdate(userPrompt = null, force = false) {
    let now = Date.now();
    let minGap = (userPrompt || force) ? 2000 : 7000;
    if (isAiRequestPending) return;
    if (now - lastAiRequestTime < minGap && !force) return;

    let autoEvolve = document.getElementById('chk-ai-auto-evolve')?.checked ?? true;
    if (!userPrompt && !force && !autoEvolve && activeMusicMode !== 'ai') return;

    isAiRequestPending = true;
    lastAiRequestTime = now;

    let pill = document.getElementById('ai-director-status-pill');
    if (pill) {
      pill.textContent = '⏳ AI THINKING...';
      pill.style.background = 'rgba(234,179,8,0.2)';
      pill.style.color = '#fde047';
      pill.style.borderColor = 'rgba(234,179,8,0.4)';
    }

    try {
      let currentMetrics = {
        bpm: currentBPM || 120,
        bass: Number((displayBass / 255).toFixed(2)),
        mid: Number((displayMids / 255).toFixed(2)),
        treble: Number((displayTreb / 255).toFixed(2)),
        volume: Number((Math.min(255, (displayBass + displayMids + displayTreb) / 3) / 255).toFixed(2)),
        energy: Number((Math.min(1.0, (avgKickEnvelope * 1.2 + avgMidEnvelope) / 200)).toFixed(2)),
        beat_strength: Number((Math.min(1.0, maxKickObserved / 200)).toFixed(2))
      };

      let payload = {
        current: currentMetrics,
        history: aiMusicHistory.slice(-5),
        user_command: userPrompt || ""
      };

      let resp = await fetch('/api/gemini/interpret', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });

      if (resp.ok) {
        let jsonRes = await resp.json();
        if (jsonRes.interpretation) {
          applyAiLightingInterpretation(jsonRes.interpretation);
        }
      }
    } catch(err) {
      console.warn("Gemini AI fetch notice:", err);
    } finally {
      isAiRequestPending = false;
      let p = document.getElementById('ai-director-status-pill');
      if (p) {
        p.textContent = (activeMusicMode === 'ai') ? '🟢 AI ENGAGED' : '🟢 AI READY';
        p.style.background = 'rgba(16,185,129,0.2)';
        p.style.color = '#34d399';
        p.style.borderColor = 'rgba(16,185,129,0.4)';
      }
    }
  }

  function sendUserAiCommand(customText = null) {
    let input = document.getElementById('ai-user-prompt-input');
    let text = customText || (input ? input.value.trim() : '');
    if (!text) return;
    if (input && !customText) input.value = '';
    
    if (activeMusicMode !== 'ai') {
      setMusicMode('ai');
    }
    requestGeminiAiUpdate(text, true);
  }


  // Master Studio ON/OFF Toggle
  function toggleStudioMaster() {
    isCustomStudioActive = !isCustomStudioActive;
    let btn = document.getElementById('btn-studio-master');
    let pill = document.getElementById('studio-master-status');
    let txt = document.getElementById('studio-master-text');
    if (isCustomStudioActive) {
      if (btn) {
        btn.classList.add('active');
        btn.style.background = 'linear-gradient(135deg, #10b981, #059669)';
      }
      if (txt) txt.textContent = 'CUSTOM STUDIO ENGINE: ON (Click to Bypass)';
      if (pill) {
        pill.textContent = '● CUSTOM ACTIVE';
        pill.style.background = 'rgba(16,185,129,0.15)';
        pill.style.color = '#34d399';
        pill.style.borderColor = 'rgba(16,185,129,0.3)';
      }
    } else {
      if (btn) {
        btn.classList.remove('active');
        btn.style.background = 'rgba(239,68,68,0.2)';
      }
      if (txt) txt.textContent = 'CUSTOM STUDIO ENGINE: OFF (Bypassed to Raw)';
      if (pill) {
        pill.textContent = '⚪ BYPASS (RAW)';
        pill.style.background = 'rgba(239,68,68,0.15)';
        pill.style.color = '#fca5a5';
        pill.style.borderColor = 'rgba(239,68,68,0.3)';
      }
    }
  }

  // Section ON/OFF Handlers
  function onBassEnableChange(checked) { isBassCustomActive = checked; }
  function onVocalEnableChange(checked) { isVocalCustomActive = checked; }
  function onRangeEnableChange(checked) { isRangeCustomActive = checked; }
  function onSpikeEnableChange(checked) { isSpikeCustomActive = checked; }
  function onChromaEnableChange(checked) { isChromaCustomActive = checked; }

  // Studio Handlers: Bass Weight & Boost
  function onBassWeightChange(val) {
    bassWeight = parseFloat(val) / 10.0;
    let disp = document.getElementById('bass-weight-display');
    if (disp) disp.textContent = bassWeight.toFixed(1) + 'x ' + (bassWeight > 2.0 ? '(Heavy 808 Bass)' : (bassWeight < 1.0 ? '(Subtle Bass)' : '(Punchy Sub-Bass)'));
  }

  function onBassBoostChange(checked) {
    isBassBoost = checked;
  }

  // Studio Handlers: Vocal Weight & Spotlight
  function onVocalWeightChange(val) {
    vocalWeight = parseFloat(val) / 10.0;
    let disp = document.getElementById('vocal-weight-display');
    if (disp) disp.textContent = vocalWeight.toFixed(1) + 'x ' + (vocalWeight > 2.0 ? '(Heavy Voice)' : (vocalWeight < 1.0 ? '(Subtle Voice)' : '(Punchy Vocals)'));
  }

  function onVocalSpotlightChange(checked) {
    isVocalSpotlight = checked;
  }

  // Studio Handlers: Dynamic Range (Floor & Ceiling)
  function updateRangeWindowUI() {
    let activeWin = document.getElementById('range-active-window');
    if (activeWin) {
      let f = brightnessFloor;
      let w = Math.max(2, brightnessCeiling - brightnessFloor);
      activeWin.style.left = f + '%';
      activeWin.style.width = w + '%';
    }
    let summary = document.getElementById('range-summary-display');
    if (summary) summary.textContent = `${brightnessFloor}% - ${brightnessCeiling}%`;
    let flDisp = document.getElementById('range-floor-display');
    if (flDisp) flDisp.textContent = `${brightnessFloor}%`;
    let clDisp = document.getElementById('range-ceiling-display');
    if (clDisp) clDisp.textContent = `${brightnessCeiling}%`;
    let flSlider = document.getElementById('range-floor-slider');
    if (flSlider) flSlider.value = brightnessFloor;
    let clSlider = document.getElementById('range-ceiling-slider');
    if (clSlider) clSlider.value = brightnessCeiling;
  }

  function onRangeFloorChange(val) {
    brightnessFloor = parseInt(val);
    if (brightnessFloor >= brightnessCeiling) {
      brightnessCeiling = Math.min(100, brightnessFloor + 5);
    }
    updateRangeWindowUI();
  }

  function onRangeCeilingChange(val) {
    brightnessCeiling = parseInt(val);
    if (brightnessCeiling <= brightnessFloor) {
      brightnessFloor = Math.max(0, brightnessCeiling - 5);
    }
    updateRangeWindowUI();
  }

  function setRangePreset(floorVal, ceilVal) {
    brightnessFloor = floorVal;
    brightnessCeiling = ceilVal;
    document.querySelectorAll('[id^="range-chip-"]').forEach(c => c.classList.remove('active'));
    if (floorVal === 0 && ceilVal === 100) document.getElementById('range-chip-full')?.classList.add('active');
    else if (floorVal === 70 && ceilVal === 98) document.getElementById('range-chip-user')?.classList.add('active');
    else if (floorVal === 40 && ceilVal === 85) document.getElementById('range-chip-night')?.classList.add('active');
    else if (floorVal === 20 && ceilVal === 100) document.getElementById('range-chip-club')?.classList.add('active');
    updateRangeWindowUI();
  }

  function onKeepFloorSilenceChange(checked) {
    keepFloorOnSilence = checked;
  }

  function updateRangeNeedle(finalIntensity) {
    let needle = document.getElementById('range-needle');
    if (needle) {
      let pct = Math.min(100, Math.max(0, (finalIntensity / 255.0) * 100));
      needle.style.left = pct + '%';
    }
  }

  function applyBrightnessRange(rawIntensity, isSilent = false) {
    if (!isCustomStudioActive || !isRangeCustomActive) {
      if (isSilent) return 0;
      return Math.min(255, Math.max(0, Math.round(rawIntensity)));
    }
    if (isSilent) {
      if (!keepFloorOnSilence) return 0;
      return Math.round((brightnessFloor / 100.0) * 255.0);
    }
    let f = (brightnessFloor / 100.0) * 255.0;
    let c = (brightnessCeiling / 100.0) * 255.0;
    if (c <= f) c = Math.min(255, f + 1);
    let scaled = f + (rawIntensity / 255.0) * (c - f);
    return Math.min(255, Math.max(0, Math.round(scaled)));
  }

  // Studio Handlers: 3-Stage Spike Dynamics
  function onSpikeAnticipationChange(checked) {
    enableSpikeAnticipation = checked;
  }

  function onSpikeMulticolorChange(checked) {
    enableSpikeMulticolor = checked;
  }

  function onSpikeWhiteStrobeChange(checked) {
    enableSpikeWhiteStrobe = checked;
  }

  function setSpikeCurve(curve) {
    activeSpikeCurve = curve;
    document.querySelectorAll('[id^="spike-curve-"]').forEach(c => c.classList.remove('active'));
    document.getElementById('spike-curve-' + curve)?.classList.add('active');
  }

  // Studio Handlers: Chroma Pitch Studio
  function onChromaWhiteSparkleChange(checked) {
    includeWhiteSparkles = checked;
  }

  function onChromaOffsetChange(val) {
    chromaRootOffset = parseInt(val);
    let disp = document.getElementById('chroma-offset-display');
    const noteMap = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    let nIdx = Math.round((chromaRootOffset / 30) % 12);
    if (disp) disp.textContent = `${chromaRootOffset}° (${noteMap[nIdx]} Base)`;
  }

  function onChromaSpreadChange(val) {
    chromaOctaveSpread = parseFloat(val) / 10.0;
    let disp = document.getElementById('chroma-spread-display');
    if (disp) disp.textContent = chromaOctaveSpread.toFixed(1) + 'x Spread';
  }

  function setChromaPalette(pal) {
    activeChromaPalette = pal;
    document.querySelectorAll('[id^="chroma-pal-"]').forEach(c => c.classList.remove('active'));
    document.getElementById('chroma-pal-' + pal)?.classList.add('active');
  }

  const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  function detectPitch(dataArray, sampleRate) {
    // Scan across melody, vocal & lead instrument range (Bins 2 to 42: ~300 Hz to 3.8 kHz)
    let maxVal = 0, maxBin = 0;
    let totalPower = 0, weightedBinSum = 0;
    for (let i = 2; i <= 42; i++) {
      let v = dataArray[i];
      totalPower += v;
      weightedBinSum += i * v;
      if (v > maxVal) {
        maxVal = v;
        maxBin = i;
      }
    }
    // Sub-bass fallback if melody is quiet
    if (maxVal < 20) {
      for (let i = 0; i <= 3; i++) {
        if (dataArray[i] > maxVal) {
          maxVal = dataArray[i];
          maxBin = i;
        }
      }
    }
    if (maxVal < 15 || totalPower < 25) return { note: '--', freq: 0, midi: 60, noteIndex: 0 };

    let binWidth = sampleRate / 256.0;
    let centroidBin = weightedBinSum / Math.max(1, totalPower);
    let effectiveBin = maxBin * 0.7 + centroidBin * 0.3;
    let freq = Math.round(effectiveBin * binWidth);
    if (freq < 45) freq = 45;
    let midi = Math.round(12 * Math.log2(freq / 440) + 69);
    let noteIndex = (midi % 12 + 12) % 12;
    let noteName = NOTE_NAMES[noteIndex];
    let octave = Math.floor(midi / 12) - 1;
    return { note: `${noteName}${octave}`, freq, midi, noteIndex };
  }

  function getChromaColor(midi, noteIdx, kick = 0, vocals = 0, mids = 0, treble = 0) {
    let p = activeChromaPalette;
    let idx = (noteIdx !== undefined && noteIdx >= 0) ? noteIdx : (midi % 12 + 12) % 12;

    // ✨ Include crisp diamond white sparkle on high treble cymbal or peak vocal transients!
    if (includeWhiteSparkles && treble > 65 && (kick > 40 || vocals > 50)) {
      return [255, 255, 255]; // Crisp diamond white highlight included in multicolour!
    }

    if (p === 'vocal_spectrum') {
      // 🎙️ VOCAL & SPECTRUM INSTRUMENT MAPPING:
      // Low voice / vocals: purplish, violet, fuchsia hues (255° to 305°)
      // Deep bass: crimson / warm amber low-frequency foundation (345° to 25°)
      // Mid rhythm & chords: electric cyan / sapphire (185° to 215°)
      // Transients / cymbals: diamond white sparkle
      if (vocals > 25 && vocals >= kick * 0.8) {
        let noteShift = ((idx * 5) % 40) - 20;
        let vHue = 275 + noteShift;
        return hsvToRgb((vHue + chromaRootOffset) % 360, 0.95, 1.0);
      } else if (kick > 35 && kick > vocals * 1.1) {
        let bHue = (idx % 2 === 0) ? 355 : 18;
        return hsvToRgb((bHue + chromaRootOffset) % 360, 1.0, 1.0);
      } else if (mids > 28) {
        let mHue = 195 + ((idx * 6) % 30);
        return hsvToRgb((mHue + chromaRootOffset) % 360, 1.0, 1.0);
      } else {
        let baseNoteHue = (idx * 30 * chromaOctaveSpread + 270 + chromaRootOffset) % 360;
        return hsvToRgb(baseNoteHue, 0.90, 1.0);
      }
    } else if (p === 'flow') {
      // 🌈 DYNAMIC MULTICOLOR FLOW: Shifts continuously with notes, chords & vocals!
      let baseHue = Math.round((idx * 30 * chromaOctaveSpread + chromaRootOffset) % 360 + 360) % 360;
      let harmonicMod = Math.round((vocals * 0.35 + mids * 0.25)) % 60;
      let finalHue = (baseHue + harmonicMod) % 360;
      return hsvToRgb(finalHue, 1.0, 1.0);
    } else if (p === 'spectrum') {
      let hue = Math.round((idx * 30 * chromaOctaveSpread + chromaRootOffset) % 360 + 360) % 360;
      return hsvToRgb(hue, 1.0, 1.0);
    } else if (p === 'cyber') {
      const cyberHues = [325, 340, 185, 200, 275, 290, 325, 340, 185, 200, 275, 290];
      let hue = Math.round((cyberHues[idx] + chromaRootOffset) % 360 + 360) % 360;
      return hsvToRgb(hue, 1.0, 1.0);
    } else if (p === 'sunset') {
      const sunsetHues = [5, 18, 30, 42, 54, 5, 18, 30, 42, 54, 345, 355];
      let hue = Math.round((sunsetHues[idx] + chromaRootOffset) % 360 + 360) % 360;
      return hsvToRgb(hue, 1.0, 1.0);
    } else if (p === 'fifths') {
      let hue = Math.round((((idx * 7) % 12) * 30 * chromaOctaveSpread + chromaRootOffset) % 360 + 360) % 360;
      return hsvToRgb(hue, 1.0, 1.0);
    }
    return [0, 245, 255];
  }

  // ====================================================================
  // 🎨 13 DIVERSE MUSIC LIGHT MODES + FREE CUSTOM COLOR
  // ====================================================================
  let activeMusicMode = 'cyber';
  let modeHue = 320;
  let modeBeatCount = 0;
  let freeMusicColorRGB = [0, 245, 255]; // Default Free Beat Color: Electric Cyan

  function setMusicMode(m) {
    activeMusicMode = m;
    document.querySelectorAll('.music-mode-btn').forEach(b => b.classList.remove('active'));
    let el = document.getElementById('mode-' + m);
    if (el) el.classList.add('active');
    let aiBtn = document.getElementById('btn-activate-ai-mode');
    if (aiBtn) {
      if (m === 'ai') {
        aiBtn.textContent = '● AI Mode Active';
        aiBtn.style.background = '#10b981';
        aiBtn.style.color = '#fff';
      } else {
        aiBtn.textContent = '▶ Engage AI Mode';
        aiBtn.style.background = 'rgba(16,185,129,0.2)';
        aiBtn.style.color = '#a7f3d0';
      }
    }
    if (m === 'ai') {
      requestGeminiAiUpdate(null, false);
    }
  }

  function setFreeMusicColor(hex) {
    let r = parseInt(hex.slice(1, 3), 16);
    let g = parseInt(hex.slice(3, 5), 16);
    let b = parseInt(hex.slice(5, 7), 16);
    freeMusicColorRGB = [r, g, b];
    let picker = document.getElementById('free-music-picker');
    if (picker) picker.value = hex;
    let hexEl = document.getElementById('free-color-preview-hex');
    if (hexEl) hexEl.textContent = hex.toUpperCase();
    setMusicMode('free');
  }

  function hsvToRgb(h, s, v) {
    h = (h % 360 + 360) % 360;
    let c = v * s;
    let x = c * (1 - Math.abs((h / 60) % 2 - 1));
    let m = v - c;
    let r1 = 0, g1 = 0, b1 = 0;
    if (h < 60)       { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
    else              { r1 = c; g1 = 0; b1 = x; }
    return [Math.round((r1 + m) * 255), Math.round((g1 + m) * 255), Math.round((b1 + m) * 255)];
  }

  function getMusicModeColor(isHeavy, kick = 0, vocals = 0, mids = 0, treble = 0) {
    modeBeatCount++;
    let m = activeMusicMode;

    if (m === 'autofeeling') {
      // 🎭 AUTO FEELING AI ENGINE: Dynamically adapts to song vibe!
      if (detectedFeeling === 'Vocal Ballad') {
        let h = (modeBeatCount % 6 < 3) ? 345 : 38; // Rose Gold / Warm Amber
        return hsvToRgb(h, 0.95, 1.0);
      } else if (detectedFeeling === 'EDM Festival') {
        if (isHeavy) {
          // Multicolour neon burst with occasional white strobe on 4th beat
          return (modeBeatCount % 4 === 0) ? [255, 255, 255] : hsvToRgb((modeBeatCount * 65 + 180) % 360, 1.0, 1.0);
        }
        return [0, 245, 255];
      } else if (detectedFeeling === 'Trap 808 Bass') {
        let h = (modeBeatCount % 6 < 3) ? 280 : 0; // Deep Violet / Blood Red
        return hsvToRgb(h, 1.0, 1.0);
      } else if (detectedFeeling === 'Chill Lo-Fi') {
        let h = (modeBeatCount % 6 < 3) ? 260 : 165; // Lavender / Seafoam
        return hsvToRgb(h, 0.70, 0.95);
      } else if (detectedFeeling === 'Funk Groove') {
        let h = (modeBeatCount % 6 < 3) ? 42 : 95; // Golden Groove / Electric Lime
        return hsvToRgb(h, 1.0, 1.0);
      } else {
        // Cinematic Suspense
        if (isHeavy) {
          return (modeBeatCount % 4 === 0) ? [255, 255, 255] : [220, 20, 50];
        }
        return [180, 10, 40];
      }
    } else if (m === 'cyber') {
      let h = (modeBeatCount % 8 < 4) ? 325 : 190; // Pink / Cyan
      return hsvToRgb(h, 1.0, 1.0);
    } else if (m === 'fire') {
      let h = (modeBeatCount % 8 < 4) ? 5 : 38;   // Crimson / Amber
      return hsvToRgb(h, 1.0, 1.0);
    } else if (m === 'disco') {
      return hsvToRgb((modeBeatCount * 45) % 360, 1.0, 1.0);
    } else if (m === 'edm') {
      // Dynamic multicolor EDM cycle with white punch accent
      if (isHeavy) {
        return (modeBeatCount % 3 === 0) ? [255, 255, 255] : hsvToRgb((modeBeatCount * 75 + 190) % 360, 1.0, 1.0);
      }
      return [0, 170, 255];
    } else if (m === 'thrill') {
      if (isHeavy) {
        return (modeBeatCount % 4 === 0) ? [255, 255, 255] : [255, 20, 80];
      }
      return [255, 0, 10];
    } else if (m === 'zen') {
      let h = (modeBeatCount % 6 < 3) ? 275 : 160; // Violet / Seafoam
      return hsvToRgb(h, 0.75, 1.0);
    } else if (m === 'pitch') {
      // 🌈 CUSTOMIZABLE CHROMA PITCH ENGINE: Tracks melodic note & passes dynamic bands
      return getChromaColor(detectedPitchMidi, detectedPitchNoteIndex, kick, vocals, mids, treble);
    } else if (m === 'ocean') {
      let h = (modeBeatCount % 8 < 4) ? 215 : 175; // Navy / Teal
      return hsvToRgb(h, 0.95, 1.0);
    } else if (m === 'acid') {
      let h = (modeBeatCount % 6 < 3) ? 85 : 305; // Lime Green / Hot Magenta
      return hsvToRgb(h, 1.0, 1.0);
    } else if (m === 'volcano') {
      let h = (modeBeatCount % 6 < 3) ? 0 : 25; // Lava Red / Magma Orange
      return hsvToRgb(h, 1.0, 1.0);
    } else if (m === 'gold') {
      return [255, 195, 80]; // Warm 2700K Audiophile Gold
    } else if (m === 'ice') {
      if (isHeavy) {
        return (modeBeatCount % 3 === 0) ? [255, 255, 255] : [0, 220, 255];
      }
      return [120, 220, 255];
    } else if (m === 'free') {
      // 🎨 Free Custom Beat Color: User's chosen exact color!
      let isWhiteKick = document.getElementById('chk-free-white-kick')?.checked;
      if (isHeavy && isWhiteKick) {
        return [255, 255, 255]; // Crisp white flash on heavy drop
      }
      return [freeMusicColorRGB[0], freeMusicColorRGB[1], freeMusicColorRGB[2]];
    } else if (m === 'ai') {
      // ✨ GEMINI AI MUSIC DIRECTOR
      // Uses Gemini's emotional 3-color palette interpolated with real-time audio dynamics
      let pal = aiLightingState.colorsRGB;
      if (!pal || pal.length < 3) pal = [[0, 243, 255], [255, 0, 127], [123, 0, 255]];
      if (isHeavy) {
        // Peak beat climax: flash between primary intense color (pal[0]) and secondary (pal[1])
        return (modeBeatCount % 2 === 0) ? pal[0] : pal[1];
      } else if (vocals > 35 && vocals > kick) {
        // Melodic vocal presence: emotional midpoint color (pal[1])
        return pal[1];
      } else if (mids > 30) {
        // Rhythm / chords: harmonic background color (pal[2])
        return pal[2];
      } else {
        // Ambient groove: cycle based on detected note and mood
        let pIdx = (detectedPitchNoteIndex + modeBeatCount) % 3;
        return pal[pIdx];
      }
    }
    return [0, 245, 255];
  }

  // ====================================================================
  // 📊 REAL-TIME SOUND & BEAT MONITOR CANVAS
  // ====================================================================
  let canvas = document.getElementById('visualizer-canvas');
  let canvasCtx = canvas.getContext('2d');

  function resizeCanvas() {
    if (canvas && canvas.parentElement) {
      canvas.width = canvas.parentElement.clientWidth || 360;
      canvas.height = 60;
    }
  }
  window.addEventListener('resize', resizeCanvas);
  resizeCanvas();

  // Idle Wave Animation
  function drawIdleWave() {
    if (isListening || isEarbudsSyncActive || isFilePlaying) return;
    canvasCtx.fillStyle = '#05070c';
    canvasCtx.fillRect(0, 0, canvas.width, canvas.height);
    
    canvasCtx.beginPath();
    canvasCtx.strokeStyle = 'rgba(99, 102, 241, 0.45)';
    canvasCtx.lineWidth = 1.5;
    let mid = canvas.height / 2;
    let t = Date.now() / 700;
    for (let x = 0; x < canvas.width; x += 4) {
      let y = mid + Math.sin(x * 0.04 + t) * 5 * Math.sin(t * 0.4);
      if (x === 0) canvasCtx.moveTo(x, y);
      else canvasCtx.lineTo(x, y);
    }
    canvasCtx.stroke();
    
    canvasCtx.fillStyle = 'rgba(130, 146, 179, 0.5)';
    canvasCtx.font = '10px sans-serif';
    canvasCtx.textAlign = 'center';
    canvasCtx.fillText('Standby • Start Earbuds Beat or Play Audio', canvas.width / 2, mid + 16);
    
    requestAnimationFrame(drawIdleWave);
  }
  requestAnimationFrame(drawIdleWave);

  // 4-Band Multi-Instrument Meter smooth falloff helper
  let displayBass = 0, displayVocals = 0, displayMids = 0, displayTreb = 0;
  function updateMeters(k, v, m, t) {
    displayBass = Math.max(k, displayBass * 0.85);
    displayVocals = Math.max(v, displayVocals * 0.85);
    displayMids = Math.max(m, displayMids * 0.85);
    displayTreb = Math.max(t, displayTreb * 0.85);

    let pb = Math.round((displayBass / 255) * 100);
    let pv = Math.round((displayVocals / 255) * 100);
    let pm = Math.round((displayMids / 255) * 100);
    let pt = Math.round((displayTreb / 255) * 100);

    let mb = document.getElementById('meter-bass');
    if (mb) mb.style.width = pb + '%';
    let vb = document.getElementById('val-bass');
    if (vb) vb.textContent = pb + '%';

    let mv = document.getElementById('meter-vocals');
    if (mv) mv.style.width = pv + '%';
    let vv = document.getElementById('val-vocals');
    if (vv) vv.textContent = pv + '%';

    let mm = document.getElementById('meter-mids');
    if (mm) mm.style.width = pm + '%';
    let vm = document.getElementById('val-mids');
    if (vm) vm.textContent = pm + '%';

    let mt = document.getElementById('meter-treble');
    if (mt) mt.style.width = pt + '%';
    let vt = document.getElementById('val-treble');
    if (vt) vt.textContent = pt + '%';
  }

  // ====================================================================
  // 🛑 STOP ALL AUDIO & RESET DASHBOARD
  // ====================================================================
  function stopAllAudio() {
    isListening = false;
    isFilePlaying = false;
    isEarbudsSyncActive = false;
    isDynamicLoopQueued = false;
    
    if (audioStream) {
      audioStream.getTracks().forEach(t => t.stop());
      audioStream = null;
    }
    if (earbudsTimer) {
      clearInterval(earbudsTimer);
      earbudsTimer = null;
    }
    if (demoOscTimer) {
      clearInterval(demoOscTimer);
      demoOscTimer = null;
    }

    if (typeof stopBackgroundAudioWorker === 'function') {
      stopBackgroundAudioWorker();
    }
    document.title = 'ESP32 RGB PRO - Studio';

    let audioPlayer = document.getElementById('media-audio-player');
    if (audioPlayer) {
      audioPlayer.pause();
      audioPlayer.currentTime = 0;
    }

    const btnSync = document.getElementById('btn-sync-toggle');
    if (btnSync) btnSync.classList.remove('active');
    const syncTxt = document.getElementById('sync-text');
    if (syncTxt) syncTxt.textContent = 'START EARBUDS BEAT: ON';
    const syncIcon = document.getElementById('sync-icon');
    if (syncIcon) syncIcon.textContent = '▶';

    let micLabel = document.getElementById('mic-btn-label');
    if (micLabel) micLabel.textContent = 'Start External Microphone Scanner';

    const hud = document.getElementById('hud-status');
    if (hud) hud.textContent = '⏹️ Stopped (LED Off)';
    const badge = document.getElementById('beat-status-badge');
    if (badge) {
      badge.className = 'beat-badge normal';
      badge.textContent = 'Stopped / Idle';
    }
    setStatusPill('● Stopped');

    updateMeters(0, 0, 0, 0);
    updateVirtualLed(0, 0, 0, 0);
    updateRangeNeedle(0);
    let pitchNoteEl = document.getElementById('live-pitch-note');
    if (pitchNoteEl) pitchNoteEl.textContent = '🎵 Note: --';
    let feelingBadge = document.getElementById('auto-feeling-badge');
    if (feelingBadge) feelingBadge.textContent = '🎭 Vibe: Idle';
    let spikeBadge = document.getElementById('spike-phase-badge');
    if (spikeBadge) {
      spikeBadge.textContent = '⚡ Spike: Normal';
      spikeBadge.style.color = '#a5b4fc';
    }

    sendDynamicBeat(0, 0, 0, 0, 80);
    requestAnimationFrame(drawIdleWave);
  }

  // ====================================================================
  // 🎧 EARBUDS BEAT PULSE ENGINE (Direct Sound Monitor Integration)
  // ====================================================================
  let isEarbudsSyncActive = false;
  let earbudsTimer = null;
  let currentBPM = 124;
  let activeGenre = 'edm';
  let beatCounter = 0;
  let lastBeatTimestamp = 0;
  let playEarbudsClick = false;
  let audioCtx = null;

  function toggleClickSound(checked) {
    playEarbudsClick = checked;
    if (checked && (!audioCtx || audioCtx.state === 'suspended')) {
      if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      audioCtx.resume();
    }
  }

  function playClickAudio(isHeavy) {
    if (!playEarbudsClick) return;
    try {
      if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      if (audioCtx.state === 'suspended') audioCtx.resume();
      let osc = audioCtx.createOscillator();
      let gain = audioCtx.createGain();
      osc.type = isHeavy ? 'triangle' : 'sine';
      osc.frequency.setValueAtTime(isHeavy ? 160 : 750, audioCtx.currentTime);
      osc.frequency.exponentialRampToValueAtTime(30, audioCtx.currentTime + 0.06);
      gain.gain.setValueAtTime(0.25, audioCtx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + 0.06);
      osc.connect(gain);
      gain.connect(audioCtx.destination);
      osc.start();
      osc.stop(audioCtx.currentTime + 0.07);
    } catch(e) {}
  }

  function setGenre(genre) {
    activeGenre = genre;
    document.querySelectorAll('.genre-chips .genre-chip[id^="chip-"]').forEach(c => c.classList.remove('active'));
    let el = document.getElementById('chip-' + genre);
    if (el) el.classList.add('active');
  }

  function toggleEarbudsSync() {
    isEarbudsSyncActive = !isEarbudsSyncActive;
    let btn = document.getElementById('btn-sync-toggle');
    let txt = document.getElementById('sync-text');
    let icon = document.getElementById('sync-icon');

    if (isEarbudsSyncActive) {
      if (btn) btn.classList.add('active');
      if (txt) txt.textContent = 'EARBUDS BEAT: ACTIVE (' + currentBPM + ' BPM)';
      if (icon) icon.textContent = '⏹';
      const hud = document.getElementById('hud-status');
      if (hud) hud.textContent = '🎧 Earbuds Beat Active (' + currentBPM + ' BPM)';
      setStatusPill('● Beat Sync ON');
      setMode(11);
      startEarbudsLoop();
      requestAnimationFrame(earbudsMonitorLoop);
    } else {
      stopAllAudio();
    }
  }

  function onBpmSlider(bpm) {
    currentBPM = parseInt(bpm);
    let bpmDisp = document.getElementById('bpm-display');
    if (bpmDisp) bpmDisp.textContent = currentBPM + ' BPM';
    if (isEarbudsSyncActive) {
      let syncTxt = document.getElementById('sync-text');
      if (syncTxt) syncTxt.textContent = 'EARBUDS BEAT: ACTIVE (' + currentBPM + ' BPM)';
      let hud = document.getElementById('hud-status');
      if (hud) hud.textContent = '🎧 Earbuds Beat Active (' + currentBPM + ' BPM)';
      startEarbudsLoop();
    }
  }

  function startEarbudsLoop() {
    if (earbudsTimer) clearInterval(earbudsTimer);
    let interval = Math.round(60000 / currentBPM);
    sendEarbudsStep();
    earbudsTimer = setInterval(sendEarbudsStep, interval);
    if (typeof initBackgroundEngine === 'function') initBackgroundEngine();
    if (bgWorker) {
      bgWorker.postMessage({ action: 'startMetro', interval: interval });
    }
    if (typeof requestScreenWakeLock === 'function') requestScreenWakeLock();
    if (typeof updateBackgroundPill === 'function') updateBackgroundPill(true);
  }

  function sendEarbudsStep() {
    beatCounter++;
    let now = Date.now();
    lastBeatTimestamp = now;

    let isHeavyKick = (beatCounter % 4 === 1);
    let isDrop = false;
    let badge = document.getElementById('beat-status-badge');

    if (activeGenre === 'edm') {
      if (beatCounter % 16 === 15) isDrop = true;
    } else if (activeGenre === 'trap') {
      isHeavyKick = (beatCounter % 8 === 1 || beatCounter % 8 === 5);
    } else if (activeGenre === 'thrill') {
      if (beatCounter % 8 === 7) isDrop = true;
    }

    if (isDrop) {
      if (!document.hidden) {
        if (badge) {
          badge.className = 'beat-badge drop';
          badge.textContent = '🤫 BEAT DROP (BLACKOUT)';
        }
        updateVirtualLed(0, 0, 0, 0);
      } else {
        document.title = '🤫 [DROP] ' + currentBPM + ' BPM';
      }
      sendDynamicBeat(0, 0, 0, 0, 100);
      playClickAudio(false);
      return;
    }

    let [r, g, b] = getMusicModeColor(isHeavyKick, isHeavyKick ? 200 : 80, 120, 100, 100);
    let rawIntensity = isHeavyKick ? 255 : 220;
    let intensity = applyBrightnessRange(rawIntensity, false);
    let decay = isHeavyKick ? activeDecay : Math.round(activeDecay * 0.8);
    updateRangeNeedle(intensity);

    if (isHeavyKick) {
      if (!document.hidden) {
        if (badge) {
          badge.className = 'beat-badge peak';
          badge.textContent = `🔥 HEAVY BEAT (Kick Peak 255)`;
        }
      } else {
        document.title = '🔥 [BEAT] ' + currentBPM + ' BPM';
      }
      playClickAudio(true);
    } else {
      if (!document.hidden) {
        if (badge) {
          badge.className = 'beat-badge normal';
          badge.textContent = `✨ GROOVE BEAT (Glow ${intensity})`;
        }
      } else {
        document.title = '✨ [GROOVE] ' + currentBPM + ' BPM';
      }
      playClickAudio(false);
    }

    if (!document.hidden) {
      updateVirtualLed(r, g, b, intensity);
    }
    sendDynamicBeat(r, g, b, intensity, decay);
  }

  function earbudsMonitorLoop() {
    if (!isEarbudsSyncActive) return;
    if (document.hidden) return; // Save GPU/CPU while tab is in background

    let now = Date.now();
    let beatPeriod = 60000 / currentBPM;
    let elapsed = now - lastBeatTimestamp;
    let phase = Math.min(1.0, elapsed / beatPeriod);
    let decay = Math.max(0, 1.0 - phase * 1.3);

    let kickEnergy = Math.round(255 * Math.exp(-phase * 3.5));
    let midsEnergy = Math.round(150 * Math.sin(phase * Math.PI));
    let trebEnergy = Math.round(180 * Math.max(0, Math.sin(phase * Math.PI * 4)));

    let vocalEnergy = Math.round(midsEnergy * 0.9);
    updateMeters(kickEnergy, vocalEnergy, midsEnergy, trebEnergy);

    // Draw Dynamic Spectrum Bars
    canvasCtx.fillStyle = '#05070c';
    canvasCtx.fillRect(0, 0, canvas.width, canvas.height);
    let numBars = 32;
    let barWidth = (canvas.width / numBars) - 2;

    for (let i = 0; i < numBars; i++) {
      let bandFrac = i / numBars;
      let barVal = 0;
      if (bandFrac < 0.25) barVal = kickEnergy * (1 - bandFrac * 2);
      else if (bandFrac < 0.65) barVal = midsEnergy * (0.8 + 0.4 * Math.sin(i + now/180));
      else barVal = trebEnergy * (0.6 + 0.5 * Math.cos(i + now/120));

      let barH = (barVal / 255) * canvas.height;
      let hue = modeHue + (i * 2);
      canvasCtx.fillStyle = `hsl(${hue}, 100%, ${48 + decay * 22}%)`;
      canvasCtx.fillRect(i * (barWidth + 2), canvas.height - barH, barWidth, barH);
    }

    requestAnimationFrame(earbudsMonitorLoop);
  }

  let tapHistory = [];
  function onTapTempo() {
    let now = Date.now();
    if (tapHistory.length > 0 && now - tapHistory[tapHistory.length - 1] > 2500) tapHistory = [];
    tapHistory.push(now);

    let [r, g, b] = getMusicModeColor(true, 220, 150, 100, 100);
    updateVirtualLed(r, g, b, 255);
    sendDynamicBeat(r, g, b, 255, 450);
    playClickAudio(true);

    if (tapHistory.length > 1) {
      let intervals = [];
      for (let i = 1; i < tapHistory.length; i++) intervals.push(tapHistory[i] - tapHistory[i - 1]);
      let avg = intervals.reduce((a, b) => a + b) / intervals.length;
      let bpm = Math.min(200, Math.max(50, Math.round(60000 / avg)));
      currentBPM = bpm;
      document.getElementById('bpm-slider').value = bpm;
      document.getElementById('bpm-display').textContent = bpm + ' BPM';
      if (!isEarbudsSyncActive) toggleEarbudsSync();
      else startEarbudsLoop();
    }
  }

  // ====================================================================
  // ⚡ MULTI-THREADED BACKGROUND TAB ENGINE & AUDIO KEEP-ALIVE
  // ====================================================================
  let bgWorker = null;
  let bgAudioProcessor = null;
  let silentKeepAliveOsc = null;
  let silentKeepAliveGain = null;
  let screenWakeLock = null;
  let isDynamicLoopQueued = false;
  let lastDynamicStepTime = 0;

  function initBackgroundEngine() {
    if (bgWorker) return;
    try {
      const workerBlobCode = `
        let audioTimer = null;
        let metroTimer = null;
        self.onmessage = function(e) {
          const d = e.data;
          if (d.action === 'startAudio') {
            if (audioTimer) clearInterval(audioTimer);
            audioTimer = setInterval(() => {
              self.postMessage({ type: 'audioTick' });
            }, d.interval || 28);
          } else if (d.action === 'stopAudio') {
            if (audioTimer) { clearInterval(audioTimer); audioTimer = null; }
          } else if (d.action === 'startMetro') {
            if (metroTimer) clearInterval(metroTimer);
            metroTimer = setInterval(() => {
              self.postMessage({ type: 'metroTick' });
            }, d.interval || 500);
          } else if (d.action === 'stopMetro') {
            if (metroTimer) { clearInterval(metroTimer); metroTimer = null; }
          }
        };
      `;
      const blob = new Blob([workerBlobCode], { type: 'application/javascript' });
      bgWorker = new Worker(URL.createObjectURL(blob));
      bgWorker.onmessage = function(e) {
        if (e.data.type === 'audioTick') {
          if (isListening && document.hidden) {
            triggerBackgroundAudioStep();
          }
        } else if (e.data.type === 'metroTick') {
          if (isEarbudsSyncActive) {
            onBackgroundMetroTick();
          }
        }
      };
    } catch(err) {
      console.warn("Background Worker init notice:", err);
    }
  }

  function startBackgroundAudioWorker() {
    initBackgroundEngine();
    if (bgWorker) {
      bgWorker.postMessage({ action: 'startAudio', interval: 28 });
    }
    requestScreenWakeLock();
    updateBackgroundPill(true);
  }

  function stopBackgroundAudioWorker() {
    if (bgWorker) {
      bgWorker.postMessage({ action: 'stopAudio' });
      bgWorker.postMessage({ action: 'stopMetro' });
    }
    releaseScreenWakeLock();
    updateBackgroundPill(false);
  }

  function updateBackgroundPill(active) {
    let pill = document.getElementById('bg-status-pill');
    if (!pill) return;
    if (active) {
      pill.textContent = document.hidden ? '💤 Background Syncing' : '⚡ Background Ready';
      pill.style.background = 'rgba(16,185,129,0.2)';
      pill.style.color = '#34d399';
      pill.style.borderColor = 'rgba(16,185,129,0.4)';
    } else {
      pill.textContent = '⚪ Background Idle';
      pill.style.background = 'rgba(99,102,241,0.15)';
      pill.style.color = '#a5b4fc';
      pill.style.borderColor = 'rgba(99,102,241,0.3)';
    }
  }

  // Silent Audio Output node to keep Chrome Audio Thread awake in background tabs
  function enableAudioKeepAlive() {
    if (!audioCtx) return;
    if (!silentKeepAliveOsc) {
      try {
        silentKeepAliveGain = audioCtx.createGain();
        silentKeepAliveGain.gain.setValueAtTime(0.00001, audioCtx.currentTime); // Inaudible
        silentKeepAliveOsc = audioCtx.createOscillator();
        silentKeepAliveOsc.frequency.setValueAtTime(440, audioCtx.currentTime);
        silentKeepAliveOsc.connect(silentKeepAliveGain);
        silentKeepAliveGain.connect(audioCtx.destination);
        silentKeepAliveOsc.start();
      } catch(e) {
        console.warn("Silent keep-alive error:", e);
      }
    }
  }

  // Screen Wake Lock API: Prevent laptop display / OS sleep while lighting is active
  async function requestScreenWakeLock() {
    if ('wakeLock' in navigator) {
      try {
        if (!screenWakeLock) {
          screenWakeLock = await navigator.wakeLock.request('screen');
          screenWakeLock.addEventListener('release', () => { screenWakeLock = null; });
        }
      } catch(e) {}
    }
  }

  function releaseScreenWakeLock() {
    if (screenWakeLock) {
      screenWakeLock.release().catch(() => {});
      screenWakeLock = null;
    }
  }

  // Earbuds Metronome background tick handler
  function onBackgroundMetroTick() {
    let now = Date.now();
    let interval = Math.round(60000 / currentBPM);
    if (now - lastBeatTimestamp < interval * 0.75) return;
    sendEarbudsStep();
  }

  // Trigger audio step during background execution
  function triggerBackgroundAudioStep() {
    let now = performance.now();
    if (now - lastDynamicStepTime < 22) return;
    lastDynamicStepTime = now;
    if (typeof processDynamicAudioFrame === 'function') {
      processDynamicAudioFrame(true);
    }
  }

  // Visibility change listener: resume 60fps UI when user returns to tab
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) {
      document.title = 'ESP32 RGB PRO - Studio';
      updateBackgroundPill(isListening || isEarbudsSyncActive);
      if (isListening && !isDynamicLoopQueued) {
        processDynamicAudioLoop();
      }
      if (isEarbudsSyncActive) {
        requestAnimationFrame(earbudsMonitorLoop);
      }
    } else {
      updateBackgroundPill(isListening || isEarbudsSyncActive);
    }
  });

  // ====================================================================
  // 🎵 IN-BROWSER AUDIO PLAYER & FULL FFT WEB AUDIO PIPELINE
  // ====================================================================
  let analyser = null, audioStream = null;
  let isListening = false, isFilePlaying = false;
  let mediaElementSource = null;

  function initWebAudio() {
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === 'suspended') audioCtx.resume();
    if (!analyser) {
      analyser = audioCtx.createAnalyser();
      analyser.fftSize = 256;
      analyser.smoothingTimeConstant = 0.05; // ULTRA LOW LATENCY (0.05 for near-instant transient response!)
    }
    enableAudioKeepAlive();
    initBackgroundEngine();

    // Hardware soundcard clock ticker (never throttled by Chrome in background tabs!)
    if (!bgAudioProcessor && audioCtx.createScriptProcessor) {
      try {
        bgAudioProcessor = audioCtx.createScriptProcessor(2048, 1, 1);
        bgAudioProcessor.onaudioprocess = function(e) {
          let out = e.outputBuffer.getChannelData(0);
          for (let i = 0; i < out.length; i++) out[i] = 0; // Absolute silence

          if (isListening && document.hidden) {
            triggerBackgroundAudioStep();
          }
        };
        analyser.connect(bgAudioProcessor);
        bgAudioProcessor.connect(audioCtx.destination);
      } catch(e) {
        console.warn("ScriptProcessor background clock fallback:", e);
      }
    }
  }

  function onAudioFileSelected(evt) {
    let file = evt.target.files[0];
    if (!file) return;

    initWebAudio();
    let player = document.getElementById('media-audio-player');
    player.style.display = 'block';
    player.src = URL.createObjectURL(file);
    player.play();

    if (!mediaElementSource) {
      mediaElementSource = audioCtx.createMediaElementSource(player);
      mediaElementSource.connect(analyser);
      analyser.connect(audioCtx.destination);
    }

    isFilePlaying = true;
    isListening = true;
    let hud = document.getElementById('hud-status');
    if (hud) hud.textContent = '🎵 Playing: ' + file.name.substring(0, 24);
    setMode(11);
    startBackgroundAudioWorker();
    processDynamicAudioLoop();
  }

  // Built-in Demo EDM Synth Track
  let demoOscTimer = null;
  function playDemoBeatTrack() {
    initWebAudio();
    if (demoOscTimer) {
      stopAllAudio();
      return;
    }

    isListening = true;
    document.getElementById('hud-status').textContent = '▶ Demo EDM Beat Playing in Earbuds';
    setMode(11);
    let step = 0;

    demoOscTimer = setInterval(() => {
      step++;
      let isKick = (step % 4 === 1);
      let osc = audioCtx.createOscillator();
      let gain = audioCtx.createGain();
      osc.type = isKick ? 'sine' : 'triangle';
      osc.frequency.setValueAtTime(isKick ? 130 : 380, audioCtx.currentTime);
      osc.frequency.exponentialRampToValueAtTime(35, audioCtx.currentTime + (isKick ? 0.18 : 0.08));
      gain.gain.setValueAtTime(0.4, audioCtx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + (isKick ? 0.18 : 0.08));
      
      osc.connect(gain);
      gain.connect(analyser);
      analyser.connect(audioCtx.destination);
      osc.start();
      osc.stop(audioCtx.currentTime + 0.2);
    }, 240);

    startBackgroundAudioWorker();
    processDynamicAudioLoop();
  }

  // ====================================================================
  // 💻 SYSTEM SPOTIFY & LIVE MICROPHONE CAPTURE
  // ====================================================================
  async function startLaptopSpotifyCapture() {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getDisplayMedia) {
      const modal = document.getElementById('secure-modal');
      if (modal) modal.style.display = 'flex';
      return;
    }
    try {
      audioStream = await navigator.mediaDevices.getDisplayMedia({
        video: true,
        audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false }
      });
      let audioTracks = audioStream.getAudioTracks();
      if (audioTracks.length === 0) {
        alert("⚠️ Audio was not shared!\n\nPlease click 'Sync Laptop Spotify Directly' again, pick your Spotify tab/window, and make sure the 'Share audio' / 'Also share tab audio' checkbox is CHECKED!");
        audioStream.getTracks().forEach(t => t.stop());
        return;
      }
      setupStreamAudio(audioStream, "💻 Laptop Spotify Audio Active");
    } catch(err) {
      if (err.name !== 'NotAllowedError') {
        alert("Audio Capture Notice: " + err.message);
      }
    }
  }

  async function toggleMicrophone() {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      const modal = document.getElementById('secure-modal');
      if (modal) modal.style.display = 'flex';
      return;
    }
    if (isListening) {
      stopAllAudio();
      return;
    }
    try {
      audioStream = await navigator.mediaDevices.getUserMedia({ audio: true });
      setupStreamAudio(audioStream, "🎙️ Live Microphone Active");
      const ml = document.getElementById('mic-btn-label');
      if (ml) ml.textContent = 'Stop Microphone Scanner';
    } catch(err) {
      if (err.name === 'NotAllowedError' || err.name === 'PermissionDeniedError') {
        alert("🎤 Microphone Access Denied!\n\nYour browser or Windows settings have microphone blocked for localhost:8000.\n\nTo allow it in Chrome / Edge:\n1. Click the 'View site info' / tune icon (left of the address bar 'localhost:8000').\n2. Toggle 'Microphone' to ALLOW.\n3. Refresh this page!\n\n💡 Tip: You can also use 'Sync Laptop Spotify Directly' or load an MP3 file directly without needing microphone access!");
      } else {
        alert("Microphone Notice: " + err.message);
      }
    }
  }

  function setupStreamAudio(stream, label) {
    try {
      initWebAudio();
      let src = audioCtx.createMediaStreamSource(stream);
      src.connect(analyser);
      isListening = true;
      const hud = document.getElementById('hud-status');
      if (hud) hud.textContent = label;
      setMode(11);
      startBackgroundAudioWorker();
      processDynamicAudioLoop();
    } catch(e) {
      console.error("setupStreamAudio error:", e);
      alert("Audio Stream Notice: " + e.message);
    }
  }

  // ====================================================================
  // 🎧 REAL FFT DSP PROCESSOR (GRADED VELOCITY, DYNAMIC AGC, ZERO-DROP QUEUE)
  // ====================================================================
  let silenceFrames = 0;
  let isCurrentlySilent = false;
  let maxKickObserved = 60;
  let maxMidObserved = 50;
  let avgKickEnvelope = 25;
  let avgMidEnvelope = 20;
  let lastKickEnergy = 0;
  let lastMidEnergy = 0;
  let recentEnergy = [];
  let beatSensitivityLevel = 3; // 1 to 5 (default 3 - Normal Adaptive)

  function onSensitivityChange(val) {
    beatSensitivityLevel = parseInt(val);
    const labels = {
      1: "1 - Very Subtle (Only Peak Drops)",
      2: "2 - Gentle (Smooth Groove)",
      3: "3 - Normal Adaptive (Recommended)",
      4: "4 - Dynamic Punch (Very Reactive)",
      5: "5 - Maximum Sensitivity (Ultra-Fast)"
    };
    let sensDisp = document.getElementById('sens-display');
    if (sensDisp) sensDisp.textContent = labels[beatSensitivityLevel] || (val + '/5');
  }

  function processDynamicAudioLoop() {
    if (!isListening) {
      isDynamicLoopQueued = false;
      return;
    }

    lastDynamicStepTime = performance.now();
    processDynamicAudioFrame(false);

    if (!document.hidden) {
      isDynamicLoopQueued = true;
      requestAnimationFrame(processDynamicAudioLoop);
    } else {
      isDynamicLoopQueued = false;
    }
  }

  function processDynamicAudioFrame(isBackground = false) {
    if (!isListening || !analyser) return;

    let bufLen = analyser.frequencyBinCount;
    let dataArray = new Uint8Array(bufLen);
    analyser.getByteFrequencyData(dataArray);

    // 1. 4-Band Multi-Instrument Extraction:
    // Sub-bass & Kicks (Bins 0-3: ~0-280 Hz)
    let rawKick = (dataArray[0] + dataArray[1] + dataArray[2] + dataArray[3]) / 4;
    let bMult = (isCustomStudioActive && isBassCustomActive) ? (bassWeight * (isBassBoost ? 1.45 : 1.0)) : 1.0;
    let kick = Math.min(255, Math.round(rawKick * bMult));

    // Lead Vocals & Voice Formants (Bins 3-18: ~300-1400 Hz)
    let vocalSum = 0;
    for (let i = 3; i <= 18; i++) vocalSum += dataArray[i];
    let vocalRaw = vocalSum / 16;
    let vMult = (isCustomStudioActive && isVocalCustomActive) ? vocalWeight : 1.0;
    let vocals = Math.min(255, Math.round(vocalRaw * vMult));

    // Rhythm, Chords & Synths (Bins 6-22: ~500-1900 Hz)
    let midsSum = 0;
    for (let i = 6; i <= 22; i++) midsSum += dataArray[i];
    let mids = midsSum / 17;

    // Highs, Cymbals, Snares & Hi-Hats (Bins 23-50: ~2000-4300 Hz)
    let trebSum = 0;
    for (let i = 23; i <= 50; i++) trebSum += dataArray[i];
    let treble = trebSum / 28;

    let totalEnergy = (kick * 1.3 + vocals * 1.2 + mids * 1.0 + treble * 0.8) / 4.3;

    // Dynamic Automatic Gain Control (AGC) - Adapts to low or high Spotify volume
    maxKickObserved = Math.max(kick, maxKickObserved * 0.996);
    if (maxKickObserved < 20) maxKickObserved = 20;

    maxMidObserved = Math.max(mids, maxMidObserved * 0.996);
    if (maxMidObserved < 15) maxMidObserved = 15;

    avgKickEnvelope = avgKickEnvelope * 0.90 + kick * 0.10;
    avgMidEnvelope = avgMidEnvelope * 0.90 + mids * 0.10;

    // 2. Real-Time Musical Note & Pitch Detection
    let pitchInfo = detectPitch(dataArray, audioCtx ? audioCtx.sampleRate : 44100);
    detectedPitchNote = pitchInfo.note;
    detectedPitchFreq = pitchInfo.freq;
    detectedPitchMidi = pitchInfo.midi;
    detectedPitchNoteIndex = pitchInfo.noteIndex;

    let now = Date.now();
    let badge = document.getElementById('beat-status-badge');
    let spikePhaseBadge = document.getElementById('spike-phase-badge');

    if (!isBackground) {
      let livePitchEl = document.getElementById('live-pitch-note');
      if (livePitchEl) livePitchEl.textContent = `🎵 ${detectedPitchNote} (${detectedPitchFreq}Hz)`;
      let chromaNoteLive = document.getElementById('chroma-note-live');
      if (chromaNoteLive) chromaNoteLive.textContent = `🎵 ${detectedPitchNote} • ${detectedPitchFreq} Hz`;
    }

    // 3. Auto Feeling AI Vibe Classification (Rolling Window)
    energyHistory.push(totalEnergy);
    vocalHistory.push(vocals);
    kickHistory.push(kick);
    if (energyHistory.length > 50) energyHistory.shift();
    if (vocalHistory.length > 50) vocalHistory.shift();
    if (kickHistory.length > 50) kickHistory.shift();

    let avgE = energyHistory.reduce((a, b) => a + b, 0) / energyHistory.length;
    let avgV = vocalHistory.reduce((a, b) => a + b, 0) / vocalHistory.length;
    let avgK = kickHistory.reduce((a, b) => a + b, 0) / kickHistory.length;
    let varE = energyHistory.reduce((a, b) => a + Math.pow(b - avgE, 2), 0) / energyHistory.length;

    if (avgV > 38 && avgV > avgK * 1.15) {
      detectedFeeling = 'Vocal Ballad';
    } else if (varE > 320 && avgK > 48) {
      detectedFeeling = 'EDM Festival';
    } else if (avgK > 55 && avgK > avgV * 1.25) {
      detectedFeeling = 'Trap 808 Bass';
    } else if (avgE < 28 && varE < 75) {
      detectedFeeling = 'Chill Lo-Fi';
    } else if (mids > 40 && kick > 25) {
      detectedFeeling = 'Funk Groove';
    } else {
      detectedFeeling = 'Cinematic Suspense';
    }

    let feelingBadge = document.getElementById('auto-feeling-badge');
    if (feelingBadge && !isBackground) feelingBadge.textContent = `🎭 ${detectedFeeling}`;

    // 🛑 4. TRUE DIGITAL SILENCE / SPOTIFY PAUSE DETECTION
    if (totalEnergy < 2.5) {
      silenceFrames++;
      if (silenceFrames >= 20) {
        let finalSilence = applyBrightnessRange(0, true);
        if (!isBackground) {
          updateMeters(0, 0, 0, 0);
          drawPausedSpectrum(canvasCtx, canvas);
          if (badge) {
            badge.className = 'beat-badge drop';
            badge.textContent = keepFloorOnSilence ? '⏸️ SPOTIFY PAUSED (FLOOR AMBIENT)' : '⏸️ SPOTIFY PAUSED (LED OFF)';
          }
          if (spikePhaseBadge) {
            spikePhaseBadge.textContent = '⚡ Spike: Idle';
            spikePhaseBadge.style.color = '#94a3b8';
          }
          updateVirtualLed(0, 0, 0, finalSilence);
          updateRangeNeedle(finalSilence);
        } else {
          document.title = '⏸️ [PAUSED] ESP32 RGB PRO';
        }

        if (!isCurrentlySilent) {
          isCurrentlySilent = true;
          dispatchDynamicBeat(0, 0, 0, finalSilence, 60);
        }
        return;
      }
    } else {
      silenceFrames = 0;
      isCurrentlySilent = false;
    }

    // 5. DRAW ACTIVE SPECTRUM BARS (When tab visible)
    if (!isBackground) {
      canvasCtx.fillStyle = '#05070c';
      canvasCtx.fillRect(0, 0, canvas.width, canvas.height);
      let barWidth = (canvas.width / 80) * 1.8;
      let x = 0;
      for (let i = 0; i < 80; i++) {
        let barHeight = (dataArray[i] / 255) * canvas.height;
        canvasCtx.fillStyle = `hsl(${modeHue + (i * 2.5)}, 100%, 55%)`;
        canvasCtx.fillRect(x, canvas.height - barHeight, barWidth, barHeight);
        x += barWidth + 1.2;
      }
      updateMeters(kick, vocals, mids, treble);
    }

    recentEnergy.push(totalEnergy);
    if (recentEnergy.length > 25) recentEnergy.shift();
    let avgRecent = recentEnergy.reduce((a, b) => a + b, 0) / recentEnergy.length;

    // Rolling snapshot recording for Gemini context & music evolution
    if (now - lastSnapshotTime > 1200) {
      lastSnapshotTime = now;
      aiMusicHistory.push({
        bass: Number((kick / 255).toFixed(2)),
        mid: Number((mids / 255).toFixed(2)),
        treble: Number((treble / 255).toFixed(2)),
        energy: Number((totalEnergy / 255).toFixed(2))
      });
      if (aiMusicHistory.length > 8) aiMusicHistory.shift();

      // Periodic AI heartbeat if listening in AI mode
      if (activeMusicMode === 'ai' && (now - lastAiRequestTime > 12000)) {
        requestGeminiAiUpdate(null, false);
      }
    }

    // 6. STAGE 1 (BEFORE SPIKE): TENSION PRE-DROP RADAR
    let energyDelta = totalEnergy - avgRecent;
    if (enableSpikeAnticipation && energyDelta > 14 && totalEnergy > 26 && (now - lastBeatTimestamp > 110)) {
      if (activeMusicMode === 'ai' && energyDelta > 16 && (now - lastAiRequestTime > 8000)) {
        requestGeminiAiUpdate(null, false);
      }
      if (spikePhaseBadge && !isBackground) {
        spikePhaseBadge.textContent = '⚡ Stage 1: Tension Radar (Pre-Drop)';
        spikePhaseBadge.style.color = '#fde68a';
      }
      let preIntensity = applyBrightnessRange(Math.min(130, Math.round(totalEnergy * 1.4)), false);
      let [pr, pg, pb] = getMusicModeColor(false, kick, vocals, mids, treble);
      dispatchDynamicBeat(pr, pg, pb, preIntensity, 110);
      if (!isBackground) {
        updateVirtualLed(pr, pg, pb, preIntensity);
        updateRangeNeedle(preIntensity);
      }
    }

    // 7. SUDDEN MUSICAL BEAT DROP (Dramatic Blackout on Big Drop)
    let isDrop = (recentEnergy.length > 12 && totalEnergy < avgRecent * 0.35 && totalEnergy < 25);
    if (isDrop) {
      if (activeMusicMode === 'ai' && (now - lastAiRequestTime > 7000)) {
        requestGeminiAiUpdate(null, false);
      }
      let finalDrop = applyBrightnessRange(0, false);
      if (!isBackground) {
        if (badge) {
          badge.className = 'beat-badge drop';
          badge.textContent = '🤫 BEAT DROP (BLACKOUT)';
        }
        if (spikePhaseBadge) {
          spikePhaseBadge.textContent = '🤫 Drop Blackout';
          spikePhaseBadge.style.color = '#94a3b8';
        }
        updateVirtualLed(0, 0, 0, finalDrop);
        updateRangeNeedle(finalDrop);
      } else {
        document.title = '🤫 [DROP] ESP32 RGB PRO';
      }
      dispatchDynamicBeat(0, 0, 0, finalDrop, 70);
    }
    // 8. MULTI-LEVEL DYNAMIC BEAT & SPIKE DETECTION (Low Beats, Snares, Vocals, and Heavy Kicks)
    else {
      let kickRatio = kick / Math.max(2, avgKickEnvelope);
      let midRatio = mids / Math.max(2, avgMidEnvelope);
      let normKick = Math.min(1.0, kick / maxKickObserved);
      let normMid = Math.min(1.0, mids / maxMidObserved);

      let sensFactors = [0.7, 0.75, 0.9, 1.0, 1.2, 1.45];
      let sens = sensFactors[beatSensitivityLevel] || 1.0;

      // Detection tests:
      let isHeavyKick = (kickRatio > (1.18 / sens) && kick > 12) || (normKick > 0.72);
      let isSnareClap = !isHeavyKick && ((midRatio > (1.14 / sens) && mids > 10) || (normMid > 0.65));
      let isVocalLeadHit = !isHeavyKick && !isSnareClap && (isCustomStudioActive && isVocalCustomActive && isVocalSpotlight) && (vocals > 45 && vocals > kick * 1.1);
      let isLowBeat   = !isHeavyKick && !isSnareClap && !isVocalLeadHit && ((kickRatio > (1.05 / sens) && kick > 6) || (normKick > 0.28 && (now - lastBeatTimestamp > 140)));

      let beatHit = isHeavyKick || isSnareClap || isVocalLeadHit || isLowBeat;
      let minGap = isHeavyKick ? 90 : (isSnareClap ? 70 : 50);

      if (beatHit && (now - lastBeatTimestamp > minGap)) {
        lastBeatTimestamp = now;

        let rawIntensity = 0;
        let decayMs = 0;
        let [r, g, b] = [255, 255, 255];

        if (isHeavyKick) {
          // 🔥 STAGE 2 (DURING SPIKE): Explosive Impact
          rawIntensity = Math.round(210 + normKick * 45);
          // Prioritize dynamic multicolors; white strobe fires only when explicitly enabled AND on extreme drops (>94%) AND not in chroma pitch mode
          if (enableSpikeWhiteStrobe && !enableSpikeMulticolor && normKick > 0.94 && activeMusicMode !== 'pitch') {
            [r, g, b] = [255, 255, 255]; // Crisp white flash on peak kick drop
          } else {
            [r, g, b] = getMusicModeColor(true, kick, vocals, mids, treble);
          }
          if (spikePhaseBadge && !isBackground) {
            spikePhaseBadge.textContent = '💥 Stage 2: Climax Spike Impact!';
            spikePhaseBadge.style.color = '#ef4444';
          }
          if (!isBackground && badge) {
            badge.className = 'beat-badge peak';
            badge.textContent = `🔥 HEAVY BEAT (Kick Peak ${rawIntensity})`;
          } else {
            document.title = '🔥 [BEAT] ESP32 RGB PRO';
          }
        } else if (isVocalLeadHit) {
          // 🎙️ VOCALIST SPOTLIGHT HIT
          rawIntensity = Math.round(170 + (vocals / 255.0) * 80);
          [r, g, b] = [244, 63, 94]; // Rose singer spotlight
          if (spikePhaseBadge && !isBackground) {
            spikePhaseBadge.textContent = '🎤 Stage 2: Vocalist Spotlight Hit';
            spikePhaseBadge.style.color = '#f472b6';
          }
          if (!isBackground && badge) {
            badge.className = 'beat-badge normal';
            badge.textContent = `🎙️ LEAD VOCAL (Bloom ${rawIntensity})`;
          } else {
            document.title = '🎤 [VOCAL] ESP32 RGB PRO';
          }
        } else if (isSnareClap) {
          // 🥁 SNARE / RHYTHM: Crisp, snappy strike
          rawIntensity = Math.round(140 + normMid * 60);
          [r, g, b] = getMusicModeColor(false, kick, vocals, mids, treble);
          if (spikePhaseBadge && !isBackground) {
            spikePhaseBadge.textContent = '⚡ Stage 2: Snare / Clap Hit';
            spikePhaseBadge.style.color = '#38bdf8';
          }
          if (!isBackground && badge) {
            badge.className = 'beat-badge normal';
            badge.textContent = `🥁 SNARE / CLAP (Power ${rawIntensity})`;
          } else {
            document.title = '✨ [SNARE] ESP32 RGB PRO';
          }
        } else {
          // 🫧 LOW / SOFT BEAT: Subtle mellow groove
          rawIntensity = Math.round(75 + normKick * 60);
          [r, g, b] = getMusicModeColor(false, kick, vocals, mids, treble);
          if (spikePhaseBadge && !isBackground) {
            spikePhaseBadge.textContent = '🫧 Stage 2: Groove Rhythm';
            spikePhaseBadge.style.color = '#a5b4fc';
          }
          if (!isBackground && badge) {
            badge.className = 'beat-badge normal';
            badge.textContent = `🫧 GROOVE BEAT (Glow ${rawIntensity})`;
          } else {
            document.title = '🎵 [GROOVE] ESP32 RGB PRO';
          }
        }

        // STAGE 3 (AFTER SPIKE): Customizable Harmonic Reverb Tail
        if (isCustomStudioActive && isSpikeCustomActive) {
          if (activeSpikeCurve === 'harmonic') {
            decayMs = Math.round(activeDecay * 1.15); // Smooth analog fade
          } else if (activeSpikeCurve === 'snappy') {
            decayMs = Math.round(activeDecay * 0.55); // Rapid club drop-off
          } else if (activeSpikeCurve === 'ripple') {
            decayMs = Math.round(activeDecay * 0.80);
            // Secondary echo ripple bounce
            let rippleIntensity = applyBrightnessRange(Math.round(rawIntensity * 0.55), false);
            setTimeout(() => {
              dispatchDynamicBeat(r, g, b, rippleIntensity, 180);
            }, 130);
          } else if (activeSpikeCurve === 'bloom') {
            decayMs = Math.round(activeDecay * 1.45); // Lingering vocal bloom
          }
        } else {
          decayMs = activeDecay;
        }

        if (activeMusicMode === 'ai') {
          decayMs = Math.max(120, Math.round(decayMs * (1.5 - aiLightingState.movement_speed * 0.7)));
          rawIntensity = Math.min(255, Math.max(20, Math.round(rawIntensity * aiLightingState.brightness * (aiLightingState.beat_reactivity * 0.4 + 0.6))));
        }

        // Apply User's Custom Brightness Range [Floor% - Ceiling%]
        let finalIntensity = applyBrightnessRange(rawIntensity, false);

        if (!isBackground) {
          updateVirtualLed(r, g, b, finalIntensity);
          updateRangeNeedle(finalIntensity);
        }

        if (syncOffsetMs > 0) {
          setTimeout(() => { dispatchDynamicBeat(r, g, b, finalIntensity, decayMs); }, syncOffsetMs);
        } else {
          dispatchDynamicBeat(r, g, b, finalIntensity, decayMs);
        }
      } else if (now - lastBeatTimestamp > 180 && totalEnergy > 6) {
        // Continuous ambient musical breathing pulse so LED stays alive during musical flow
        lastBeatTimestamp = now;
        let [r, g, b] = getMusicModeColor(false, kick, vocals, mids, treble);
        let ambientRaw = Math.min(65, Math.max(30, Math.round(totalEnergy * 0.8)));
        if (activeMusicMode === 'ai') {
          ambientRaw = Math.min(255, Math.round(ambientRaw * aiLightingState.brightness));
        }
        let finalAmbient = applyBrightnessRange(ambientRaw, false);
        if (!isBackground) {
          updateVirtualLed(r, g, b, finalAmbient);
          updateRangeNeedle(finalAmbient);
        }
        dispatchDynamicBeat(r, g, b, finalAmbient, 240);
      }
    }
  }

  function drawPausedSpectrum(ctx, c) {
    ctx.fillStyle = '#05070c';
    ctx.fillRect(0, 0, c.width, c.height);
    ctx.strokeStyle = 'rgba(239, 68, 68, 0.45)';
    ctx.lineWidth = 1.5;
    let mid = c.height / 2;
    ctx.beginPath();
    ctx.moveTo(0, mid);
    ctx.lineTo(c.width, mid);
    ctx.stroke();

    ctx.fillStyle = 'rgba(248, 113, 113, 0.8)';
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('⏸️ SPOTIFY PAUSED • No Audio (LED Off)', c.width / 2, mid + 16);
  }

  let lastDynamicBeatSent = 0;
  let pendingDynamicBeat = null;
  let pendingBeatTimeout = null;

  function dispatchDynamicBeat(r, g, b, v, d) {
    // 1. If Bluetooth LE is connected: Instant sub-2ms direct characteristic write!
    if (bleConnected && bleRxCharacteristic) {
      sendBleCommand(`!B:${r},${g},${b},${v},${d}
`);
      return;
    }

    // 2. Over HTTP / USB Serial Bridge: Rate-limited to 38ms with zero-drop queue
    let now = performance.now();
    if (now - lastDynamicBeatSent < 38) {
      if (!pendingDynamicBeat || v === 0 || v > pendingDynamicBeat.v) {
        pendingDynamicBeat = { r, g, b, v, d };
        if (!pendingBeatTimeout) {
          let delayMs = Math.max(1, Math.round(38 - (now - lastDynamicBeatSent)));
          pendingBeatTimeout = setTimeout(() => {
            pendingBeatTimeout = null;
            if (pendingDynamicBeat) {
              let p = pendingDynamicBeat;
              pendingDynamicBeat = null;
              dispatchDynamicBeat(p.r, p.g, p.b, p.v, p.d);
            }
          }, delayMs);
        }
      }
      return;
    }

    lastDynamicBeatSent = now;
    fetch(`/api/beat?r=${r}&g=${g}&b=${b}&v=${v}&d=${d}`, { keepalive: true }).catch(() => {});
  }

  function sendDynamicBeat(r, g, b, v, d) {
    dispatchDynamicBeat(r, g, b, v, d);
  }

  // ====================================================================
  // 🤖 GEMINI AI DIRECTOR
  // ====================================================================
  function setAndAskGemini(promptText) {
    document.getElementById('ai-prompt-input').value = promptText;
    askGeminiPrompt();
  }

  async function askGeminiPrompt() {
    let key = document.getElementById('gemini-key').value.trim();
    let prompt = document.getElementById('ai-prompt-input').value.trim();
    let bubble = document.getElementById('ai-bubble');
    let titleEl = document.getElementById('ai-title');
    let descEl = document.getElementById('ai-desc');
    let btn = document.getElementById('btn-ai-send');

    if (!prompt) {
      alert('Please type any song, mood, scene or vibe in the box or click a quick prompt!');
      return;
    }

    bubble.style.display = 'block';
    titleEl.textContent = '✨ Gemini AI is analyzing & choreographing...';
    descEl.textContent = 'Composing unique lighting recipe with Gemini 3.5 Flash...';
    btn.disabled = true;

    let instruction = "You are a professional lighting designer for an ESP32 RGB LED. The user wants lighting for this concept, scene, song, or atmosphere: '" + prompt + "'.\nCreate a synchronized RGB lighting sequence (between 4 and 10 steps) matching the exact tempo, genre, rhythm, and color atmosphere.\n\nRespond ONLY with valid JSON in this exact structure without markdown backticks:\n{\n  \"moodTitle\": \"Short creative title\",\n  \"bpm\": 120,\n  \"vibe\": \"One sentence describing the lighting concept\",\n  \"steps\": [\n    {\"r\": 255, \"g\": 0, \"b\": 50, \"ms\": 400, \"fade\": 1},\n    {\"r\": 0, \"g\": 10, \"b\": 80, \"ms\": 500, \"fade\": 0}\n  ]\n}";

    try {
      let res = await fetch('https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent?key=' + key, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          contents: [{ parts: [{ text: instruction }] }],
          generationConfig: { responseMimeType: 'application/json' }
        })
      });

      if (!res.ok) throw new Error("API returned status " + res.status);
      let data = await res.json();
      let rawText = data.candidates[0].content.parts[0].text;
      let cleanJson = rawText.replace(/```json/g, '').replace(/```/g, '').trim();
      let recipe = JSON.parse(cleanJson);

      titleEl.textContent = '🎬 ' + recipe.moodTitle + ' (' + (recipe.bpm || 120) + ' BPM)';
      descEl.textContent = recipe.vibe;

      timeline = recipe.steps.map(s => ({
        r: s.r, g: s.g, b: s.b, ms: s.ms || 500, fade: (s.fade !== undefined ? s.fade : 1)
      }));
      renderTimeline();

      if (recipe.bpm) {
        currentBPM = recipe.bpm;
        document.getElementById('bpm-slider').value = currentBPM;
        document.getElementById('bpm-display').textContent = currentBPM + ' BPM';
      }

      playCustomSequence();
      document.getElementById('active-mode-title').textContent = 'AI: ' + recipe.moodTitle;
    } catch(err) {
      titleEl.textContent = '⚠️ Generation Note';
      descEl.textContent = err.message + '. (If offline, use the 20 built-in presets or Studio Builder!)';
    } finally {
      btn.disabled = false;
    }
  }

  // ====================================================================
  // 🛠️ CUSTOM SEQUENCE BUILDER
  // ====================================================================
  let timeline = [
    { r: 255, g: 0, b: 120, ms: 800, fade: 1 },
    { r: 0, g: 240, b: 255, ms: 800, fade: 1 },
    { r: 160, g: 0, b: 255, ms: 600, fade: 0 }
  ];

  function renderTimeline() {
    let list = document.getElementById('timeline-list');
    list.innerHTML = '';
    timeline.forEach((step, idx) => {
      let div = document.createElement('div');
      div.className = 'timeline-item';
      div.style.background = `rgb(${step.r},${step.g},${step.b})`;
      div.innerHTML = `<span>${step.ms}ms</span><span style="font-size:9px;">${step.fade ? 'Fade' : 'Snap'}</span><div class="del" onclick="deleteStep(${idx})">×</div>`;
      list.appendChild(div);
    });
  }

  function addCurrentStep() {
    let hex = document.getElementById('step-color').value;
    let ms = parseInt(document.getElementById('step-ms').value) || 1000;
    let fade = parseInt(document.getElementById('step-fade').value);
    let r = parseInt(hex.slice(1, 3), 16);
    let g = parseInt(hex.slice(3, 5), 16);
    let b = parseInt(hex.slice(5, 7), 16);
    timeline.push({ r, g, b, ms, fade });
    renderTimeline();
  }

  function deleteStep(idx) {
    timeline.splice(idx, 1);
    renderTimeline();
  }

  function clearTimeline() {
    timeline = [];
    renderTimeline();
  }

  function playCustomSequence() {
    if (timeline.length === 0) {
      alert('Add at least 1 step to the timeline!');
      return;
    }
    let parts = timeline.map(s => `${s.r},${s.g},${s.b},${s.ms},${s.fade}`);
    sendHardwareCommand('!SEQ:' + parts.join(';'), '/api/custom/set?steps=' + encodeURIComponent(parts.join(';')));
    setMode(-2);
  }

  // ====================================================================
  // 🌐 HOME WI-FI SETUP CALLS
  // ====================================================================
  function saveAndConnectHomeWifi() {
    let ssid = document.getElementById('wifi-ssid-input').value.trim();
    let pass = document.getElementById('wifi-pass-input').value.trim();
    let msgEl = document.getElementById('wifi-status-msg');

    if (!ssid) {
      alert('Please enter your Home Wi-Fi SSID / Name!');
      return;
    }

    msgEl.innerHTML = '<b style="color:#f59e0b;">⏳ Saving and connecting ESP32 to ' + ssid + '...</b>';
    fetch(`/api/wifi?ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`)
      .then(res => res.text())
      .then(txt => {
        msgEl.innerHTML = '<b style="color:#10b981;">✅ Saved! ESP32 is connecting to ' + ssid + '. Check serial or mDNS at http://esp32-rgb.local</b>';
      })
      .catch(err => {
        msgEl.innerHTML = '<b style="color:#ef4444;">Error: ' + err.message + '</b>';
      });
  }

  renderTimeline();
  updateRangeWindowUI();
</script>
</body>
</html>
)rawliteral";

// ====================================================================
// 🌐 WEB SERVER API HANDLERS
// ====================================================================
void handleRoot() {
    server.send(200, "text/html", INDEX_HTML);
}

void handleSetMode() {
    if (server.hasArg("val")) {
        int val = server.arg("val").toInt();
        if (val == 8) {
            autoCycle = true;
            currentMode = 1;
            lastAutoSwitch = millis();
        } else {
            autoCycle = false;
            currentMode = val;
            if (val == 0) setRGB(0, 0, 0);
        }
        server.send(200, "text/plain", "OK");
        Serial.printf("\r\n[Web] Switched to Mode %d\n\rEnter command >> ", currentMode);
    } else {
        server.send(400, "text/plain", "Missing val");
    }
}

void handleSetColor() {
    if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
        staticR = server.arg("r").toInt();
        staticG = server.arg("g").toInt();
        staticB = server.arg("b").toInt();
        currentMode = -1; // Solid Free Light / Lamp mode!
        autoCycle = false;
        setRGB(staticR, staticG, staticB);
        server.send(200, "text/plain", "OK");
        Serial.printf("\r\n[Web] Free Lamp Color: R=%d, G=%d, B=%d\n\rEnter command >> ", staticR, staticG, staticB);
    } else {
        server.send(400, "text/plain", "Missing RGB");
    }
}

void handleSetBrightness() {
    if (server.hasArg("val")) {
        int val = server.arg("val").toInt();
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        masterBrightness = val;
        server.send(200, "text/plain", "OK");
        Serial.printf("\r\n[Web] Brightness: %d%%\n\rEnter command >> ", masterBrightness);
    } else {
        server.send(400, "text/plain", "Missing val");
    }
}

// Low-latency Dynamic Beat with Intensity (v) and Agile Decay (d)
void handleBeat() {
    if (server.hasArg("r")) beatR = server.arg("r").toInt();
    if (server.hasArg("g")) beatG = server.arg("g").toInt();
    if (server.hasArg("b")) beatB = server.arg("b").toInt();
    if (server.hasArg("v")) beatIntensity = server.arg("v").toInt(); else beatIntensity = 255;
    if (server.hasArg("d")) beatDecayMs = server.arg("d").toInt(); else beatDecayMs = 480;

    lastBeatTime = millis();
    currentMode = 11;
    autoCycle = false;
    server.send(200, "text/plain", "OK");
}

void handleSetCustomSequence() {
    if (server.hasArg("steps")) {
        String data = server.arg("steps");
        customStepCount = 0;

        int start = 0;
        while (start < data.length() && customStepCount < MAX_CUSTOM_STEPS) {
            int semi = data.indexOf(';', start);
            String item = (semi == -1) ? data.substring(start) : data.substring(start, semi);

            int c1 = item.indexOf(',');
            int c2 = item.indexOf(',', c1 + 1);
            int c3 = item.indexOf(',', c2 + 1);
            int c4 = item.indexOf(',', c3 + 1);

            if (c1 != -1 && c2 != -1 && c3 != -1 && c4 != -1) {
                customSteps[customStepCount].r = item.substring(0, c1).toInt();
                customSteps[customStepCount].g = item.substring(c1 + 1, c2).toInt();
                customSteps[customStepCount].b = item.substring(c2 + 1, c3).toInt();
                customSteps[customStepCount].durationMs = item.substring(c3 + 1, c4).toInt();
                customSteps[customStepCount].fade = (item.substring(c4 + 1).toInt() == 1);
                customStepCount++;
            }

            if (semi == -1) break;
            start = semi + 1;
        }

        currentCustomIndex = 0;
        stepStartTime = millis();
        currentMode = -2;
        autoCycle = false;

        server.send(200, "text/plain", "OK");
        Serial.printf("\r\n[Web] Loaded sequence with %d steps!\n\rEnter command >> ", customStepCount);
    } else {
        server.send(400, "text/plain", "Missing steps");
    }
}

// Wi-Fi Configuration API Handlers
void handleSetExtRgb() {
    if (server.hasArg("en")) {
        extRgbEnabled = (server.arg("en").toInt() != 0);
    }
    if (server.hasArg("mode")) {
        extRgbMode = (uint8_t)server.arg("mode").toInt();
    }
    if (server.hasArg("bri")) {
        extBrightness = (uint8_t)server.arg("bri").toInt();
    }
    server.send(200, "text/plain", "OK");
    Serial.printf("\r\n[Ext RGB] Enabled: %d, Mode: %d, Brightness: %d%%\n\rEnter command >> ", extRgbEnabled, extRgbMode, extBrightness);
}

void handleSetWiFi() {
    if (server.hasArg("ssid")) {
        String ssid = server.arg("ssid");
        String pass = server.hasArg("pass") ? server.arg("pass") : "";
        preferences.begin("wifi_cfg", false);
        preferences.putString("ssid", ssid);
        preferences.putString("pass", pass);
        preferences.end();
        server.send(200, "text/plain", "Connecting to Home Wi-Fi...");
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.printf("\r\n[Wi-Fi] Saved & Connecting to Home Wi-Fi: %s ...\n\rEnter command >> ", ssid.c_str());
    } else {
        server.send(400, "text/plain", "Missing ssid");
    }
}

// ====================================================================
// ⚡ HIGH-SPEED USB SERIAL PROTOCOL HANDLER
// ====================================================================
void processSerialProtocol(String cmd) {
    // !B:r,g,b,v,d
    if (cmd.startsWith("B:")) {
        int c1 = cmd.indexOf(',', 2);
        int c2 = cmd.indexOf(',', c1 + 1);
        int c3 = cmd.indexOf(',', c2 + 1);
        int c4 = cmd.indexOf(',', c3 + 1);
        if (c1 != -1 && c2 != -1 && c3 != -1 && c4 != -1) {
            beatR = cmd.substring(2, c1).toInt();
            beatG = cmd.substring(c1 + 1, c2).toInt();
            beatB = cmd.substring(c2 + 1, c3).toInt();
            beatIntensity = cmd.substring(c3 + 1, c4).toInt();
            beatDecayMs = cmd.substring(c4 + 1).toInt();
            lastBeatTime = millis();
            currentMode = 11;
            autoCycle = false;
        }
    }
    // !C:r,g,b
    else if (cmd.startsWith("C:")) {
        int c1 = cmd.indexOf(',', 2);
        int c2 = cmd.indexOf(',', c1 + 1);
        if (c1 != -1 && c2 != -1) {
            staticR = cmd.substring(2, c1).toInt();
            staticG = cmd.substring(c1 + 1, c2).toInt();
            staticB = cmd.substring(c2 + 1).toInt();
            currentMode = -1;
            autoCycle = false;
            setRGB(staticR, staticG, staticB);
        }
    }
    // !M:val
    else if (cmd.startsWith("M:")) {
        int val = cmd.substring(2).toInt();
        if (val == 8) {
            autoCycle = true;
            currentMode = 1;
            lastAutoSwitch = millis();
        } else {
            autoCycle = false;
            currentMode = val;
            if (val == 0) setRGB(0, 0, 0);
        }
    }
    // !BR:val
    else if (cmd.startsWith("BR:")) {
        int val = cmd.substring(3).toInt();
        if (val >= 0 && val <= 100) masterBrightness = val;
    }
    // !SEQ:steps
    else if (cmd.startsWith("SEQ:")) {
        String data = cmd.substring(4);
        customStepCount = 0;
        int start = 0;
        while (start < data.length() && customStepCount < MAX_CUSTOM_STEPS) {
            int semi = data.indexOf(';', start);
            String item = (semi == -1) ? data.substring(start) : data.substring(start, semi);
            int c1 = item.indexOf(',');
            int c2 = item.indexOf(',', c1 + 1);
            int c3 = item.indexOf(',', c2 + 1);
            int c4 = item.indexOf(',', c3 + 1);
            if (c1 != -1 && c2 != -1 && c3 != -1 && c4 != -1) {
                customSteps[customStepCount].r = item.substring(0, c1).toInt();
                customSteps[customStepCount].g = item.substring(c1 + 1, c2).toInt();
                customSteps[customStepCount].b = item.substring(c2 + 1, c3).toInt();
                customSteps[customStepCount].durationMs = item.substring(c3 + 1, c4).toInt();
                customSteps[customStepCount].fade = (item.substring(c4 + 1).toInt() == 1);
                customStepCount++;
            }
            if (semi == -1) break;
            start = semi + 1;
        }
        currentCustomIndex = 0;
        stepStartTime = millis();
        currentMode = -2;
        autoCycle = false;
    }
    // !EXT:en,mode,bri
    else if (cmd.startsWith("EXT:")) {
        int c1 = cmd.indexOf(',', 4);
        int c2 = cmd.indexOf(',', c1 + 1);
        if (c1 != -1) {
            extRgbEnabled = (cmd.substring(4, c1).toInt() != 0);
            if (c2 != -1) {
                extRgbMode = (uint8_t)cmd.substring(c1 + 1, c2).toInt();
                extBrightness = (uint8_t)cmd.substring(c2 + 1).toInt();
            } else {
                extRgbMode = (uint8_t)cmd.substring(c1 + 1).toInt();
            }
        }
    }
    // !EXTD:r0,g0,b0;r1,g1,b1;... (Direct Framebuffer Streaming)
    else if (cmd.startsWith("EXTD:")) {
        String data = cmd.substring(5);
        int start = 0;
        int idx = 0;
        while (start < data.length() && idx < 8) {
            int semi = data.indexOf(';', start);
            String item = (semi == -1) ? data.substring(start) : data.substring(start, semi);
            int c1 = item.indexOf(',');
            int c2 = item.indexOf(',', c1 + 1);
            if (c1 != -1 && c2 != -1) {
                extLeds[idx].r = item.substring(0, c1).toInt();
                extLeds[idx].g = item.substring(c1 + 1, c2).toInt();
                extLeds[idx].b = item.substring(c2 + 1).toInt();
                idx++;
            }
            if (semi == -1) break;
            start = semi + 1;
        }
        extRgbMode = 6;
    }
}

// ====================================================================
// 💬 TERMINAL INTERACTION
// ====================================================================
void printMenu() {
    Serial.println("\n\r==============================================");
    Serial.println("\r   ✨ ESP32-S3 AI RGB PRO STUDIO (20 MOODS)   ");
    Serial.println("\r==============================================");
    Serial.printf("\r AP URL: http://%s\n\r", WiFi.softAPIP().toString().c_str());
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\r Home Network URL: http://%s (or http://esp32-rgb.local)\n\r", WiFi.localIP().toString().c_str());
    }
    Serial.printf("\r Active Pin: GPIO %d | Brightness: %d%% | Ext 8-LED Bar: %s (Mode %d)\n\r", rgbPin, masterBrightness, extRgbEnabled ? "ON" : "OFF", extRgbMode);
    Serial.println("\r----------------------------------------------");
    Serial.println("\r [1] 🌌 Aurora Borealis      [11] 🪩 Disco Beat Reactive");
    Serial.println("\r [2] ⚡ Cyberpunk Pulse      [12] 🦇 Thriller Suspense");
    Serial.println("\r [3] 🔥 Campfire Ember       [13] 🧘 Peace / Zen Ambient");
    Serial.println("\r [4] 🌈 Rainbow 360 Wave     [14] 🌊 Ocean Waves");
    Serial.println("\r [5] 🚨 Emergency Strobe     [15] 🌋 Volcano Magma");
    Serial.println("\r [6] 💡 Breathing White      [16] 🌲 Enchanted Forest");
    Serial.println("\r [7] 🚥 Traffic Light        [17] 🕯️ Candlelight Flicker");
    Serial.println("\r [8] 🔄 Auto-Cycle 20 Moods  [18] 🌆 Neon Tokyo Synth");
    Serial.println("\r [9] ⚡ Lightning Storm      [19] 🧊 Glacier Ice Frost");
    Serial.println("\r [10] 🎆 Party Rave Beat     [20] 👾 Matrix Cyber Rain");
    Serial.println("\r [0] 🌑 Turn LED OFF         [+] / [-] Brightness");
    Serial.println("\r [pin <num>] Change GPIO Pin (e.g. 'pin 48')");
    Serial.println("\r [help] Show this menu");
    Serial.println("\r==============================================");
    Serial.print("\rEnter command >> ");
}

void processTerminalInput(String input) {
    input.trim();
    if (input.length() == 0) return;

    int num = input.toInt();
    if (num >= 0 && num <= 20 && (input == String(num))) {
        autoCycle = (num == 8);
        currentMode = (num == 8) ? 1 : num;
        if (num == 0) setRGB(0, 0, 0);
        Serial.printf("\r\n>> Switched to Mode %d!\n\r", num);
    } else if (input == "+") {
        if (masterBrightness <= 90) masterBrightness += 10; else masterBrightness = 100;
        Serial.printf("\r\n>> Brightness: %d%%\n\r", masterBrightness);
    } else if (input == "-") {
        if (masterBrightness >= 15) masterBrightness -= 10; else masterBrightness = 5;
        Serial.printf("\r\n>> Brightness: %d%%\n\r", masterBrightness);
    } else if (input.startsWith("ext ")) {
        String sub = input.substring(4);
        sub.trim();
        if (sub.equalsIgnoreCase("on")) {
            extRgbEnabled = true;
            Serial.println("\r\n>> External 8-LED Bar turned ON!");
        } else if (sub.equalsIgnoreCase("off")) {
            extRgbEnabled = false;
            Serial.println("\r\n>> External 8-LED Bar turned OFF!");
        } else if (sub.startsWith("mode ")) {
            int m = sub.substring(5).toInt();
            if (m >= 0 && m <= 5) {
                extRgbMode = m;
                Serial.printf("\r\n>> External 8-LED Bar Mode set to %d!\n\r", m);
            }
        } else if (sub.startsWith("bri ")) {
            int b = sub.substring(4).toInt();
            if (b >= 0 && b <= 100) {
                extBrightness = b;
                Serial.printf("\r\n>> External 8-LED Bar Brightness set to %d%%\n\r", b);
            }
        }
    } else if (input.startsWith("pin ")) {
        int newPin = input.substring(4).toInt();
        if (newPin >= 0 && newPin <= 48) {
            setRGB(0, 0, 0);
            rgbPin = newPin;
            Serial.printf("\r\n>> Inbuilt RGB Pin changed to GPIO %d!\n\r", rgbPin);
        } else {
            Serial.println("\r\n>> Invalid GPIO number!");
        }
    } else if (input.equalsIgnoreCase("help") || input == "?") {
        printMenu();
    } else {
        Serial.printf("\r\n>> Unknown command: '%s'. Type 'help' for menu.\n\r", input.c_str());
    }
}

void processIncomingChar(char c) {
    // Check for high-speed serial command starting with '!'
    if (isSerialCommand) {
        if (c == '\n' || c == '\r') {
            if (serialCmdBuffer.length() > 0) {
                processSerialProtocol(serialCmdBuffer);
                serialCmdBuffer = "";
            }
            isSerialCommand = false;
        } else {
            serialCmdBuffer += c;
        }
        return;
    }

    if (c == '!') {
        isSerialCommand = true;
        serialCmdBuffer = "";
        return;
    }

    // Normal terminal CLI handling
    if (c == '\r' || c == '\n') {
        if (terminalBuffer.length() > 0) {
            Serial.println();
            processTerminalInput(terminalBuffer);
            terminalBuffer = "";
            Serial.print("\r\nEnter command >> ");
        } else {
            printMenu();
        }
    } else if (c == 8 || c == 127) {
        if (terminalBuffer.length() > 0) {
            terminalBuffer.remove(terminalBuffer.length() - 1);
            Serial.print("\b \b");
        }
    } else {
        terminalBuffer += c;
        Serial.print(c);
        if (terminalBuffer.length() == 1) {
            char k = terminalBuffer[0];
            if ((k >= '0' && k <= '8') || k == '+' || k == '-') {
                Serial.println();
                processTerminalInput(terminalBuffer);
                terminalBuffer = "";
                Serial.print("\r\nEnter command >> ");
            }
        }
    }
}

// ====================================================================
// 🚀 ARDUINO SETUP & LOOP
// ====================================================================
void setup() {
    Serial.begin(115200);

    // 0. Thermal & Power Optimizations (Keeps ESP32 cool and prevents overheating!)
    setCpuFrequencyMhz(160); // 160 MHz: fast and punchy, yet generates 40% less heat than 240 MHz
    WiFi.setTxPower(WIFI_POWER_13dBm); // 13 dBm: cuts RF heating by ~40% while easily covering room
    WiFi.setSleep(true); // Enables 802.11 modem sleep between beacon intervals

    // 1. Wi-Fi Dual Mode: AP + Station
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_ssid, ap_pass);
    Serial.printf("\n\r[Wi-Fi AP] SSID: %s (Password: %s)\n\r", ap_ssid, ap_pass);
    Serial.printf("[Wi-Fi AP IP] http://%s\n\r", WiFi.softAPIP().toString().c_str());

    // Connect to saved Home Wi-Fi credentials if present (default: GUEST / iitram*123)
    preferences.begin("wifi_cfg", false);
    String savedSSID = preferences.getString("ssid", "GUEST");
    String savedPass = preferences.getString("pass", "iitram*123");
    preferences.end();

    if (savedSSID.length() > 0) {
        WiFi.begin(savedSSID.c_str(), savedPass.c_str());
        Serial.printf("[Wi-Fi STA] Connecting to Wi-Fi '%s' ...\n\r", savedSSID.c_str());
    }

    // 2. Setup mDNS (allows http://esp32-rgb.local)
    if (MDNS.begin("esp32-rgb")) {
        Serial.println("[mDNS] Active! Hostname: http://esp32-rgb.local");
    }

    // 0b. Setup External 8-LED RGB Module (Keyes ARTOU LED RGB V2)
    for (int i = 0; i < 8; i++) {
        pinMode(extDigitPins[i], OUTPUT);
        digitalWrite(extDigitPins[i], HIGH); // Disable all PNP digits (Active LOW)
    }
    ledcSetup(EXT_LEDC_CH_R, EXT_LEDC_FREQ, EXT_LEDC_RES);
    ledcSetup(EXT_LEDC_CH_G, EXT_LEDC_FREQ, EXT_LEDC_RES);
    ledcSetup(EXT_LEDC_CH_B, EXT_LEDC_FREQ, EXT_LEDC_RES);
    ledcAttachPin(PIN_EXT_R, EXT_LEDC_CH_R);
    ledcAttachPin(PIN_EXT_G, EXT_LEDC_CH_G);
    ledcAttachPin(PIN_EXT_B, EXT_LEDC_CH_B);
    ledcWrite(EXT_LEDC_CH_R, 255);
    ledcWrite(EXT_LEDC_CH_G, 255);
    ledcWrite(EXT_LEDC_CH_B, 255);

    // Launch FreeRTOS 1ms Multiplexer Task on Core 1
    xTaskCreatePinnedToCore(extRgbMultiplexTask, "extRgbMux", 2048, NULL, 3, NULL, 1);
    Serial.println("[Ext RGB] Initialized! 8-LED Multiplexer running at 125 FPS.");

    // 3. Setup Web Server Endpoints
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/ext", HTTP_GET, handleSetExtRgb);
    server.on("/api/mode", HTTP_GET, handleSetMode);
    server.on("/api/color", HTTP_GET, handleSetColor);
    server.on("/api/brightness", HTTP_GET, handleSetBrightness);
    server.on("/api/beat", HTTP_GET, handleBeat);
    server.on("/api/custom/set", HTTP_GET, handleSetCustomSequence);
    server.on("/api/wifi", HTTP_GET, handleSetWiFi);
    server.enableCORS(true);
    server.begin();
    Serial.println("[Web] HTTP Server active on port 80 with CORS!");

    // 4. Setup Bluetooth Low Energy (Nordic UART Service for Web Bluetooth)
    BLEDevice::init("ESP32-RGB-Studio");
    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new MyBleServerCallbacks());

    BLEService *pBleService = pBleServer->createService(BLE_SERVICE_UUID);
    pBleTxCharacteristic = pBleService->createCharacteristic(
        BLE_CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pBleTxCharacteristic->addDescriptor(new BLE2902());

    BLECharacteristic *pBleRxCharacteristic = pBleService->createCharacteristic(
        BLE_CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pBleRxCharacteristic->setCallbacks(new MyBleRxCallbacks());

    pBleService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    
    BLEAdvertisementData advData;
    advData.setFlags(0x06); // General Discoverable + BR/EDR not supported
    advData.setName("ESP32-RGB-Studio");

    BLEAdvertisementData scanData;
    scanData.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));
    scanData.setName("ESP32-RGB-Studio");

    pAdvertising->setAdvertisementData(advData);
    pAdvertising->setScanResponseData(scanData);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x20); // Low-heat advertising interval
    pAdvertising->setMaxPreferred(0x40);
    BLEDevice::startAdvertising();
    Serial.println("[BLE] Active! Advertising as 'ESP32-RGB-Studio' (Web Bluetooth Enabled)");
}

void loop() {
    unsigned long now = millis();

    // 1. Handle HTTP Requests
    server.handleClient();

    // 2. Announce Home Wi-Fi connection if established
    static bool staConnectedAnnounced = false;
    if (WiFi.status() == WL_CONNECTED && !staConnectedAnnounced) {
        staConnectedAnnounced = true;
        // Turn OFF SoftAP beaconing when connected to Wi-Fi to stop excess RF heat!
        WiFi.mode(WIFI_STA);
        Serial.printf("\n\r[Wi-Fi STA] Connected to Network! Local IP: http://%s\n\r", WiFi.localIP().toString().c_str());
        Serial.println("[Wi-Fi STA] SoftAP disabled to keep chip cool and silent.\n\rEnter command >> ");
    } else if (WiFi.status() != WL_CONNECTED && staConnectedAnnounced) {
        staConnectedAnnounced = false;
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(ap_ssid, ap_pass);
    }

    // 3. Bluetooth Reconnect Handling
    if (!bleDeviceConnected && oldBleDeviceConnected) {
        delay(20);
        pBleServer->startAdvertising();
        Serial.println("\r\n[BLE] Advertising restarted...");
        oldBleDeviceConnected = bleDeviceConnected;
    }
    if (bleDeviceConnected && !oldBleDeviceConnected) {
        oldBleDeviceConnected = bleDeviceConnected;
    }

    // 4. Auto-detect Serial connect & print menu
    if (Serial && !menuPrinted) {
        menuPrinted = true;
        delay(150);
        printMenu();
    }

    // 5. Process Serial Terminal & High-Speed USB Protocol
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        processIncomingChar(c);
    }

    // 6. Auto-Cycle Mode Switching
    if (autoCycle && (now - lastAutoSwitch >= AUTO_CYCLE_INTERVAL)) {
        lastAutoSwitch = now;
        currentMode = (currentMode % 20) + 1;
        Serial.printf("\r\n[Auto Cycle] >> Switched to Mode %d\n\r", currentMode);
        Serial.print("Enter command >> ");
    }

    // 7. Render Active Lighting Mode
    switch (currentMode) {
        case 1:  modeAurora(now); break;
        case 2:  modeCyberpunk(now); break;
        case 3:  modeCampfire(now); break;
        case 4:  modeRainbow(now); break;
        case 5:  modeStrobe(now); break;
        case 6:  modeBreathingWhite(now); break;
        case 7:  modeTrafficLight(now); break;
        case 9:  modeLightning(now); break;
        case 10: modePartyBeat(now); break;
        case 11: modeBeatReactive(now); break; // Dynamic Beat Reactive
        case 12: modeThriller(now); break;
        case 13: modePeace(now); break;
        case 14: modeOcean(now); break;
        case 15: modeVolcano(now); break;
        case 16: modeForest(now); break;
        case 17: modeCandle(now); break;
        case 18: modeNeonTokyo(now); break;
        case 19: modeGlacier(now); break;
        case 20: modeMatrix(now); break;
        case -1: setRGB(staticR, staticG, staticB); break; // Free Solid Light / Lamp
        case -2: runCustomSequence(now); break;
        case 0:  setRGB(0, 0, 0); break;
    }

    updateExternalRgbEffects(now);

    delay(4); // 250Hz cycle: responsive while letting FreeRTOS idle task run light sleep to keep chip cool
}
