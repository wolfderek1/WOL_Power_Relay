#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <FastLED.h>
#include <HTTPClient.h>

// -------- Wi-Fi Config --------
const char* ssid = "Rebellious Amish Family";
const char* password = "HenryOzzy423";

// -------- ESP32 Web Server --------
WebServer server(80);

// -------- WS2812B RGB LEDs --------
#define NUM_LEDS_PER_STRIP 1
const int pc1StatusLED = 4;
const int pc2StatusLED = 5;
const int wifiStatusLED = 18;

CRGB pc1Led[NUM_LEDS_PER_STRIP];
CRGB pc2Led[NUM_LEDS_PER_STRIP];
CRGB wifiLed[NUM_LEDS_PER_STRIP];

// LED Colors
#define COLOR_OFF CRGB::Black
#define COLOR_READY CRGB::Green
#define COLOR_ACTIVE CRGB::Blue
#define COLOR_ERROR CRGB::Red
#define COLOR_WARNING CRGB::Yellow
#define COLOR_CONNECTING CRGB::Purple

// LED update flag to batch updates
bool ledNeedsUpdate = false;

// -------- PC #1 (WoL) Config --------
const char* pc1MACStr = "9c:6b:00:96:b2:aa";
IPAddress pc1IP(192, 168, 1, 69);
IPAddress broadcastIP;
const int WOLPort = 9;

WiFiUDP udp;

// -------- PC #2 (Relay) Config --------
const int powerPin = 2;
IPAddress pc2IP(192, 168, 1, 70);

// -------- Status Check Variables --------
unsigned long lastStatusCheck = 0;
const unsigned long statusCheckInterval = 15000; // Increased to 15 seconds
bool pc1Online = false;
bool pc2Online = false;
bool enableStatusCheck = true;

// -------- WiFi Stability --------
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 30000;
unsigned long lastHeapLog = 0;
const unsigned long heapLogInterval = 60000;

// Watchdog reset counter
unsigned long lastLoopTime = 0;

void parseMAC(const char* macStr, uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    mac[i] = strtoul(macStr + i*3, nullptr, 16);
  }
}

void setBroadcastIP() {
  IPAddress localIP = WiFi.localIP();
  IPAddress subnet = WiFi.subnetMask();
  uint32_t broadcast = (uint32_t(localIP) & uint32_t(subnet)) | (~uint32_t(subnet));
  broadcastIP = IPAddress(broadcast);
  Serial.print("Broadcast IP: "); Serial.println(broadcastIP);
}

// Improved ping with proper cleanup and timeout
bool pingPC(IPAddress ip) {
  WiFiClient client;
  client.setTimeout(300); // Shorter timeout
  bool result = false;
  
  // Try HTTP port
  if (client.connect(ip, 80, 300)) {
    result = true;
  }
  
  // Always ensure client is stopped
  client.stop();
  delay(50); // Give time for proper cleanup
  
  // If HTTP failed, try HTTPS
  if (!result) {
    if (client.connect(ip, 443, 300)) {
      result = true;
    }
    client.stop();
    delay(50);
  }
  
  return result;
}

void updatePCStatus() {
  if (!enableStatusCheck) return;
  unsigned long currentTime = millis();
  if (currentTime < lastStatusCheck) {
    lastStatusCheck = 0;
  }
  if (currentTime < 15000) {
    setPC1Status(COLOR_WARNING);
    setPC2Status(COLOR_WARNING);
    return;
  }
  if (currentTime - lastStatusCheck >= statusCheckInterval) {
    lastStatusCheck = currentTime;
    // Poll PC1 server at 192.168.1.69:5000
    bool newPC1Status = false;
    HTTPClient http;
    http.begin("http://192.168.1.69:5000");
    int httpCode = http.GET();
    if (httpCode > 0) {
      // If server responds with 200, PC is ON
      if (httpCode == 200) {
        newPC1Status = true;
      }
    }
    http.end();
    pc1Online = newPC1Status;
    setPC1Status(pc1Online ? COLOR_READY : COLOR_ERROR);

    // PC2 status (keep existing logic)
    bool newPC2Status = pingPC(pc2IP);
    pc2Online = newPC2Status;
    setPC2Status(pc2Online ? COLOR_READY : COLOR_ERROR);
  }
}

// Batch LED updates
void setPC1Status(CRGB color) {
  if (pc1Led[0] != color) {
    pc1Led[0] = color;
    ledNeedsUpdate = true;
  }
}

