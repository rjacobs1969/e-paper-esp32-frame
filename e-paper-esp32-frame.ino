#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include "epd7in3combined.h"
#include <Preferences.h>
#include <algorithm>
#include <vector>
#include "time_utils.h"
#include "esp_adc_cal.h"
#include "driver/adc.h"
#include <pngle.h>
#include "dither.h"

//This is the pin for the transistor that powers the external components
#define TRANSISTOR_PIN 26

// Compile-time debug mode: when set to 1 the sketch will NOT run
// the normal behaviour. Instead it will set `TRANSISTOR_PIN` as an
// output and toggle it every 5 seconds to help debug the circuit.
#ifndef DEBUG_TOGGLE_TRANSISTOR
#define DEBUG_TOGGLE_TRANSISTOR 0
#endif

Preferences preferences;

Epd epd;
unsigned long delta; // Variable to store the time it took to update the display for deep sleep calculations
unsigned long deltaSinceTimeObtain; // Variable to store the time it took to update the display since the time was obtained for deep sleep calculations

#define SD_CS_PIN 22

uint16_t width() { return EPD_WIDTH; }
uint16_t height() { return EPD_HEIGHT; }

SPIClass vspi(VSPI); // VSPI for SD card

DitherMode ditherMode = DITHER_NONE;


// Color palette for dithering — Spectra 6 (7IN3E) display
uint8_t colorPallete[6*3] = {
  0, 0, 0,        // BLACK
  255, 255, 255,   // WHITE
  255, 255, 0,     // YELLOW
  255, 0, 0,       // RED
  0, 0, 255,       // BLUE
  0, 255, 0        // GREEN
};

uint16_t read16(fs::File &f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read(); // MSB
  return result;
}

uint32_t read32(fs::File &f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read(); // LSB
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read(); // MSB
  return result;
}

float readBattery() {
  uint32_t value = 0;
  int rounds = 11;
  esp_adc_cal_characteristics_t adc_chars;

  //battery voltage divided by 2 can be measured at GPIO34, which equals ADC1_CHANNEL6
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
  switch(esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars)) {
    case ESP_ADC_CAL_VAL_EFUSE_TP:
      Serial.println("Characterized using Two Point Value");
      break;
    case ESP_ADC_CAL_VAL_EFUSE_VREF:
      Serial.printf("Characterized using eFuse Vref (%d mV)\r\n", adc_chars.vref);
      break;
    default:
      Serial.printf("Characterized using Default Vref (%d mV)\r\n", 1100);
  }

  //to avoid noise, sample the pin several times and average the result
  for(int i=1; i<=rounds; i++) {
    value += adc1_get_raw(ADC1_CHANNEL_6);
  }
  value /= (uint32_t)rounds;

  //due to the voltage divider (1M+1M) values must be multiplied by 2
  //and convert mV to V
  return esp_adc_cal_raw_to_voltage(value, &adc_chars)*2.0/1000.0;
}

void setup() {
  setCpuFrequencyMhz(240); // full speed for image decoding
  Serial.begin(115200);
  delta = millis();

#if DEBUG_TOGGLE_TRANSISTOR
  // Debug-only startup: initialise the transistor pin and stop.
  pinMode(TRANSISTOR_PIN, OUTPUT);
  digitalWrite(TRANSISTOR_PIN, LOW);
  Serial.println("DEBUG_TOGGLE_TRANSISTOR enabled: toggling pin every 5s");
  return;
#endif
  preferences.begin("e-paper", false);

  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();

  if(wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from deep sleep due to timer.");
  } else {
    Serial.println("Did not wake up from deep sleep.");
  }

  // Release hold on TRANSISTOR_PIN after deep sleep so we can control it again
  gpio_hold_dis((gpio_num_t)TRANSISTOR_PIN);
  // Turn on the transistor to power the external components
  pinMode(TRANSISTOR_PIN, OUTPUT);
  digitalWrite(TRANSISTOR_PIN, LOW);
  delay(100);

  // Initialize the SD card
  while(!SD.begin(SD_CS_PIN, vspi)){
    Serial.println("Card Mount Failed");
    hibernateShort();
  }

  // Initialize Wifi and get the time
  initializeWifi();
  initializeTime();

  deltaSinceTimeObtain = millis();

  // Initialize the e-paper display
  if (epd.Init() != 0) {
    Serial.println("eP init F");
    hibernate();
  }else{
    Serial.println("eP init no F");
  }

  String file = getNextFile(); // Get the next file to display

  delay(10); // Feed the watchdog — setup() may have been running close to the 5 s WDT limit
  drawImage(file); // Display the file

  digitalWrite(TRANSISTOR_PIN, HIGH); // Turn off external components

  preferences.end();
}

