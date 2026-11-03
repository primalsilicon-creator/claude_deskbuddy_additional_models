#include "hal.h"
#if defined(BOARD_ESP32_32E)
// LCDwiki ESP32-32E displays. See hal.h for the pin maps. TFT_eSPI drives
// the panel (pins from the build flags in platformio.ini). The XPT2046
// touch chip is read here: on the 3.2" it shares the LCD's SPI bus and is
// read through TFT_eSPI, on the 2.8" it has its own pins (TOUCH_SPI_SEPARATE).
#include <Preferences.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include <time.h>

// ───────────────────────── pins ─────────────────────────
#ifndef LCD_BL_PIN
#define LCD_BL_PIN 27                 // 3.2"; the 2.8" uses 21
#endif
static const int PIN_LCD_BL   = LCD_BL_PIN;   // active high
static const int PIN_TOUCH_IRQ = 36;  // XPT2046 PENIRQ, low while touched
static const int PIN_BTN_BOOT = 0;
static const int PIN_BAT_ADC  = 34;
static const int PIN_LED_R    = 22;   // RGB LED, common anode: low = on
static const int PIN_LED_G    = 16;
static const int PIN_LED_B    = 17;
static const int PIN_SPK      = 26;   // DAC pin into the FM8002E amp
static const int PIN_AMP_EN   = 4;    // low = amp on

// LEDC channels (Arduino core 2.x API)
static const int BL_CH  = 1;
static const int SPK_CH = 2;

HalButton BtnA;   // BOOT
HalButton BtnB;   // no button: never pressed

// ───────────────────────── display ─────────────────────────
static bool _flip = false;

void halPush(TFT_eSprite& spr)    { spr.pushSprite(0, PET_PANE_H); }
void halPushPet(TFT_eSprite& pet) { pet.pushSprite(0, 0); }

void halSetFlip(bool flipped) {
  _flip = flipped;
  tft.setRotation(flipped ? 2 : 0);
}

// ───────────────────────── backlight ─────────────────────────
static uint8_t _blPercent = 100;
static bool _screenOn = true;

static void blApply() {
  uint32_t duty = _screenOn ? (uint32_t)_blPercent * 255 / 100 : 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LCD_BL, duty);
#else
  ledcWrite(BL_CH, duty);
#endif
}
static void blBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LCD_BL, 5000, 8);
#else
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(PIN_LCD_BL, BL_CH);
#endif
  _screenOn = false;
  blApply();
}
void halBacklight(uint8_t percent) {
  _blPercent = percent > 100 ? 100 : percent;
  blApply();
}
void halScreenOn(bool on) {
  _screenOn = on;
  blApply();
}

// ───────────────────────── buttons ─────────────────────────
void HalButton::begin(uint8_t pin) {
  _pin = pin;
  if (pin == 0xFF) return;
  pinMode(pin, INPUT_PULLUP);
  _raw = _pressed = digitalRead(pin) == LOW;
  _since = _rawSince = millis();
}
void HalButton::update() {
  _wasPressed = _wasReleased = false;
  if (_pin == 0xFF) return;
  bool raw = digitalRead(_pin) == LOW;
  uint32_t now = millis();
  if (raw != _raw) { _raw = raw; _rawSince = now; }
  if (now - _rawSince < 20 || raw == _pressed) return;   // 20ms debounce
  _pressed = raw;
  _since = now;
  if (_locked) { if (!raw) _locked = false; _pressed = false; return; }
  if (raw) _wasPressed = true; else _wasReleased = true;
}

// ───────────────────────── touch ─────────────────────────
// Resistive, so it needs calibrating. Three crosshairs give the raw reading
// at known points, which also shows whether the panel's axes are swapped or
// mirrored. The result is saved to NVS. Calibration always runs at rotation
// 0; when the picture is flipped, the point is mirrored here instead.
static Preferences _touchPrefs;
static bool _touchDown = false, _tapPending = false, _longPending = false, _longFired = false;
static int  _downX = 0, _downY = 0, _lastX = 0, _lastY = 0;
static int  _tapX = 0, _tapY = 0;
static uint32_t _downSince = 0, _lastTouchPoll = 0, _lastSeenDown = 0;
static const uint32_t LONG_PRESS_MS = 600;
static const int TAP_SLOP_PX = 24;    // more movement than this isn't a tap
static const uint16_t Z_PRESS = 300;  // pressure needed to count as a touch