void setPC2Status(CRGB color) {
  if (pc2Led[0] != color) {
    pc2Led[0] = color;
    ledNeedsUpdate = true;
  }
}

void setWiFiStatus(CRGB color) {
  if (wifiLed[0] != color) {
    wifiLed[0] = color;
    ledNeedsUpdate = true;
  }
}

// Update all LEDs at once if needed
void updateLEDs() {
  if (ledNeedsUpdate) {
    FastLED.show();
    ledNeedsUpdate = false;
  }
}

void blinkLED(int ledType, CRGB color, int times = 3) {
  for (int i = 0; i < times; i++) {
    switch(ledType) {
      case 1: pc1Led[0] = color; break;
      case 2: pc2Led[0] = color; break;
      case 3: wifiLed[0] = color; break;
    }
    FastLED.show();
    delay(200);
    switch(ledType) {
      case 1: pc1Led[0] = COLOR_OFF; break;
      case 2: pc2Led[0] = COLOR_OFF; break;
      case 3: wifiLed[0] = COLOR_OFF; break;
    }
    FastLED.show();
    delay(200);
  }
  ledNeedsUpdate = false; // Reset flag after manual show
}

void sendWOL(const char* macStr) {
  setPC1Status(COLOR_ACTIVE);
  updateLEDs();
  
  Serial.println("Sending Wake-on-LAN packet...");
  
  uint8_t mac[6];
  parseMAC(macStr, mac);
  uint8_t packet[102];
  
  for (int i = 0; i < 6; i++) packet[i] = 0xFF;
  for (int i = 1; i <= 16; i++) {
    for (int j = 0; j < 6; j++) {
      packet[i*6 + j] = mac[j];
    }
  }
  
  udp.beginPacket(broadcastIP, WOLPort);
  udp.write(packet, sizeof(packet));
  udp.endPacket();
  
  Serial.println("Magic packet sent!");
  blinkLED(1, COLOR_READY, 2);
  
  lastStatusCheck = 0;
  updatePCStatus();
}

// Serve HTML in chunks to avoid large String allocation
void sendHTMLChunked() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  
  server.sendContent("<!DOCTYPE html><html lang=\"en\"><head>");
  server.sendContent("<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">");
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
  server.sendContent("<h1 class=\"title\">⚡Power HUB⚡</h1>");
  server.sendContent("<p class=\"subtitle\">Wawtor NETWORK</p>");
  server.sendContent("<p style=\"color:#0a0;font-size:0.9em;margin-bottom:20px;\">");
  server.sendContent("<a href='/update' style='color:#0a0;'>⚙️ OTA UPDATE</a> | ");
  server.sendContent("<a href='/status' style='color:#0a0;'>📊 STATUS</a></p>");
  server.sendContent("<div class=\"pc-section\"><div class=\"pc-title\">🖥️ TERMINAL 01 - WAKE</div>");
  server.sendContent("<button class=\"btn\" onclick=\"sendCmd('/pc1','WAKE SIGNAL SENT')\">ACTIVATE</button></div>");
  server.sendContent("<div class=\"pc-section\"><div class=\"pc-title\">🖲️ TERMINAL 02 - RELAY</div>");
  server.sendContent("<button class=\"btn\" onclick=\"sendCmd('/pc2/on','POWER ON EXECUTED')\">POWER ON</button>");
  server.sendContent("<button class=\"btn btn-power-off\" onclick=\"sendCmd('/pc2/off','SHUTDOWN INITIATED')\">POWER OFF</button>");
  server.sendContent("</div></div><script>");
  server.sendContent("async function sendCmd(e,m){try{const r=await fetch(e);");
  server.sendContent("alert(r.ok?m:'ERROR: '+await r.text());}catch(err){alert('NET ERROR: '+err.message);}}");
  server.sendContent("</script></body></html>");
  
  server.sendContent(""); // End chunked transfer
  server.client().stop();
}

