#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>   // Hardware watchdog

// -------- Wi-Fi Config --------
const char* ssid     = "Rebellious Amish Family";
const char* password = "HenryOzzy423";

// -------- ESP32 Web Server --------
WebServer server(80);

// -------- WS2812B RGB LEDs --------
#define NUM_LEDS_PER_STRIP 1
const int pc1StatusLED  = 4;
const int pc2StatusLED  = 5;
const int wifiStatusLED = 18;

CRGB pc1Led[NUM_LEDS_PER_STRIP];
CRGB pc2Led[NUM_LEDS_PER_STRIP];
CRGB wifiLed[NUM_LEDS_PER_STRIP];

#define COLOR_OFF        CRGB::Black
#define COLOR_READY      CRGB::Green
#define COLOR_ACTIVE     CRGB::Blue
#define COLOR_ERROR      CRGB::Red
#define COLOR_WARNING    CRGB::Yellow
#define COLOR_CONNECTING CRGB::Purple

bool ledNeedsUpdate = false;

// -------- PC #1 (WoL) Config --------
const char* pc1MACStr = "9c:6b:00:96:b2:aa";
IPAddress   pc1IP(192, 168, 1, 69);
IPAddress   broadcastIP;
const int   WOLPort = 9;

WiFiUDP udp;

// -------- PC #2 (Relay) Config --------
const int powerPin = 2;
IPAddress pc2IP(192, 168, 1, 70);

// -------- Status Check Variables --------
unsigned long lastStatusCheck      = 0;
const unsigned long statusCheckInterval = 15000;
bool pc1Online       = false;
bool pc2Online       = false;
bool enableStatusCheck = true;

// -------- WiFi Stability --------
unsigned long lastWiFiCheck  = 0;
const unsigned long wifiCheckInterval = 30000;
unsigned long lastHeapLog    = 0;
const unsigned long heapLogInterval   = 60000;

// -------- Watchdog --------
#define WDT_TIMEOUT_SEC 30   // Reboot if loop stalls longer than this

// ============================================================
//  Helpers
// ============================================================

void parseMAC(const char* macStr, uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    mac[i] = (uint8_t)strtoul(macStr + i * 3, nullptr, 16);
  }
}

void setBroadcastIP() {
  IPAddress localIP = WiFi.localIP();
  IPAddress subnet  = WiFi.subnetMask();
  uint32_t broadcast = (uint32_t(localIP) & uint32_t(subnet)) | (~uint32_t(subnet));
  broadcastIP = IPAddress(broadcast);
  Serial.print("Broadcast IP: ");
  Serial.println(broadcastIP);
}

// Re-init UDP after every WiFi connect / reconnect
void initUDP() {
  udp.stop();
  udp.begin(WOLPort);
}

// ============================================================
//  LED helpers
// ============================================================

void setPC1Status(CRGB color) {
  if (pc1Led[0] != color) { pc1Led[0] = color; ledNeedsUpdate = true; }
}
void setPC2Status(CRGB color) {
  if (pc2Led[0] != color) { pc2Led[0] = color; ledNeedsUpdate = true; }
}
void setWiFiStatus(CRGB color) {
  if (wifiLed[0] != color) { wifiLed[0] = color; ledNeedsUpdate = true; }
}
void updateLEDs() {
  if (ledNeedsUpdate) { FastLED.show(); ledNeedsUpdate = false; }
}

void blinkLED(int ledType, CRGB color, int times = 3) {
  for (int i = 0; i < times; i++) {
    switch (ledType) {
      case 1: pc1Led[0] = color; break;
      case 2: pc2Led[0] = color; break;
      case 3: wifiLed[0] = color; break;
    }
    FastLED.show();
    delay(200);
    switch (ledType) {
      case 1: pc1Led[0] = COLOR_OFF; break;
      case 2: pc2Led[0] = COLOR_OFF; break;
      case 3: wifiLed[0] = COLOR_OFF; break;
    }
    FastLED.show();
    delay(200);
    esp_task_wdt_reset();  // Feed WDT during blink delays
  }
  ledNeedsUpdate = false;
}

// ============================================================
//  PC status check — FIXED: HTTPClient timeout, fresh clients
// ============================================================

// FIX: pingPC now creates a fresh WiFiClient each attempt
bool pingPC(IPAddress ip) {
  // Try port 80 first
  {
    WiFiClient client;
    client.setTimeout(300);
    bool ok = client.connect(ip, 80, 300);
    client.stop();
    if (ok) return true;
  }
  delay(30);
  // Try port 443
  {
    WiFiClient client;
    client.setTimeout(300);
    bool ok = client.connect(ip, 443, 300);
    client.stop();
    if (ok) return true;
  }
  return false;
}

