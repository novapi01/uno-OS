#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>

// ─── Display ──────────────────────────────────────────────
// This shield uses a parallel (8-bit) interface driven by MCUFRIEND_kbv,
// NOT an SPI ST7789 panel — the pin numbers MCUFRIEND_kbv needs are wired
// internally by the library for the standard Uno shield pinout, so there's
// nothing to configure here beyond begin()/setRotation().
MCUFRIEND_kbv tft;

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// ─── Colors (RGB565) ──────────────────────────────────────
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_GRAY      0x8410
#define COLOR_DARKGREY  0x4208
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_RED       0xF800

// ─── Pins ─────────────────────────────────────────────────
#define SD_CS       10   // SD card chip select (confirmed working)
#define BUTTON_PIN  A5   // Wire A5 to GND to cycle modes (no touch confirmed yet —
                          // see handleButton() for where to swap in real touch later)

// ─── Timing ───────────────────────────────────────────────
#define PICTURE_DELAY   5000   // ms between images
#define BUTTON_DEBOUNCE 200    // ms

// ─── Software Clock (no RTC on this board) ────────────────
// Starting time — edit these to roughly match reality at power-on.
#define INITIAL_HOUR    10
#define INITIAL_MINUTE  45

// ─── App Modes ────────────────────────────────────────────
enum Mode {
  MODE_DASHBOARD,
  MODE_CLOCK,
  MODE_PICTURE_FRAME,
  MODE_EYES,
  MODE_SPOTIFY        // placeholder — future
};

Mode currentMode = MODE_DASHBOARD;
unsigned long lastTick = 0;

// Software clock state
int hours   = INITIAL_HOUR;
int minutes = INITIAL_MINUTE;
int seconds = 0;
int lastClockMinute = -1;

// Picture frame state
int picIndex        = 0;
int prevPicIndex    = -1;
int totalPictures   = 0;
unsigned long picLastChange = 0;

// Eyes animation state
int  eyeY, eyeX;
int  lookX = 0;
bool eyeBlink   = false;
bool eyeClosing = false;
unsigned long lookChangeTime = 0;
unsigned long blinkTime    = 0;
unsigned long blinkHold    = 0;
const int BLINK_DURATION = 150;
const int MIN_BLINK_GAP  = 1500;
const int MAX_BLINK_GAP  = 4500;
const int LOOK_DURATION  = 2000;

// ─── Helpers ──────────────────────────────────────────────

// Case-insensitive check for a ".bmp" extension
static bool isBmpFile(const char* name) {
  int len = strlen(name);
  if (len < 4) return false;
  int ext = len - 4;
  char c0 = name[ext], c1 = name[ext + 1], c2 = name[ext + 2], c3 = name[ext + 3];
  return ((c0 == 'b' || c0 == 'B') &&
          (c1 == 'm' || c1 == 'M') &&
          (c2 == 'p' || c2 == 'P') &&
          (c3 == '.'));
}

uint16_t read16(File &f) { uint16_t r; f.read((uint8_t*)&r, 2); return r; }
uint32_t read32(File &f) { uint32_t r; f.read((uint8_t*)&r, 4); return r; }

// ─── Arduino Setup ────────────────────────────────────────
void setup() {
  Serial.begin(9600);
  Serial.println("TFT Hat Dashboard starting...");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Deselect SD before touching the display bus, in case they ever share lines.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  uint16_t id = tft.readID();
  Serial.print("Display ID: 0x");
  Serial.println(id, HEX);
  if (id == 0xD3D3) id = 0x9481;   // common fallback for this shield family
  tft.begin(id);
  tft.setRotation(1);              // landscape, 320x240
  tft.fillScreen(COLOR_BLACK);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(30, 110);
  tft.println("Initializing...");

  Serial.print("Initializing SD card... ");
  if (!SD.begin(SD_CS)) {
    Serial.println("FAILED!");
    tft.fillScreen(COLOR_RED);
    tft.setTextSize(1);
    tft.setCursor(20, 100);
    tft.println("SD card init failed!");
    tft.setCursor(20, 130);
    tft.println("Check wiring & card");
    while (1) delay(1000);
  }
  Serial.println("OK");

  countPictures();

  setupEyes();
  drawDashboard();
}

// ─── Arduino Loop ─────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // Software clock tick
  if (now - lastTick >= 1000) {
    lastTick = now;
    seconds++;
    if (seconds >= 60) { seconds = 0; minutes++; }
    if (minutes >= 60) { minutes = 0; hours++; }
    if (hours >= 24)   { hours = 0; }
  }

  handleButton();

  switch (currentMode) {
    case MODE_DASHBOARD:
      break;  // static view, redrawn only on mode entry

    case MODE_CLOCK:
      drawClock();
      break;

    case MODE_PICTURE_FRAME:
      if (now - picLastChange >= PICTURE_DELAY && totalPictures > 0) {
        picIndex = (picIndex + 1) % totalPictures;
        picLastChange = now;
        prevPicIndex = -1;  // force redraw
      }
      drawPictureFrame();
      break;

    case MODE_EYES:
      drawEyes(now);
      break;

    case MODE_SPOTIFY:
      break;  // static view, redrawn only on mode entry
  }
}

