# WoL Touch Screen Device

## Hardware Requirements
- ESP32 (any variant with enough pins)
- ILI9341 2.8" TFT Touch Screen Display (240x320)
- USB cable for programming

## Pin Configuration (Default TFT_eSPI)
You'll need to configure the TFT_eSPI library for your specific display.

### Typical ILI9341 Pin Connections:
- **TFT_MISO**: GPIO 19
- **TFT_MOSI**: GPIO 23
- **TFT_SCLK**: GPIO 18
- **TFT_CS**: GPIO 15
- **TFT_DC**: GPIO 2
- **TFT_RST**: GPIO 4
- **TOUCH_CS**: GPIO 21

**Note:** You must configure these pins in the TFT_eSPI library User_Setup.h file!

## Required Libraries
Install these libraries via Arduino IDE Library Manager or Arduino CLI:

```bash
arduino-cli lib install "TFT_eSPI"
```

## TFT_eSPI Configuration
Before compiling, you MUST edit the TFT_eSPI library configuration:

1. Find the TFT_eSPI library folder:
   - Windows: `C:\Users\<YourUsername>\Documents\Arduino\libraries\TFT_eSPI\`
   - Or in Arduino15 packages folder

2. Edit `User_Setup.h` or create `User_Setup_Select.h`:
   - Uncomment `#define ILI9341_DRIVER`
   - Set your pin definitions
   - Enable touch support: `#define TOUCH_CS 21`

### Example User_Setup.h configuration:
```cpp
#define ILI9341_DRIVER

#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4

#define TOUCH_CS 21

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define SMOOTH_FONT

#define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
```

## Touch Calibration
The touch screen needs calibration. Run this calibration sketch first:

1. Load the TFT_eSPI example: `File -> Examples -> TFT_eSPI -> Generic -> Touch_calibrate`
2. Upload and follow the on-screen instructions
3. Copy the calibration data and update the `calData` array in the sketch

## Features
- **Matrix-themed Interface**: Green-on-black hacker aesthetic
- **Touch Button**: Large, easy-to-press wake button
- **PC Status Display**: Shows if PC is online/offline
- **Visual Feedback**: Button responds to touch with color changes
- **Network Info**: Displays ESP32 IP address

## Network Configuration
Update these values in the code:
```cpp
const char* ssid = "Rebellious Amish Family";
const char* password = "HenryOzzy423";
const char* pcMACStr = "9c:6b:00:96:b2:aa";  // Your PC's MAC address
IPAddress pcIP(192, 168, 1, 69);              // Your PC's IP address
```

## Compilation
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 WOL_Touch_Screen
```

## Upload
```bash
arduino-cli upload --fqbn esp32:esp32:esp32 --port COM3 WOL_Touch_Screen
```

## Usage
1. Power on the device
2. Wait for WiFi connection
3. Screen will show "PC ONLINE" or "PC OFFLINE" at the top
4. Touch the "WAKE PC" button to send Wake-on-LAN packet
5. Button will show "SENDING..." feedback
6. PC status will update automatically every 5 seconds

## Troubleshooting

### Display not working:
- Check TFT_eSPI User_Setup.h configuration
- Verify pin connections
- Try different SPI frequencies

### Touch not responding:
- Run touch calibration sketch
- Check TOUCH_CS pin configuration
- Verify touch controller is XPT2046 compatible

### WoL not working:
- Verify PC MAC address is correct
- Check broadcast IP is correct for your network
- Ensure PC has Wake-on-LAN enabled in BIOS
- Check network allows UDP broadcasts

### PC status always shows offline:
- Verify PC IP address is correct
- Check firewall settings on PC
- Ensure PC has HTTP/HTTPS service running