#if defined(TOUCH_SPI_SEPARATE)
static SPIClass _tspi(VSPI);
static uint16_t xptRead(uint8_t cmd) {
  _tspi.transfer(cmd);
  return _tspi.transfer16(0) >> 3;
}
#endif

// Raw 12-bit readings and pressure. a and b are the chip's two axes, which
// may be either way round relative to the screen.
static void touchRaw(uint16_t* a, uint16_t* b, uint16_t* z) {
#if defined(TOUCH_SPI_SEPARATE)
  _tspi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_SPI_CS, LOW);
  int z1 = xptRead(0xB1), z2 = xptRead(0xC1);
  int zz = z1 + 4095 - z2;
  *z = (z1 == 0) ? 0 : (zz < 0 ? 0 : zz);
  xptRead(0xD1);                                   // first sample is noisy
  *a = (xptRead(0xD1) + xptRead(0xD1)) / 2;
  *b = (xptRead(0x91) + xptRead(0x91)) / 2;
  xptRead(0xD0);                                   // power down, PENIRQ on
  digitalWrite(TOUCH_SPI_CS, HIGH);
  _tspi.endTransaction();
#else
  *z = tft.getTouchRawZ();
  tft.getTouchRaw(a, b);
#endif
}

// Calibration: raw value of the screen-x axis at x=CAL_M and x=W-CAL_M,
// of the screen-y axis at y=CAL_M and y=H-CAL_M, and whether a/b swap.
struct TouchCal { int16_t x0, x1, y0, y1; uint8_t swap; };
static TouchCal _cal = { 300, 3700, 300, 3700, 0 };   // rough until calibrated
static const int CAL_M = 24;

static bool touchRead(int* sx, int* sy) {
  uint16_t a, b, z;
  touchRaw(&a, &b, &z);
  if (z < Z_PRESS) return false;
  int rx = _cal.swap ? b : a, ry = _cal.swap ? a : b;
  if (_cal.x1 == _cal.x0 || _cal.y1 == _cal.y0) return false;
  int x = CAL_M + (rx - _cal.x0) * (SCREEN_W - 2 * CAL_M) / (_cal.x1 - _cal.x0);
  int y = CAL_M + (ry - _cal.y0) * (SCREEN_H - 2 * CAL_M) / (_cal.y1 - _cal.y0);
  *sx = constrain(x, 0, SCREEN_W - 1);
  *sy = constrain(y, 0, SCREEN_H - 1);
  return true;
}

// Show a crosshair and wait for a press and release there. Returns the
// averaged raw reading, or false after timeoutMs with no press. Resistive
// reads drop out for a sample now and then mid-press, so a press only ends
// after 100ms with no contact, and the next point waits for a clean release.
static bool calPoint(int x, int y, uint32_t timeoutMs, uint16_t* ra, uint16_t* rb) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString("Touch calibration", SCREEN_W / 2, SCREEN_H / 2 - 10);
  tft.drawString("Press the + firmly", SCREEN_W / 2, SCREEN_H / 2 + 10);
  tft.setTextDatum(TL_DATUM);
  tft.drawFastHLine(x - 12, y, 25, TFT_MAGENTA);
  tft.drawFastVLine(x, y - 12, 25, TFT_MAGENTA);
  tft.drawCircle(x, y, 6, TFT_MAGENTA);

  uint32_t t0 = millis(), lastLog = 0, upSince = millis(), sumA = 0, sumB = 0, n = 0;
  bool armed = false;   // seen 200ms of no contact since the crosshair appeared
  while (millis() - t0 < timeoutMs) {
    uint16_t a, b, z;
    touchRaw(&a, &b, &z);
    uint32_t now = millis();
    bool down = z >= Z_PRESS;
    if (now - lastLog > 1000 || (down && n == 0 && armed)) {
      lastLog = now;
      Serial.printf("touch: raw a %u b %u z %u, irq %d\n", a, b, z, digitalRead(PIN_TOUCH_IRQ));
    }
    if (down) {
      upSince = now;
      if (armed) { sumA += a; sumB += b; n++; }
    } else if (!armed) {
      if (now - upSince >= 200) armed = true;
    } else if (n >= 5 && now - upSince >= 100) {
      *ra = sumA / n; *rb = sumB / n;
      Serial.printf("touch: point %d,%d -> raw %u,%u (%u samples)\n", x, y, *ra, *rb, n);
      return true;
    }
    delay(10);
  }
  return false;
}

