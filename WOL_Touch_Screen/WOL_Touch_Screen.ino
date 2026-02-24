#include <WiFi.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>

// -------- Wi-Fi Config --------
const char* ssid = "Rebellious Amish Family";
const char* password = "HenryOzzy423";

// -------- TFT Display --------
TFT_eSPI tft = TFT_eSPI();

// -------- Touch Calibration --------
// You may need to adjust these values for your specific display
#define TOUCH_THRESHOLD 600

// -------- PC (WoL) Config --------
const char* pcMACStr = "9c:6b:00:96:b2:aa"; // PC1 MAC address
IPAddress pcIP(192, 168, 1, 69); // PC IP for status monitoring
IPAddress broadcastIP; // Auto-detected network broadcast
const int WOLPort = 9; // Standard WoL port

WiFiUDP udp;

// -------- Button Settings --------
#define BUTTON_X 40
#define BUTTON_Y 80
#define BUTTON_W 240
#define BUTTON_H 80

// -------- Status Variables --------
unsigned long lastStatusCheck = 0;
const unsigned long statusCheckInterval = 5000; // Check every 5 seconds
bool pcOnline = false;
bool lastTouchState = false;

// -------- Colors --------
#define COLOR_BG       0x0000      // Black
#define COLOR_ONLINE   0x07E0      // Green
#define COLOR_OFFLINE  0xF800      // Red
#define COLOR_SENDING  0x001F      // Blue
#define COLOR_TEXT     0xFFFF      // White
#define COLOR_MATRIX   0x07E0      // Matrix Green
#define COLOR_BTN_IDLE 0x0320      // Dark Green
#define COLOR_BTN_PRESS 0x07E0     // Bright Green

// Convert MAC string to byte array
void parseMAC(const char* macStr, uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    mac[i] = strtoul(macStr + i*3, nullptr, 16);
  }
}

// Auto-detect broadcast IP
void setBroadcastIP() {
  IPAddress localIP = WiFi.localIP();
  IPAddress subnet = WiFi.subnetMask();
  
  // Calculate broadcast address
  uint32_t broadcast = (uint32_t(localIP) & uint32_t(subnet)) | (~uint32_t(subnet));
  broadcastIP = IPAddress(broadcast);
  
  Serial.print("Broadcast IP: "); Serial.println(broadcastIP);
}

// Ping PC to check if it's online
bool pingPC(IPAddress ip) {
  WiFiClient client;
  client.setTimeout(500);
  
  // Try HTTP port
  bool result = client.connect(ip, 80);
  if (result) {
    client.stop();
    return true;
  }
  
  // Try HTTPS as backup
  result = client.connect(ip, 443);
  if (result) {
    client.stop();
    return true;
  }
  
  return false;
}

// Send WoL magic packet
void sendWOL(const char* macStr) {
  Serial.println("Sending Wake-on-LAN packet...");
  
  uint8_t mac[6];
  parseMAC(macStr, mac);
  uint8_t packet[102];
  
  // 6 x 0xFF
  for (int i = 0; i < 6; i++) packet[i] = 0xFF;
  
  // 16 repetitions of MAC
  for (int i = 1; i <= 16; i++) {
    for (int j = 0; j < 6; j++) {
      packet[i*6 + j] = mac[j];
    }
  }
  
  udp.beginPacket(broadcastIP, WOLPort);
  udp.write(packet, sizeof(packet));
  udp.endPacket();
  
  Serial.println("Magic packet sent!");
  
  // Visual feedback
  drawButton(COLOR_SENDING);
  tft.setTextColor(COLOR_TEXT, COLOR_SENDING);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("SENDING...", BUTTON_X + BUTTON_W/2, BUTTON_Y + BUTTON_H/2, 4);
  
  delay(1000);
  
  // Force immediate status check
  lastStatusCheck = 0;
}

// Draw the wake button
void drawButton(uint16_t color) {
  tft.fillRoundRect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, 10, color);
  tft.drawRoundRect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, 10, COLOR_MATRIX);
  tft.drawRoundRect(BUTTON_X+1, BUTTON_Y+1, BUTTON_W-2, BUTTON_H-2, 9, COLOR_MATRIX);
}

// Update display with PC status
void updateDisplay() {
  // Status indicator at top
  tft.fillRect(0, 0, 320, 60, COLOR_BG);
  tft.setTextColor(pcOnline ? COLOR_ONLINE : COLOR_OFFLINE, COLOR_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(pcOnline ? "PC ONLINE" : "PC OFFLINE", 160, 10, 4);
  
  // Draw button
  drawButton(COLOR_BTN_IDLE);
  tft.setTextColor(COLOR_MATRIX, COLOR_BTN_IDLE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("WAKE PC", BUTTON_X + BUTTON_W/2, BUTTON_Y + BUTTON_H/2, 4);
  
  // IP info at bottom
  tft.fillRect(0, 180, 320, 60, COLOR_BG);
  tft.setTextColor(COLOR_MATRIX, COLOR_BG);
  tft.setTextDatum(BC_DATUM);
  tft.drawString(WiFi.localIP().toString(), 160, 230, 2);
}

// Check if button is touched
bool isButtonTouched() {
  uint16_t x, y;
  if (tft.getTouch(&x, &y)) {
    // Check if touch is within button bounds
    if (x >= BUTTON_X && x <= (BUTTON_X + BUTTON_W) &&
        y >= BUTTON_Y && y <= (BUTTON_Y + BUTTON_H)) {
      return true;
    }
  }
  return false;
}

// Update PC status
void updatePCStatus() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastStatusCheck >= statusCheckInterval) {
    lastStatusCheck = currentTime;
    
    bool newStatus = pingPC(pcIP);
    
    if (newStatus != pcOnline) {
      Serial.printf("PC status changed: %s -> %s\n", 
                   pcOnline ? "ONLINE" : "OFFLINE",
                   newStatus ? "ONLINE" : "OFFLINE");
      pcOnline = newStatus;
      updateDisplay();
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize display
  tft.init();
  tft.setRotation(1); // Landscape orientation
  tft.fillScreen(COLOR_BG);
  
  // Calibrate touch (you may need to run a calibration sketch first)
  uint16_t calData[5] = {275, 3620, 264, 3532, 1};
  tft.setTouch(calData);
  
  // Show connecting message
  tft.setTextColor(COLOR_MATRIX, COLOR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("CONNECTING TO WiFi...", 160, 120, 4);
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  setBroadcastIP();
  
  // Clear screen and draw initial UI
  tft.fillScreen(COLOR_BG);
  
  // Draw title
  tft.setTextColor(COLOR_MATRIX, COLOR_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("MATRIX POWER", 160, 10, 4);
  
  // Initial status check
  pcOnline = pingPC(pcIP);
  updateDisplay();
  
  Serial.println("WoL Touch Screen initialized!");
}

void loop() {
  // Check for touch input
  bool currentTouch = isButtonTouched();
  
  // Detect touch press (rising edge)
  if (currentTouch && !lastTouchState) {
    Serial.println("Button pressed!");
    
    // Button press visual feedback
    drawButton(COLOR_BTN_PRESS);
    tft.setTextColor(COLOR_TEXT, COLOR_BTN_PRESS);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("WAKE PC", BUTTON_X + BUTTON_W/2, BUTTON_Y + BUTTON_H/2, 4);
    
    delay(100); // Debounce
    
    // Send WoL packet
    sendWOL(pcMACStr);
    
    // Restore button
    updateDisplay();
  }
  
  lastTouchState = currentTouch;
  
  // Update PC status periodically
  updatePCStatus();
  
  delay(50); // Small delay for touch responsiveness
}