void updatePCStatus() {
  if (!enableStatusCheck) return;
  unsigned long currentTime = millis();

  // Skip first 15 s after boot
  if (currentTime < 15000) {
    setPC1Status(COLOR_WARNING);
    setPC2Status(COLOR_WARNING);
    return;
  }
  if (currentTime - lastStatusCheck < statusCheckInterval) return;
  lastStatusCheck = currentTime;

  // --- PC1 via HTTP with explicit timeouts ---
  // FIX: setConnectTimeout + setTimeout prevent indefinite blocking
  bool newPC1 = false;
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.setConnectTimeout(3000);  // 3 s TCP connect timeout  <-- KEY FIX
    http.setTimeout(3000);         // 3 s response timeout     <-- KEY FIX
    http.begin("http://192.168.1.69:5000");
    int code = http.GET();
    newPC1 = (code == 200);
    http.end();
  }
  pc1Online = newPC1;
  setPC1Status(pc1Online ? COLOR_READY : COLOR_ERROR);

  // Feed WDT between the two network checks
  esp_task_wdt_reset();

  // --- PC2 via TCP ping ---
  bool newPC2 = (WiFi.status() == WL_CONNECTED) ? pingPC(pc2IP) : false;
  pc2Online = newPC2;
  setPC2Status(pc2Online ? COLOR_READY : COLOR_ERROR);
}

// ============================================================
//  WOL
// ============================================================

void sendWOL(const char* macStr) {
  setPC1Status(COLOR_ACTIVE);
  updateLEDs();
  Serial.println("Sending Wake-on-LAN packet...");

  uint8_t mac[6];
  parseMAC(macStr, mac);
  uint8_t packet[102];
  for (int i = 0; i < 6; i++) packet[i] = 0xFF;
  for (int i = 1; i <= 16; i++)
    for (int j = 0; j < 6; j++)
      packet[i * 6 + j] = mac[j];

  udp.beginPacket(broadcastIP, WOLPort);
  udp.write(packet, sizeof(packet));
  udp.endPacket();

  Serial.println("Magic packet sent!");
  blinkLED(1, COLOR_READY, 2);
  lastStatusCheck = 0;  // Force a status recheck soon
}

// ============================================================
//  Web handlers — chunked to avoid large String allocations
// ============================================================

void sendHTMLChunked() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent("<!DOCTYPE html><html lang=\"en\"><head>");
  server.sendContent("<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">");
  server.sendContent("<title>Wawtor Power Hub</title><style>");
  server.sendContent("*{margin:0;padding:0;box-sizing:border-box;}");
  server.sendContent("body{font-family:'Courier New',monospace;background:#000;min-height:100vh;");
  server.sendContent("display:flex;align-items:center;justify-content:center;padding:20px;}");
  server.sendContent(".container{background:rgba(0,20,0,0.9);border-radius:20px;");
  server.sendContent("border:2px solid #0f0;box-shadow:0 0 30px rgba(0,255,0,0.3);padding:40px;");
  server.sendContent("width:100%;max-width:500px;text-align:center;}");
  server.sendContent(".title{color:#0f0;font-size:2.5em;font-weight:700;margin-bottom:10px;text-shadow:0 0 10px #0f0;}");
  server.sendContent(".subtitle{color:#0a0;font-size:1.1em;margin-bottom:30px;}");
  server.sendContent(".pc-section{background:rgba(0,40,0,0.8);border-radius:15px;padding:25px;");
  server.sendContent("margin:20px 0;border:1px solid #0f0;}");
  server.sendContent(".pc-title{color:#0f0;font-size:1.3em;font-weight:600;margin-bottom:15px;}");
  server.sendContent(".btn{padding:12px 24px;font-size:16px;font-weight:600;border:2px solid #0f0;");
  server.sendContent("border-radius:10px;cursor:pointer;margin:5px;min-width:140px;");
  server.sendContent("font-family:'Courier New';background:rgba(0,20,0,0.95);color:#0f0;");
  server.sendContent("text-shadow:0 0 5px #0f0;box-shadow:0 0 10px rgba(0,255,0,0.3);}");
  server.sendContent(".btn:hover{transform:translateY(-2px);box-shadow:0 0 20px rgba(0,255,0,0.6);}");
  server.sendContent(".btn-power-off{border-color:#f00;color:#f00;text-shadow:0 0 5px #f00;}");
  server.sendContent("</style></head><body><div class=\"container\">");
  server.sendContent("<h1 class=\"title\">&#9889;Power HUB&#9889;</h1>");
  server.sendContent("<p class=\"subtitle\">Wawtor NETWORK</p>");
  server.sendContent("<p style=\"color:#0a0;font-size:0.9em;margin-bottom:20px;\">");
  server.sendContent("<a href='/update' style='color:#0a0;'>&#9881;&#65039; OTA UPDATE</a> | ");
  server.sendContent("<a href='/status' style='color:#0a0;'>&#128202; STATUS</a></p>");
  server.sendContent("<div class=\"pc-section\"><div class=\"pc-title\">&#128421;&#65039; TERMINAL 01 - WAKE</div>");
  server.sendContent("<button class=\"btn\" onclick=\"sendCmd('/pc1','WAKE SIGNAL SENT')\">ACTIVATE</button></div>");
  server.sendContent("<div class=\"pc-section\"><div class=\"pc-title\">&#128754; TERMINAL 02 - RELAY</div>");
  server.sendContent("<button class=\"btn\" onclick=\"sendCmd('/pc2/on','POWER ON EXECUTED')\">POWER ON</button>");
  server.sendContent("<button class=\"btn btn-power-off\" onclick=\"sendCmd('/pc2/off','SHUTDOWN INITIATED')\">POWER OFF</button>");
  server.sendContent("</div></div><script>");
  server.sendContent("async function sendCmd(e,m){try{const r=await fetch(e);");
  server.sendContent("alert(r.ok?m:'ERROR: '+await r.text());}catch(err){alert('NET ERROR: '+err.message);}}");
  server.sendContent("</script></body></html>");
  server.sendContent("");  // End chunked transfer
  // FIX: Removed server.client().stop() — was aborting the response prematurely
}