void halTouchCalibrate() {
  bool wasFlipped = _flip;
  tft.setRotation(0);
  halScreenOn(true);
  Serial.println("touch: calibrating (20s per point before it gives up)");
  uint16_t a1, b1, a2, b2, a3, b3;
  // A point that reads almost the same as the one before was most likely
  // the same press; show it again.
  auto same = [](uint16_t pa, uint16_t pb, uint16_t qa, uint16_t qb) {
    return abs((int)pa - (int)qa) < 150 && abs((int)pb - (int)qb) < 150;
  };
  bool ok = calPoint(CAL_M, CAL_M, 20000, &a1, &b1);
  for (int tries = 0; ok && tries < 3; tries++) {
    ok = calPoint(SCREEN_W - CAL_M, CAL_M, 20000, &a2, &b2);
    if (!ok || !same(a1, b1, a2, b2)) break;
    Serial.println("touch: point 2 matches point 1, asking again");
  }
  for (int tries = 0; ok && tries < 3; tries++) {
    ok = calPoint(CAL_M, SCREEN_H - CAL_M, 20000, &a3, &b3);
    if (!ok || !same(a2, b2, a3, b3)) break;
    Serial.println("touch: point 3 matches point 2, asking again");
  }
  if (ok) {
    // Point 1 -> 2 moves along screen x only: whichever raw axis changed
    // more is screen x.
    TouchCal c;
    c.swap = abs((int)b2 - (int)b1) > abs((int)a2 - (int)a1);
    c.x0 = c.swap ? b1 : a1;  c.x1 = c.swap ? b2 : a2;
    c.y0 = c.swap ? a1 : b1;  c.y1 = c.swap ? a3 : b3;
    if (abs(c.x1 - c.x0) < 200 || abs(c.y1 - c.y0) < 200) {
      Serial.println("touch: calibration points too close together, ignored");
    } else {
      _cal = c;
      _touchPrefs.begin("touch", false);
      _touchPrefs.putBytes("cal2", &_cal, sizeof(_cal));
      _touchPrefs.end();
      Serial.printf("touch: calibrated x %d..%d y %d..%d swap %u\n", c.x0, c.x1, c.y0, c.y1, c.swap);
    }
  } else {
    // Nothing pressed: the chip may not be answering, or nobody is there.
    // Carry on with rough defaults and ask again next boot.
    Serial.println("touch: no press, using default calibration (not saved)");
  }
  tft.fillScreen(TFT_BLACK);
  halSetFlip(wasFlipped);
  // The last press may still be down; don't count it as a tap.
  _touchDown = true; _longFired = true;
}

static void touchBegin() {
  pinMode(PIN_TOUCH_IRQ, INPUT);
#if defined(TOUCH_SPI_SEPARATE)
  pinMode(TOUCH_SPI_CS, OUTPUT);
  digitalWrite(TOUCH_SPI_CS, HIGH);
  _tspi.begin(TOUCH_SPI_SCK, TOUCH_SPI_MISO, TOUCH_SPI_MOSI, -1);
#endif
  _touchPrefs.begin("touch", true);
  size_t n = _touchPrefs.getBytes("cal2", &_cal, sizeof(_cal));
  _touchPrefs.end();
  if (n == sizeof(_cal)) {
    Serial.printf("touch: calibration x %d..%d y %d..%d swap %u\n", _cal.x0, _cal.x1, _cal.y0, _cal.y1, _cal.swap);
  } else {
    halTouchCalibrate();   // first boot
  }
}