void loop() {
#if DEBUG_TOGGLE_TRANSISTOR
  static unsigned long lastToggle = 0;
  static bool state = false;
  unsigned long now = millis();
  if (now - lastToggle >= 5000) { // 5 seconds
    state = !state;
    digitalWrite(TRANSISTOR_PIN, state ? HIGH : LOW);
    Serial.println(state ? "TRANSISTOR_PIN HIGH" : "TRANSISTOR_PIN LOW");
    lastToggle = now;
  }
#else
  hibernate();
#endif
}

void hibernateShort() {
    Serial.println("start 60s nap");

    // Ensure TRANSISTOR_PIN stays HIGH during deep sleep
    gpio_hold_en((gpio_num_t)TRANSISTOR_PIN);
    gpio_deep_sleep_hold_en();

    //esp_deep_sleep(static_cast<uint64_t>(getSecondsTillNextImage(delta, deltaSinceTimeObtain))* 1e6);
    // sleep for 5 seconds debug
     esp_deep_sleep(60* 1e6); // RJDEBIG
}

void hibernate() {
    Serial.println("start sleep");

    // Ensure TRANSISTOR_PIN stays HIGH during deep sleep
    gpio_hold_en((gpio_num_t)TRANSISTOR_PIN);
    gpio_deep_sleep_hold_en();

    esp_deep_sleep(static_cast<uint64_t>(getSecondsTillNextImage(delta, deltaSinceTimeObtain))* 1e6);
    // sleep for 5 seconds debug
    // esp_deep_sleep(60* 1e6); // RJDEBIG
}

// Returns true if the filename ends with .bmp or .png (case-insensitive)
bool isImageFile(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".bmp") || lower.endsWith(".png");
}

// Function to get the next file to display.
// 1) Looks in /images_date/ for files whose name starts with DD_MM matching today's date.
// 2) If nothing matches, picks the next image sequentially from /images/.
// Supported extensions: .bmp .png
String getNextFile(){

  // ---- Build the date prefix (DD_MM) ----
  String datePrefix;

  if(timeWorking){
    int day   = timeinfo.tm_mday;
    int month = timeinfo.tm_mon + 1;

    // Before 9 AM use the previous day's date
    if (timeinfo.tm_hour < 9) {
      time_t previousDay = mktime(&timeinfo) - 24 * 60 * 60;
      struct tm* prev = localtime(&previousDay);
      day   = prev->tm_mday;
      month = prev->tm_mon + 1;
    }

    char buf[6];
    snprintf(buf, sizeof(buf), "%02d_%02d", day, month);
    datePrefix = String(buf);
  } else {
    // Fallback: use the stored date (DD.MM) converted to DD_MM
    String stored = preferences.getString("date", "01.01");
    datePrefix = stored.substring(0, 2) + "_" + stored.substring(3, 5);
  }

  Serial.println("Date prefix: " + datePrefix);

  // ---- 1) Search /images_date/ for a file starting with DD_MM ----
  File dateDir = SD.open("/images_date");
  if (dateDir && dateDir.isDirectory()) {
    while (true) {
      File entry = dateDir.openNextFile();
      if (!entry) break;

      String name = String(entry.name());
      entry.close();

      if (!isImageFile(name)) continue;

      // Check if the filename starts with the date prefix
      if (name.startsWith(datePrefix)) {
        dateDir.close();
        String path = "/images_date/" + name;
        Serial.println("Date-matched file: " + path);
        return path;
      }
    }
    dateDir.close();
  }

  Serial.println("No date-matched file found, falling back to /images/");

  // ---- 2) Fallback: cycle through /images/ sequentially ----
  File imgDir = SD.open("/images");
  if (!imgDir || !imgDir.isDirectory()) {
    Serial.println("/images/ folder not found");
    return "";
  }

  // Collect all valid image filenames and sort them
  std::vector<String> imageFiles;
  while (true) {
    File entry = imgDir.openNextFile();
    if (!entry) break;

    String name = String(entry.name());
    entry.close();

    if (isImageFile(name)) {
      imageFiles.push_back(name);
    }
  }
  imgDir.close();

  if (imageFiles.empty()) {
    Serial.println("No image files in /images/");
    return "";
  }

  std::sort(imageFiles.begin(), imageFiles.end());

  // Get the stored index and advance it
  unsigned int imageIndex = preferences.getUInt("imageIndex", 0);
  if (imageIndex >= imageFiles.size()) {
    imageIndex = 0;
  }

  String chosen = imageFiles[imageIndex];

  // Advance for next wake-up
  imageIndex++;
  if (imageIndex >= imageFiles.size()) {
    imageIndex = 0;
  }
  preferences.putUInt("imageIndex", imageIndex);

  String path = "/images/" + chosen;
  Serial.println("Sequential file: " + path);
  return path;
}