// FIX: Status page now uses chunked send instead of String concatenation
void sendStatusChunked() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent("<!DOCTYPE html><html><head><title>Status</title>");
  server.sendContent("<meta http-equiv='refresh' content='5'>");
  server.sendContent("<style>body{background:#000;color:#0f0;font-family:'Courier New';padding:20px;}</style></head><body>");
  server.sendContent("<h1>ESP32 Power Hub Status</h1>");

  char buf[80];

  snprintf(buf, sizeof(buf), "<p><strong>WiFi:</strong> %s</p>",
           WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  server.sendContent(buf);

  snprintf(buf, sizeof(buf), "<p><strong>IP:</strong> %s</p>", WiFi.localIP().toString().c_str());
  server.sendContent(buf);

  snprintf(buf, sizeof(buf), "<p><strong>Uptime:</strong> %lus</p>", millis() / 1000UL);
  server.sendContent(buf);

  snprintf(buf, sizeof(buf), "<p><strong>Free RAM:</strong> %u bytes</p>", ESP.getFreeHeap());
  server.sendContent(buf);

  snprintf(buf, sizeof(buf), "<p><strong>PC1:</strong> %s</p>", pc1Online ? "ONLINE" : "OFFLINE");
  server.sendContent(buf);

  snprintf(buf, sizeof(buf), "<p><strong>PC2:</strong> %s</p>", pc2Online ? "ONLINE" : "OFFLINE");
  server.sendContent(buf);

  server.sendContent("<br><a href='/' style='color:#0f0;'>&#8592; Back</a></body></html>");
  server.sendContent("");
}

// ============================================================
//  Setup
// ============================================================