static void touchPoll() {
  uint32_t now = millis();
  if (now - _lastTouchPoll < 20) return;
  _lastTouchPoll = now;

  int sx = 0, sy = 0;
  bool down = touchRead(&sx, &sy);
  if (down) {
    if (_flip) { sx = SCREEN_W - 1 - sx; sy = SCREEN_H - 1 - sy; }
    _lastX = sx; _lastY = sy;
    _lastSeenDown = now;
    if (!_touchDown) {
      _touchDown = true;
      _longFired = false;
      _downX = sx; _downY = sy; _downSince = now;
    } else if (!_longFired && now - _downSince >= LONG_PRESS_MS &&
               abs(sx - _downX) < TAP_SLOP_PX && abs(sy - _downY) < TAP_SLOP_PX) {
      _longFired = true;
      _longPending = true;
      Serial.printf("touch: long press at %d,%d\n", _downX, _downY);
    }
    return;
  }
  // Resistive reads drop out for a sample now and then mid-press, so only
  // call it a release after 60ms without contact.
  if (_touchDown && now - _lastSeenDown > 60) {
    _touchDown = false;
    if (!_longFired && abs(_lastX - _downX) < TAP_SLOP_PX && abs(_lastY - _downY) < TAP_SLOP_PX) {
      _tapX = _downX; _tapY = _downY;
      _tapPending = true;
      Serial.printf("touch: tap %d,%d\n", _tapX, _tapY);
    }
  }
}

bool halTouchTapped(int* x, int* y) {
  if (!_tapPending) return false;
  _tapPending = false;
  if (x) *x = _tapX;
  if (y) *y = _tapY;
  return true;
}
bool halTouching() { return _touchDown; }
bool halTouchLongPressed() {
  if (!_longPending) return false;
  _longPending = false;
  return true;
}

// ───────────────────────── no IMU ─────────────────────────
bool halImuOk() { return false; }
void halAccel(float* ax, float* ay, float* az) { *ax = 0; *ay = 0; *az = 1.0f; }

// ───────────────────────── clock ─────────────────────────
// No RTC chip: keep time in the ESP32's system clock. The desktop sends
// local time, so it's stored as if it were UTC and read back with gmtime.
void halRtcGet(RTC_TimeTypeDef* t, RTC_DateTypeDef* d) {
  time_t now = time(nullptr);
  struct tm lt; gmtime_r(&now, &lt);
  t->Hours = lt.tm_hour; t->Minutes = lt.tm_min; t->Seconds = lt.tm_sec;
  d->WeekDay = lt.tm_wday; d->Month = lt.tm_mon + 1; d->Date = lt.tm_mday;
  d->Year = lt.tm_year + 1900;
}
void halRtcSet(const RTC_TimeTypeDef* t, const RTC_DateTypeDef* d) {
  // Days from civil date (Howard Hinnant's algorithm), so this doesn't
  // depend on the TZ setting the way mktime() does.
  int y = d->Year, m = d->Month, dd = d->Date;
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
  struct timeval tv = {};
  tv.tv_sec = (time_t)(days * 86400 + t->Hours * 3600 + t->Minutes * 60 + t->Seconds);
  settimeofday(&tv, nullptr);
}

// ───────────────────────── battery / power ─────────────────────────
// The wiki doesn't give the divider on IO34; a 1/2 divider is assumed. If
// the battery reading on the DEVICE info page is off by a constant factor,
// this is the number to change.
static const int BAT_DIVIDER = 2;
static int _batMv = 0;
static uint32_t _lastBatRead = 0;
int halBatteryMv() {
  if (_batMv == 0 || millis() - _lastBatRead > 500) {
    _lastBatRead = millis();
    int mv = analogReadMilliVolts(PIN_BAT_ADC) * BAT_DIVIDER;
    _batMv = _batMv == 0 ? mv : (_batMv * 3 + mv) / 4;
  }
  return _batMv;
}
// No VBUS sense. A battery on the charger, or no battery at all (the
// charger output idles near 4.2V), reads high, so treat that as USB.
bool halOnUsb() { return halBatteryMv() > 4150; }