// ============================================================
//  Dithering helpers — controlled by the 3rd filename segment
//  Filename format: DD_MM_X_name.ext  where X is:
//    F = Floyd-Steinberg,  H = Halftone,  O = Ordered,
//    P = Pop-art,  N = None (nearest palette colour)
// ============================================================

// Parse the 3rd underscore-delimited segment of the bare filename.
DitherMode parseDitherMode(const String &filepath) {
  // Extract bare filename (after last '/')
  int lastSlash = filepath.lastIndexOf('/');
  String name = (lastSlash >= 0) ? filepath.substring(lastSlash + 1) : filepath;
  // Find 3rd segment: skip first two underscores
  int u1 = name.indexOf('_');
  if (u1 < 0) return DITHER_NONE;
  int u2 = name.indexOf('_', u1 + 1);
  if (u2 < 0) return DITHER_NONE;
  // Character right after 2nd underscore
  char c = (u2 + 1 < (int)name.length()) ? toupper(name.charAt(u2 + 1)) : 'N';
  switch (c) {
    case 'F': return DITHER_FLOYD;
    case 'H': return DITHER_HALFTONE;
    case 'O': return DITHER_ORDERED;
    case 'P': return DITHER_POPART;
    default:  return DITHER_NONE;
  }
}

// ---- 4×4 Bayer matrix for ordered dithering (values 0–15) ----
static const uint8_t bayer4[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 }
};

// ---- Halftone 4×4 threshold matrix (values 0–15) ----
static const uint8_t halftone4[4][4] = {
  { 13,  9,  5,  1 },
  {  6,  2, 14, 10 },
  {  8, 12,  0,  4 },
  {  3,  7, 11, 15 }
};

// Apply a threshold-matrix dither: perturb r,g,b then map to nearest palette.
int ditherThreshold(uint8_t r, uint8_t g, uint8_t b,
                    int col, int row, const uint8_t mat[4][4]) {
  // threshold in -128..+128 range derived from 0-15 matrix
  int t = ((int)mat[row & 3][col & 3] - 8) * 16; // range ≈ -128..+112
  uint8_t rr = constrain((int)r + t, 0, 255);
  uint8_t gg = constrain((int)g + t, 0, 255);
  uint8_t bb = constrain((int)b + t, 0, 255);
  return depalette(rr, gg, bb);
}