void setup() {
  Serial.begin(115200);

  // FIX: Enable hardware watchdog — resets device if loop stalls > WDT_TIMEOUT_SEC
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);  // Subscribe main loop task

  FastLED.addLeds<WS2812B, pc1StatusLED,  GRB>(pc1Led,  NUM_LEDS_PER_STRIP);
  FastLED.addLeds<WS2812B, pc2StatusLED,  GRB>(pc2Led,  NUM_LEDS_PER_STRIP);
  FastLED.addLeds<WS2812B, wifiStatusLED, GRB>(wifiLed, NUM_LEDS_PER_STRIP);
  FastLED.setBrightness(50);

  pinMode(powerPin, OUTPUT);
  digitalWrite(powerPin, LOW);

  setPC1Status(COLOR_OFF);
  setPC2Status(COLOR_OFF);
  setWiFiStatus(COLOR_OFF);
  updateLEDs();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  setWiFiStatus(COLOR_CONNECTING);
  updateLEDs();

  int connectAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && connectAttempts < 40) {
    delay(500);
    Serial.print(".");
    connectAttempts++;
    esp_task_wdt_reset();
  }

  if (WiFi.status() == WL_CONNECTED) {
    setWiFiStatus(COLOR_ACTIVE);
    updateLEDs();
    Serial.println("\nWiFi connected!");
    Serial.println(WiFi.localIP());
    setBroadcastIP();
    initUDP();  // FIX: always init UDP after connecting
  } else {
    Serial.println("\nWiFi connection failed!");
    setWiFiStatus(COLOR_ERROR);
    updateLEDs();
  }

  ArduinoOTA.setHostname("PowerHub");
  ArduinoOTA.setPassword("Koda1234!");
  ArduinoOTA.begin();
  Serial.println("OTA Ready");

  server.on("/",      sendHTMLChunked);
  server.on("/status", sendStatusChunked);  // FIX: chunked, no String heap churn

  server.on("/update", HTTP_GET, []() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent("<!DOCTYPE html><html><head><title>OTA Update</title>");
    server.sendContent("<style>body{background:#000;color:#0f0;font-family:'Courier New';text-align:center;padding:50px;}");
    server.sendContent("input,button{background:#001100;color:#0f0;border:2px solid #0f0;padding:10px;margin:10px;}</style></head><body>");
    server.sendContent("<h1>&#9889; MATRIX OTA UPDATER &#9889;</h1>");
    server.sendContent("<form method='POST' action='/update' enctype='multipart/form-data'>");
    server.sendContent("<input type='file' name='update' accept='.bin' required><br>");
    server.sendContent("<button type='submit'>UPLOAD FIRMWARE</button></form>");
    server.sendContent("<p><a href='/' style='color:#0a0;'>&#8592; BACK</a></p></body></html>");
    server.sendContent("");
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAILED" : "SUCCESS - REBOOTING");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("OTA: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
      esp_task_wdt_reset();  // Feed WDT during potentially long upload
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.printf("OTA success: %u bytes\n", upload.totalSize);
      else Update.printError(Serial);
    }
  });

  server.on("/pc1", []() {
    sendWOL(pc1MACStr);
    server.send(200, "text/plain", "Wake-on-LAN sent!");
  });

  server.on("/pc2/on", []() {
    setPC2Status(COLOR_WARNING);
    updateLEDs();
    digitalWrite(powerPin, HIGH);
    delay(100);
    digitalWrite(powerPin, LOW);
    blinkLED(2, COLOR_READY);
    setPC2Status(COLOR_READY);
    updateLEDs();
    server.send(200, "text/plain", "PC2 Power ON sent!");
  });

  server.on("/pc2/off", []() {
    Serial.println("PC2 shutdown - 6 second hold");
    setPC2Status(COLOR_ERROR);
    updateLEDs();
    digitalWrite(powerPin, HIGH);
    // FIX: Feed WDT during the 6-second power hold so it doesn't reset the ESP
    for (int i = 0; i < 6; i++) {
      delay(1000);
      esp_task_wdt_reset();
    }
    digitalWrite(powerPin, LOW);
    Serial.println("Shutdown complete");
    blinkLED(2, COLOR_ERROR, 5);
    setPC2Status(COLOR_OFF);
    updateLEDs();
    server.send(200, "text/plain", "PC2 Power OFF sent!");
  });

  server.begin();
  Serial.println("Web server running!");
}

// ============================================================
//  Loop
// ============================================================

void loop() {
  unsigned long currentTime = millis();

  // Feed the hardware watchdog every iteration
  esp_task_wdt_reset();

  // Handle millis() overflow (every ~49 days)
  if (currentTime < lastWiFiCheck)  { lastWiFiCheck  = 0; }
  if (currentTime < lastHeapLog)    { lastHeapLog    = 0; }
  if (currentTime < lastStatusCheck){ lastStatusCheck = 0; }

  // --- WiFi monitoring ---
  if (currentTime - lastWiFiCheck >= wifiCheckInterval) {
    lastWiFiCheck = currentTime;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost! Reconnecting...");
      setWiFiStatus(COLOR_ERROR);
      updateLEDs();

      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(ssid, password);

      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
        esp_task_wdt_reset();  // Feed WDT during reconnect wait
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi reconnected!");
        setWiFiStatus(COLOR_ACTIVE);
        setBroadcastIP();
        initUDP();  // FIX: Re-init UDP after reconnect
      } else {
        Serial.println("WiFi reconnect failed.");
        setWiFiStatus(COLOR_ERROR);
      }
      updateLEDs();
    }
  }

  // --- Heap monitoring ---
  if (currentTime - lastHeapLog >= heapLogInterval) {
    lastHeapLog = currentTime;
    Serial.printf("Free heap: %u bytes | Uptime: %lus\n",
                  ESP.getFreeHeap(), currentTime / 1000UL);
  }

  // --- WiFi LED heartbeat ---
  static unsigned long lastWiFiLEDUpdate = 0;
  if (currentTime - lastWiFiLEDUpdate > 2000) {
    lastWiFiLEDUpdate = currentTime;
    setWiFiStatus(WiFi.status() == WL_CONNECTED ? COLOR_ACTIVE : COLOR_ERROR);
  }

  ArduinoOTA.handle();
  server.handleClient();
  updatePCStatus();
  updateLEDs();

  yield();
  delay(10);
}