void halPowerOff() {
  // No power switch or latch: deep sleep until the screen is touched
  // (PENIRQ goes low) or BOOT is pressed. Waking reboots the firmware.
  halScreenOn(false);
  halLed(false);
  digitalWrite(PIN_AMP_EN, HIGH);
  tft.writecommand(0x10);   // ST7789 sleep in
  delay(120);
  // Wait for the finger to come off, or PENIRQ wakes us straight away.
  uint32_t t0 = millis();
  while (digitalRead(PIN_TOUCH_IRQ) == LOW && millis() - t0 < 3000) delay(10);
  delay(200);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_IRQ, 0);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_BOOT, ESP_EXT1_WAKEUP_ALL_LOW);
  esp_deep_sleep_start();
}

// ───────────────────────── LED ─────────────────────────
void halLed(bool on) { digitalWrite(PIN_LED_R, on ? LOW : HIGH); }

// ───────────────────────── speaker ─────────────────────────
// A square wave from LEDC on the DAC pin, at a low duty cycle so it isn't
// harsh through a 1.5W amp. The amp is only enabled while a beep plays,
// which also keeps it from hissing. Silent if no speaker is plugged in.
static uint32_t _beepUntil = 0;
static bool _beeping = false;

void halBeep(uint16_t freq, uint16_t durMs) {
  if (!freq || !durMs) return;
  digitalWrite(PIN_AMP_EN, LOW);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_SPK, freq);
  ledcWrite(PIN_SPK, 40);           // of 1023 (10-bit after WriteTone)
#else
  ledcWriteTone(SPK_CH, freq);
  ledcWrite(SPK_CH, 40);
#endif
  _beepUntil = millis() + durMs;
  _beeping = true;
}
static void beepPoll() {
  if (!_beeping || (int32_t)(millis() - _beepUntil) < 0) return;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_SPK, 0);
#else
  ledcWriteTone(SPK_CH, 0);
#endif
  digitalWrite(PIN_AMP_EN, HIGH);
  _beeping = false;
}
static void audioBegin() {
  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, HIGH);   // off
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_SPK, 2000, 10);
  ledcWrite(PIN_SPK, 0);
#else
  ledcSetup(SPK_CH, 2000, 10);
  ledcAttachPin(PIN_SPK, SPK_CH);
  ledcWrite(SPK_CH, 0);
#endif
}

float halTempC() { return temperatureRead(); }

// ───────────────────────── init / update ─────────────────────────
void halBegin() {
  pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
  digitalWrite(PIN_LED_R, HIGH); digitalWrite(PIN_LED_G, HIGH); digitalWrite(PIN_LED_B, HIGH);

  Serial.println("hal: ESP32-32E, starting display");
  tft.init();
  tft.setRotation(0);
  // Bring-up check: backlight pin driven straight high (no PWM) and the
  // panel filled red, green, blue. Tells a dead backlight from a panel that
  // isn't taking commands.
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  Serial.println("hal: backlight forced on, filling red / green / blue");
  tft.fillScreen(TFT_RED);   delay(400);
  tft.fillScreen(TFT_GREEN); delay(400);
  tft.fillScreen(TFT_BLUE);  delay(400);
  tft.fillScreen(TFT_BLACK);
  blBegin();
  Serial.printf("hal: backlight on PWM, pin reads %d\n", digitalRead(PIN_LCD_BL));
  Serial.println("hal: display up, starting touch");
  touchBegin();

  BtnA.begin(PIN_BTN_BOOT);
  BtnB.begin(0xFF);
  if (BtnA.isPressed()) BtnA.lockUntilReleased();

  analogReadResolution(12);
  audioBegin();
}

void halUpdate() {
  BtnA.update();
  BtnB.update();
  touchPoll();
  beepPoll();
}

#endif  // BOARD_ESP32_32E