// Pop-art style: boost saturation + contrast then map to nearest palette.
int ditherPopArt(uint8_t r, uint8_t g, uint8_t b) {
  // Convert to simple saturation boost: push channels away from grey
  int avg = ((int)r + (int)g + (int)b) / 3;
  // Increase saturation by 80% and contrast by stretching
  int rr = constrain((int)(avg + ((int)r - avg) * 3), 0, 255);
  int gg = constrain((int)(avg + ((int)g - avg) * 3), 0, 255);
  int bb = constrain((int)(avg + ((int)b - avg) * 3), 0, 255);
  // Posterize to 2 levels per channel (0 or 255)
  rr = (rr > 127) ? 255 : 0;
  gg = (gg > 127) ? 255 : 0;
  bb = (bb > 127) ? 255 : 0;
  return depalette(rr, gg, bb);
}

// Function to draw a BMP image on the e-paper display
bool drawBmp(const char *filename) {
  Serial.println("Drawing bitmap file: " + String(filename));
  fs::File bmpFS;
  bmpFS =  SD.open(filename); // Open requested file on SD card
  uint32_t seekOffset, headerSize, paletteSize = 0;
  int16_t row;
  uint8_t r, g, b;
  uint16_t bitDepth;
  uint16_t magic = read16(bmpFS);

  if (magic != ('B' | ('M' << 8))) { // File not found or not a BMP
    Serial.println(F("BMP not found!"));
    bmpFS.close();
    return false;
  }

  read32(bmpFS); // filesize in bytes
  read32(bmpFS); // reserved
  seekOffset = read32(bmpFS); // start of bitmap
  headerSize = read32(bmpFS); // header size
  uint32_t w = read32(bmpFS); // width
  uint32_t h = read32(bmpFS); // height
  read16(bmpFS); // color planes (must be 1)
  bitDepth = read16(bmpFS);

  // Check if the BMP is valid
  if (read32(bmpFS) != 0 || (bitDepth != 24 && bitDepth != 1 && bitDepth != 4 && bitDepth != 8)) {
    Serial.println(F("BMP format not recognized."));
    bmpFS.close();
    return false;
  }

  uint32_t palette[256];
  if (bitDepth <= 8) // 1,4,8 bit bitmap: read color palette
  {
    read32(bmpFS); read32(bmpFS); read32(bmpFS); // size, w resolution, h resolution
    paletteSize = read32(bmpFS);
    if (paletteSize == 0) paletteSize = bitDepth * bitDepth; //if 0, size is 2^bitDepth
    bmpFS.seek(14 + headerSize); // start of color palette
    for (uint16_t i = 0; i < paletteSize; i++) {
      palette[i] = read32(bmpFS);
    }
  }

  // draw img that is shorter than display in the middle
  uint16_t x = (width() - w) /2;
  uint16_t y = (height() - h) /2;

  bmpFS.seek(seekOffset);

  uint32_t lineSize = ((bitDepth * w +31) >> 5) * 4;
  uint8_t lineBuffer[lineSize];
  uint8_t nextLineBuffer[lineSize];

  epd.SendCommand(0x10); // start data frame

  epd.EPD_7IN3F_Draw_Blank(y, width(), EPD_WHITE); // fill area on top of pic white

  // row is decremented as the BMP image is drawn bottom up
  bmpFS.read(lineBuffer, sizeof(lineBuffer));

  float batteryVolts = readBattery();
  Serial.println("Battery voltage: " + String(batteryVolts) + "V");

  for (row = h-1; row >= 0; row--) {
    epd.EPD_7IN3F_Draw_Blank(1, x, EPD_WHITE); // fill area on the left of pic white

    if(row != 0){
      bmpFS.read(nextLineBuffer, sizeof(nextLineBuffer));
    }
    uint8_t*  bptr = lineBuffer;
    uint8_t*  bnptr = nextLineBuffer;

    uint8_t output = 0;

    for (uint16_t col = 0; col < w; col++)
    {
      // Get r g b values for the next pixel (BMP stores as B,G,R)
      if (bitDepth == 24) {
        b = *bptr++;
        g = *bptr++;
        r = *bptr++;
        bnptr += 3;
      } else {
        uint32_t c = 0;
        if (bitDepth == 8) {
          c = palette[*bptr++];
        }
        else if (bitDepth == 4) {
          c = palette[(*bptr >> ((col & 0x01)?0:4)) & 0x0F];
          if (col & 0x01) bptr++;
        }
        else { // bitDepth == 1
          c = palette[(*bptr >> (7 - (col & 0x07))) & 0x01];
          if ((col & 0x07) == 0x07) bptr++;
        }
        b = c; g = c >> 8; r = c >> 16;
      }

      // Floyd-Steinberg dithering is used to dither the image
      uint8_t color;
      int indexColor;
      int errorR;
      int errorG;
      int errorB;

      // Apply the selected dithering mode
      if (ditherMode == DITHER_FLOYD) {
        indexColor = depalette(r, g, b);
        errorR = r - colorPallete[indexColor*3+0];
        errorG = g - colorPallete[indexColor*3+1];
        errorB = b - colorPallete[indexColor*3+2];

        if(col < w-1){
          bptr[0] = constrain(bptr[0] + (7*errorR/16), 0, 255);
          bptr[1] = constrain(bptr[1] + (7*errorG/16), 0, 255);
          bptr[2] = constrain(bptr[2] + (7*errorB/16), 0, 255);
        }

        if(row > 0){
          if(col > 0){
            bnptr[-4] = constrain(bnptr[-4] + (3*errorR/16), 0, 255);
            bnptr[-5] = constrain(bnptr[-5] + (3*errorG/16), 0, 255);
            bnptr[-6] = constrain(bnptr[-6] + (3*errorB/16), 0, 255);
          }
          bnptr[-1] = constrain(bnptr[-1] + (5*errorR/16), 0, 255);
          bnptr[-2] = constrain(bnptr[-2] + (5*errorG/16), 0, 255);
          bnptr[-3] = constrain(bnptr[-3] + (5*errorB/16), 0, 255);

          if(col < w-1){
            bnptr[0] = constrain(bnptr[0] + (1*errorR/16), 0, 255);
            bnptr[1] = constrain(bnptr[1] + (1*errorG/16), 0, 255);
            bnptr[2] = constrain(bnptr[2] + (1*errorB/16), 0, 255);
          }
        }
      } else if (ditherMode == DITHER_ORDERED) {
        indexColor = ditherThreshold(r, g, b, col, row, bayer4);
      } else if (ditherMode == DITHER_HALFTONE) {
        indexColor = ditherThreshold(r, g, b, col, row, halftone4);
      } else if (ditherMode == DITHER_POPART) {
        indexColor = ditherPopArt(r, g, b);
      } else {
        indexColor = depalette(r, g, b);
      }

      // Set the color based on the indexColor (Spectra 6)
      switch (indexColor){
        case 0: color = EPD_7IN3E_BLACK;  break;
        case 1: color = EPD_7IN3E_WHITE;  break;
        case 2: color = EPD_7IN3E_YELLOW; break;
        case 3: color = EPD_7IN3E_RED;    break;
        case 4: color = EPD_7IN3E_BLUE;   break;
        case 5: color = EPD_7IN3E_GREEN;  break;
      }

      if (batteryVolts <= 3.3 && col <= 50 && row >= h-50){
        color = EPD_RED;
        if (batteryVolts < 3.1) {
          Serial.println("Battery critically low, hibernating...");

          //switch off everything that might consume power
          esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
          esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF);
          //esp_sleep_pd_config(ESP_PD_DOMAIN_CPU, ESP_PD_OPTION_OFF);

          //disable all wakeup sources
          esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

          digitalWrite(2, LOW);
          esp_deep_sleep_start();

          Serial.println("This should never get printed");
          return false;
        }
      }

      // Vodoo magic i don't understand
      uint32_t buf_location = (row*(width()/2)+col/2);
      if (col & 0x01) {
        output |= color;
        epd.SendData(output);
      } else {
        output = color << 4;
      }
    }

    epd.EPD_7IN3F_Draw_Blank(1, x, EPD_WHITE); // fill area on the right of pic white
    memcpy(lineBuffer, nextLineBuffer, sizeof(lineBuffer));
  }

  epd.EPD_7IN3F_Draw_Blank(y, width(), EPD_WHITE); // fill area below the pic white

  bmpFS.close(); // Close the file
  epd.TurnOnDisplay(); // Turn on the display
  epd.Sleep(); // Put the display to sleep
  return true;
}