// ─── Input Handling ───────────────────────────────────────
// NOTE: no touchscreen confirmed on this shield yet. For now a single
// button on BUTTON_PIN just cycles through every mode in order. If you
// later confirm resistive touch is wired (e.g. via the TouchScreen
// library on analog pins), replace this with tap-to-select-a-dashboard-
// -cell logic instead.
void handleButton() {
  static bool lastBtn = HIGH;
  static unsigned long lastPress = 0;

  bool btn = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (lastBtn == HIGH && btn == LOW && (now - lastPress) > BUTTON_DEBOUNCE) {
    lastPress = now;
    currentMode = (Mode)((currentMode + 1) % 5);

    if (currentMode == MODE_DASHBOARD) {
      drawDashboard();
    } else if (currentMode == MODE_EYES) {
      setupEyes();
    } else if (currentMode == MODE_CLOCK) {
      lastClockMinute = -1;  // force redraw
    } else if (currentMode == MODE_PICTURE_FRAME) {
      countPictures();
      picIndex = 0;
      prevPicIndex = -1;
      picLastChange = now;
    } else if (currentMode == MODE_SPOTIFY) {
      drawSpotify();
    }
  }
  lastBtn = btn;
}

// ─── Dashboard ────────────────────────────────────────────
void drawDashboard() {
  tft.fillScreen(COLOR_BLACK);

  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(110, 8);
  tft.println("DASHBOARD");

  tft.drawLine(10, 32, SCREEN_WIDTH - 10, 32, COLOR_GRAY);

  const int cellW = 120, cellH = 80;
  const int gapX  = 50,  gapY  = 20;
  const int ox    = 25,  oy    = 45;

  struct {
    Mode mode;
    uint16_t color;
    const char *label;
  } items[] = {
    { MODE_CLOCK,         COLOR_YELLOW, "CLOCK" },
    { MODE_PICTURE_FRAME, COLOR_CYAN,   "PICTURES" },
    { MODE_EYES,          COLOR_GREEN,  "EYES" },
    { MODE_SPOTIFY,       COLOR_GREEN,  "SPOTIFY" }
  };

  for (int i = 0; i < 4; i++) {
    int col = i % 2;
    int row = i / 2;
    int cx = ox + col * (cellW + gapX);
    int cy = oy + row * (cellH + gapY);

    tft.fillRoundRect(cx, cy, cellW, cellH, 8, COLOR_DARKGREY);
    tft.drawRoundRect(cx, cy, cellW, cellH, 8, items[i].color);

    int icx = cx + cellW / 2;
    int icy = cy + 30;
    switch (items[i].mode) {
      case MODE_CLOCK:         drawClockIcon(icx, icy, items[i].color); break;
      case MODE_PICTURE_FRAME: drawPictureIcon(icx, icy, items[i].color); break;
      case MODE_EYES:          drawEyesIcon(icx, icy, items[i].color); break;
      case MODE_SPOTIFY:       drawSpotifyIcon(icx, icy, items[i].color); break;
      default: break;
    }

    tft.setTextColor(COLOR_WHITE);
    tft.setTextSize(1);
    tft.setCursor(cx + 10, cy + 62);
    tft.println(items[i].label);
  }

  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(20, SCREEN_HEIGHT - 14);
  tft.println("Press button to cycle apps");
}

// ─── Dashboard Icons ──────────────────────────────────────
void drawClockIcon(int x, int y, uint16_t color) {
  tft.drawCircle(x, y, 16, color);
  tft.drawLine(x, y, x + 5, y - 8, color);
  tft.drawLine(x, y, x + 9, y + 3, color);
  tft.fillCircle(x, y, 2, color);
}

void drawPictureIcon(int x, int y, uint16_t color) {
  tft.drawRect(x - 16, y - 12, 32, 24, color);
  tft.fillCircle(x - 6, y - 4, 3, color);
  tft.drawLine(x - 14, y + 10, x - 4, y - 2, color);
  tft.drawLine(x - 4, y - 2, x + 5, y + 10, color);
  tft.drawLine(x + 2, y + 10, x + 10, y + 3, color);
}