void setup() {
  Serial.begin(115200);
  
  // Initialize FastLED
  FastLED.addLeds<WS2812B, pc1StatusLED, GRB>(pc1Led, NUM_LEDS_PER_STRIP);
  FastLED.addLeds<WS2812B, pc2StatusLED, GRB>(pc2Led, NUM_LEDS_PER_STRIP);
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
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    setWiFiStatus(COLOR_ACTIVE);
    updateLEDs();
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    setBroadcastIP();
  } else {
    Serial.println("\nWiFi connection failed!");
    setWiFiStatus(COLOR_ERROR);
    updateLEDs();
  }

  ArduinoOTA.setHostname("PowerHub");
  ArduinoOTA.setPassword("Koda1234!");
  ArduinoOTA.begin();
  Serial.println("OTA Ready");

  // Main page - use chunked transfer
  server.on("/", sendHTMLChunked);

  // Status page
  server.on("/status", [](){
    String html = "<!DOCTYPE html><html><head><title>Status</title>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "<style>body{background:#000;color:#0f0;font-family:'Courier New';padding:20px;}</style></head><body>";
    html += "<h1>ESP32 Power Hub Status</h1>";
    html += "<p><strong>WiFi:</strong> " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "</p>";
    html += "<p><strong>IP:</strong> " + WiFi.localIP().toString() + "</p>";
    html += "<p><strong>Uptime:</strong> " + String(millis()/1000) + "s</p>";
    html += "<p><strong>Free RAM:</strong> " + String(ESP.getFreeHeap()) + " bytes</p>";
    html += "<p><strong>PC1:</strong> " + String(pc1Online ? "ONLINE" : "OFFLINE") + "</p>";
    html += "<p><strong>PC2:</strong> " + String(pc2Online ? "ONLINE" : "OFFLINE") + "</p>";
    html += "<br><a href='/' style='color:#0f0;'>← Back</a></body></html>";
    server.send(200, "text/html", html);
  });

  // OTA Update page
  server.on("/update", HTTP_GET, [](){
    String html = "<!DOCTYPE html><html><head><title>OTA Update</title>";
    html += "<style>body{background:#000;color:#0f0;font-family:'Courier New';text-align:center;padding:50px;}";
    html += "input,button{background:#001100;color:#0f0;border:2px solid #0f0;padding:10px;margin:10px;}</style></head><body>";
    html += "<h1>⚡ MATRIX OTA UPDATER ⚡</h1>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update' accept='.bin' required><br>";
    html += "<button type='submit'>UPLOAD FIRMWARE</button></form>";
    html += "<p><a href='/' style='color:#0a0;'>← BACK</a></p></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/update", HTTP_POST, [](){
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAILED" : "SUCCESS - REBOOTING");
    delay(1000);
    ESP.restart();
  }, [](){
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.on("/pc1", [](){
    sendWOL(pc1MACStr);
    server.send(200, "text/plain", "Wake-on-LAN sent!");
  });

  server.on("/pc2/on", [](){
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

  server.on("/pc2/off", [](){
    Serial.println("PC2 shutdown - 6 second hold");
    setPC2Status(COLOR_ERROR);
    updateLEDs();
    digitalWrite(powerPin, HIGH);
    delay(6000);
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

void loop() {
  unsigned long currentTime = millis();
  
  // Watchdog - ensure loop is running
  if (currentTime - lastLoopTime > 5000) {
    Serial.println("Loop watchdog: still running");
    lastLoopTime = currentTime;
  }
  
  // Handle millis overflow
  if (currentTime < lastWiFiCheck) {
    lastWiFiCheck = 0;
    lastHeapLog = 0;
    lastStatusCheck = 0;
  }
  
  // WiFi monitoring
  if (currentTime - lastWiFiCheck >= wifiCheckInterval) {
    lastWiFiCheck = currentTime;

    // Only attempt reconnect if not already connected
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost! Reconnecting...");
      setWiFiStatus(COLOR_ERROR);
      updateLEDs();

      WiFi.disconnect(true); // Force disconnect
      delay(100);
      WiFi.begin(ssid, password);

      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Reconnected!");
        setWiFiStatus(COLOR_ACTIVE);
        updateLEDs();
        setBroadcastIP();
      } else {
        setWiFiStatus(COLOR_ERROR);
        updateLEDs();
      }
    }
  }
  
  // Heap monitoring
  if (currentTime - lastHeapLog >= heapLogInterval) {
    lastHeapLog = currentTime;
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  }
  
  ArduinoOTA.handle();
  server.handleClient();
  updatePCStatus();
  updateLEDs(); // Batch LED updates

  // Ensure WiFi status LED always reflects actual connection state
  static unsigned long lastWiFiLEDUpdate = 0;
  if (millis() - lastWiFiLEDUpdate > 2000) { // every 2 seconds
    lastWiFiLEDUpdate = millis();
    if (WiFi.status() == WL_CONNECTED) {
      setWiFiStatus(COLOR_ACTIVE);
    } else {
      setWiFiStatus(COLOR_ERROR);
    }
    updateLEDs();
  }

  yield(); // Give ESP32 time to handle WiFi/system tasks
  delay(10);
}