// Function to depalette the image
int depalette( uint8_t r, uint8_t g, uint8_t b ){
	int p;
	int mindiff = 100000000;
	int bestc = 0;

  // Find the color in the colorPallete that is closest to the r g b values
	for( p = 0; p < sizeof(colorPallete)/3; p++ )
	{
		int diffr = ((int)r) - ((int)colorPallete[p*3+0]);
		int diffg = ((int)g) - ((int)colorPallete[p*3+1]);
		int diffb = ((int)b) - ((int)colorPallete[p*3+2]);
		int diff = (diffr*diffr) + (diffg*diffg) + (diffb * diffb);
		if( diff < mindiff )
		{
			mindiff = diff;
			bestc = p;
		}
	}
	return bestc;
}

// ============================================================
//  Palette-index → display color (shared by all decoders)
// ============================================================
uint8_t indexToColor(int idx) {
  switch (idx) {
    case 0: return EPD_7IN3E_BLACK;
    case 1: return EPD_7IN3E_WHITE;
    case 2: return EPD_7IN3E_YELLOW;
    case 3: return EPD_7IN3E_RED;
    case 4: return EPD_7IN3E_BLUE;
    case 5: return EPD_7IN3E_GREEN;
    default: return EPD_7IN3E_WHITE;
  }
}