void drawEyesIcon(int x, int y, uint16_t color) {
  tft.drawCircle(x - 10, y, 8, color);
  tft.fillCircle(x - 10, y, 2, color);
  tft.drawCircle(x + 10, y, 8, color);
  tft.fillCircle(x + 10, y, 2, color);
}

void drawSpotifyIcon(int x, int y, uint16_t color) {
  tft.drawCircle(x, y, 15, color);
  tft.drawCircle(x, y, 8, color);
  tft.drawLine(x - 5, y, x + 5, y, color);
}

// ─── Clock Mode ───────────────────────────────────────────
void drawClock() {
  if (minutes == lastClockMinute) {
    drawBlinkingDots(90, 70);
    return;
  }
  lastClockMinute = minutes;

  tft.fillScreen(COLOR_DARKGREY);

  tft.fillRect(0, 0, SCREEN_WIDTH, 32, COLOR_BLUE);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 10);
  tft.println("  DIGITAL CLOCK");

  tft.drawLine(20, 36, SCREEN_WIDTH, 36, COLOR_GRAY);

  drawDigitalTime(70, 75);
}

void drawBlinkingDots(int x, int y) {
  uint16_t c = (seconds % 2 == 0) ? COLOR_WHITE : COLOR_DARKGREY;
  tft.fillCircle(x + 130, y + 35, 5, c);
}

void drawDigitalTime(int x, int y) {
  bool pm = hours >= 12;
  int h12 = hours % 12;
  if (h12 == 0) h12 = 12;
  const char *ampm = pm ? "PM" : "AM";

  char buf[6];
  sprintf(buf, "%02d:%02d", h12, minutes);

  tft.setTextSize(5);
  tft.setTextColor(COLOR_BLACK);
  tft.setCursor(x + 2, y + 2);
  tft.println(buf);
  tft.setTextColor(COLOR_WHITE);
  tft.setCursor(x, y);
  tft.println(buf);

  tft.setTextSize(2);
  tft.setTextColor(COLOR_YELLOW);
  tft.setCursor(x + 150, y);
  tft.println(ampm);

  drawBlinkingDots(x, y);
}

// ─── Picture Frame Mode ───────────────────────────────────
void countPictures() {
  totalPictures = 0;
  File root = SD.open("/");
  if (!root) return;
  File entry = root.openNextFile();
  while (entry) {
    if (isBmpFile(entry.name())) totalPictures++;
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
}

void drawPictureFrame() {
  tft.fillRect(0, 0, SCREEN_WIDTH, 28, COLOR_BLUE);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 8);
  tft.print("PICTURE FRAME  (");
  tft.print(totalPictures);
  tft.println(" images)");
  tft.drawLine(0, 28, SCREEN_WIDTH, 28, COLOR_GRAY);

  if (picIndex != prevPicIndex) {
    prevPicIndex = picIndex;
    tft.fillRect(0, 28, SCREEN_WIDTH, SCREEN_HEIGHT - 28, COLOR_BLACK);
    if (totalPictures > 0) drawNthBmp(picIndex, 0, 28);
  }

  tft.fillRect(90, SCREEN_HEIGHT - 16, 140, 16, COLOR_BLACK);
  tft.setTextColor(COLOR_GRAY);
  char txt[16];
  snprintf(txt, sizeof(txt), "Photo %d/%d", picIndex + 1, totalPictures);
  tft.setCursor(90, SCREEN_HEIGHT - 16);
  tft.print(txt);
}

// Find the Nth .bmp on the SD card and draw it, cropped/centered into the
// area below the header bar. Uses a small streaming buffer (safe on the
// Uno's 2KB of RAM) rather than loading whole rows — so large images are
// cropped rather than scaled.
bool drawNthBmp(int index, int areaY, int areaTop) {
  File root = SD.open("/");
  if (!root) return false;

  int count = 0;
  File entry = root.openNextFile();
  char name[32] = {0};
  bool found = false;
  while (entry) {
    if (isBmpFile(entry.name())) {
      if (count == index) {
        strncpy(name, entry.name(), sizeof(name) - 1);
        found = true;
        entry.close();
        break;
      }
      count++;
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  if (!found) return false;

  File bmp = SD.open(name);
  if (!bmp) return false;

  if (bmp.read() != 'B' || bmp.read() != 'M') { bmp.close(); return false; }

  bmp.seek(10);
  uint32_t offset = read32(bmp);
  bmp.seek(18);
  int32_t width  = read32(bmp);
  int32_t height = read32(bmp);
  if (height < 0) height = -height;   // top-down BMP
  bmp.seek(28);
  uint16_t depth = read16(bmp);
  if (depth != 24) { bmp.close(); return false; }

  uint32_t rowSize = (width * 3 + 3) & ~3;

  int availH = SCREEN_HEIGHT - areaTop;
  int dispW = min((int)width, SCREEN_WIDTH);
  int dispH = min((int)height, availH);
  int offsetX = (SCREEN_WIDTH - dispW) / 2;
  int offsetY = areaTop + (availH - dispH) / 2;

  uint8_t buffer[3 * 20];  // 20-pixel chunks — small & safe on Uno RAM

  for (int row = 0; row < dispH; row++) {
    uint32_t pos = offset + (uint32_t)(height - 1 - row) * rowSize;
    bmp.seek(pos);

    int col = 0;
    while (col < dispW) {
      int chunk = min(dispW - col, 20);
      bmp.read(buffer, 3 * chunk);
      for (int i = 0; i < chunk; i++) {
        uint16_t color = tft.color565(buffer[i * 3 + 2], buffer[i * 3 + 1], buffer[i * 3]);
        tft.drawPixel(offsetX + col + i, offsetY + row, color);
      }
      col += chunk;
    }
  }

  bmp.close();
  return true;
}

// ─── Eyes Mode ────────────────────────────────────────────
void setupEyes() {
  eyeY = SCREEN_HEIGHT / 2 + 10;
  eyeX = SCREEN_WIDTH / 2;
  lookX = 0;
  nextLook();
  nextBlinkTime();
  nextLookTime();

  tft.fillScreen(COLOR_WHITE);
  tft.fillRect(0, 0, SCREEN_WIDTH, 28, COLOR_CYAN);
  tft.setTextColor(COLOR_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 8);
  tft.println("  YOUR COMPANION");
  tft.drawLine(0, 28, SCREEN_WIDTH, 28, COLOR_GRAY);
}

void nextLook()      { lookX = random(-80, 80); }
void nextBlinkTime() { blinkTime = millis() + random(MIN_BLINK_GAP, MAX_BLINK_GAP); }
void nextLookTime()  { lookChangeTime = millis() + random(1000, LOOK_DURATION); }

void drawEyes(unsigned long now) {
  if (!eyeBlink && !eyeClosing && now >= blinkTime) {
    eyeBlink = true;
    eyeClosing = true;
    blinkHold = now;
  }
  if (eyeBlink && eyeClosing && now >= blinkHold + BLINK_DURATION) {
    eyeClosing = false;
  }
  if (eyeBlink && !eyeClosing) {
    eyeBlink = false;
    nextBlinkTime();
  }
  if (now >= lookChangeTime) {
    nextLook();
    nextLookTime();
  }

  const int EYE_R = 34, IRIS_R = 18, PUPIL_R = 9;
  int irisLX = eyeX - 45 + (lookX * 40 / 80);
  int irisRX = eyeX + 45 + (lookX * 40 / 80);

  drawOneEye(eyeX - 45, eyeY, EYE_R, IRIS_R, PUPIL_R, irisLX, eyeY);
  drawOneEye(eyeX + 45, eyeY, EYE_R, IRIS_R, PUPIL_R, irisRX, eyeY);

  tft.drawRoundRect(eyeX - 18, eyeY + 60, 36, 18, 8, COLOR_DARKGREY);
  tft.fillRoundRect(eyeX - 16, eyeY + 62, 32, 14, 6, COLOR_WHITE);
}

void drawOneEye(int cx, int cy, int eyeR, int irisR, int pupilR, int ix, int iy) {
  tft.fillCircle(cx, cy, eyeR, COLOR_WHITE);
  tft.drawCircle(cx, cy, eyeR, COLOR_BLACK);

  if (eyeClosing) {
    tft.fillRoundRect(cx - eyeR - 2, cy - 3, (eyeR + 2) * 2, 6, 3, COLOR_BLACK);
    return;
  }

  tft.fillCircle(ix, iy, irisR, COLOR_DARKGREY);
  tft.drawCircle(ix, iy, irisR, COLOR_BLACK);
  tft.fillCircle(ix, iy, pupilR, COLOR_BLACK);
  tft.fillCircle(ix - 4, iy - 6, 3, COLOR_WHITE);
  tft.fillCircle(ix + 3, iy + 3, 2, COLOR_WHITE);
}

// ─── Spotify Mode (placeholder) ───────────────────────────
void drawSpotify() {
  tft.fillScreen(COLOR_BLACK);
  tft.fillRect(0, 0, SCREEN_WIDTH, 28, COLOR_GREEN);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 8);
  tft.println("  SPOTIFY -- TODO");
  tft.drawLine(0, 28, SCREEN_WIDTH, 28, COLOR_GRAY);

  tft.setTextColor(COLOR_GREEN);
  tft.setTextSize(3);
  tft.setCursor(85, 90);
  tft.println("SPOTIFY");

  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(1);
  tft.setCursor(100, 140);
  tft.println("Coming Soon!");
}