// ============================================================
//  PNG decoder  (uses pngle library — lightweight, line-based)
// ============================================================

static int pngOffsetX = 0;
static int pngOffsetY = 0;
static int pngImgW = 0, pngImgH = 0;
static int pngCurRow = -1;
static uint8_t pngLineColor[EPD_WIDTH];
static float pngBatteryVolts = 0;
static int pngLastRowFlushed = -1;

void pngFlushRow(int dispRow) {
  uint8_t output = 0;
  for (int col = 0; col < (int)EPD_WIDTH; col++) {
    uint8_t color = indexToColor(pngLineColor[col]);
    if (pngBatteryVolts <= 3.3 && col <= 50 && dispRow >= (int)EPD_HEIGHT - 50) {
      color = EPD_RED;
    }
    if (col & 0x01) {
      output |= color;
      epd.SendData(output);
    } else {
      output = color << 4;
    }
  }
}

void pngDrawCallback(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      const uint8_t rgba[4]) {
  int dispRow = (int)y + pngOffsetY;
  int dispCol = (int)x + pngOffsetX;

  // New row? Flush the previous one
  if ((int)y != pngCurRow) {
    if (pngCurRow >= 0) {
      int prevDispRow = pngCurRow + pngOffsetY;
      // Fill any skipped blank rows
      while (pngLastRowFlushed < prevDispRow - 1) {
        pngLastRowFlushed++;
        memset(pngLineColor, 1, sizeof(pngLineColor));
        pngFlushRow(pngLastRowFlushed);
      }
      pngFlushRow(prevDispRow);
      pngLastRowFlushed = prevDispRow;
    }
    pngCurRow = (int)y;
    memset(pngLineColor, 1, sizeof(pngLineColor)); // reset to WHITE
  }

  if (dispCol >= 0 && dispCol < (int)EPD_WIDTH && dispRow >= 0 && dispRow < (int)EPD_HEIGHT) {
    int idx;
    if (ditherMode == DITHER_ORDERED)
      idx = ditherThreshold(rgba[0], rgba[1], rgba[2], dispCol, dispRow, bayer4);
    else if (ditherMode == DITHER_HALFTONE)
      idx = ditherThreshold(rgba[0], rgba[1], rgba[2], dispCol, dispRow, halftone4);
    else if (ditherMode == DITHER_POPART)
      idx = ditherPopArt(rgba[0], rgba[1], rgba[2]);
    else
      idx = depalette(rgba[0], rgba[1], rgba[2]);
    pngLineColor[dispCol] = idx;
  }
}

bool drawPng(const char *filename) {
  Serial.println("Drawing PNG file: " + String(filename));

  fs::File pngFile = SD.open(filename);
  if (!pngFile) {
    Serial.println("PNG file not found");
    return false;
  }

  pngle_t *pngle = pngle_new();
  if (!pngle) {
    Serial.println("pngle_new failed");
    pngFile.close();
    return false;
  }

  pngle_set_draw_callback(pngle, pngDrawCallback);

  // We need to feed some data first to get dimensions
  // Feed the whole file in chunks
  pngCurRow = -1;
  pngLastRowFlushed = -1;
  pngBatteryVolts = readBattery();
  Serial.println("Battery voltage: " + String(pngBatteryVolts) + "V");

  // Read header to get dimensions — feed first chunk
  uint8_t pngBuf[1024];
  int fed = pngFile.read(pngBuf, sizeof(pngBuf));
  if (fed > 0) {
    int consumed = pngle_feed(pngle, pngBuf, fed);
    if (consumed < 0) {
      Serial.println("PNG feed error");
      pngle_destroy(pngle);
      pngFile.close();
      return false;
    }
  }

  pngImgW = pngle_get_width(pngle);
  pngImgH = pngle_get_height(pngle);
  pngOffsetX = ((int)EPD_WIDTH  - pngImgW) / 2;
  pngOffsetY = ((int)EPD_HEIGHT - pngImgH) / 2;

  memset(pngLineColor, 1, sizeof(pngLineColor));

  // PNG delivers pixels top-down, but PSR 0x57 (UD bit flipped for
  // upside-down mount) expects bottom-up data like BMP.  Override
  // the Panel Setting Register to 0x5F (UD=1, normal scan) so the
  // top-down PNG stream maps correctly to the physical display.
  epd.SendCommand(0x00);   // PSR
  epd.SendData(0x5F);
  epd.SendData(0x69);

  epd.SendCommand(0x10); // start data frame

  // Fill blank rows above image
  for (int r = 0; r < pngOffsetY; r++) {
    memset(pngLineColor, 1, sizeof(pngLineColor));
    pngFlushRow(r);
    pngLastRowFlushed = r;
  }

  // Continue feeding the rest of the file
  while (pngFile.available()) {
    int bytesRead = pngFile.read(pngBuf, sizeof(pngBuf));
    if (bytesRead <= 0) break;
    int consumed = pngle_feed(pngle, pngBuf, bytesRead);
    if (consumed < 0) {
      Serial.println("PNG decode error");
      break;
    }
  }

  // Flush the last image row
  if (pngCurRow >= 0) {
    int lastDispRow = pngCurRow + pngOffsetY;
    while (pngLastRowFlushed < lastDispRow - 1) {
      pngLastRowFlushed++;
      memset(pngLineColor, 1, sizeof(pngLineColor));
      pngFlushRow(pngLastRowFlushed);
    }
    pngFlushRow(lastDispRow);
    pngLastRowFlushed = lastDispRow;
  }

  // Fill remaining rows below image
  while (pngLastRowFlushed < (int)EPD_HEIGHT - 1) {
    pngLastRowFlushed++;
    memset(pngLineColor, 1, sizeof(pngLineColor));
    pngFlushRow(pngLastRowFlushed);
  }

  pngle_destroy(pngle);
  pngFile.close();
  epd.TurnOnDisplay();
  epd.Sleep();
  return true;
}

// ============================================================
//  Image dispatcher — picks decoder & dithering by extension
// ============================================================
void drawImage(const String &filepath) {
  // Determine dither mode from the 3rd filename segment (DD_MM_X_…)
  ditherMode = parseDitherMode(filepath);
  Serial.println("Dither mode: " + String((int)ditherMode));

  String lower = filepath;
  lower.toLowerCase();

  if (lower.endsWith(".png")) {
    drawPng(filepath.c_str());
  } else {
    drawBmp(filepath.c_str());
  }
}