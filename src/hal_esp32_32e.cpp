#include "hal.h"
#if defined(BOARD_ESP32_32E)
// LCDwiki 3.2inch ESP32-32E (E32R32P with touch / E32N32P without). See
// hal.h for the pin map. TFT_eSPI drives the ST7789 and the XPT2046 touch
// controller on the shared SPI bus; the pins come from the build flags in
// platformio.ini.
#include <Preferences.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include <time.h>

// ───────────────────────── pins ─────────────────────────
static const int PIN_LCD_BL   = 27;   // active high
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
// Resistive, so it needs calibrating. TFT_eSPI's calibrateTouch() returns
// five values that are saved to NVS. Calibration always runs at rotation 0;
// when the picture is flipped, the point is mirrored here instead.
static Preferences _touchPrefs;
static bool _touchDown = false, _tapPending = false, _longPending = false, _longFired = false;
static int  _downX = 0, _downY = 0, _lastX = 0, _lastY = 0;
static int  _tapX = 0, _tapY = 0;
static uint32_t _downSince = 0, _lastTouchPoll = 0, _lastSeenDown = 0;
static const uint32_t LONG_PRESS_MS = 600;
static const int TAP_SLOP_PX = 24;    // more movement than this isn't a tap

void halTouchCalibrate() {
  uint16_t cal[5];
  bool wasFlipped = _flip;
  tft.setRotation(0);
  halScreenOn(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString("Touch calibration", SCREEN_W / 2, SCREEN_H / 2 - 20);
  tft.drawString("Press each corner arrow", SCREEN_W / 2, SCREEN_H / 2);
  tft.drawString("firmly, one at a time", SCREEN_W / 2, SCREEN_H / 2 + 20);
  tft.setTextDatum(TL_DATUM);
  tft.calibrateTouch(cal, TFT_MAGENTA, TFT_BLACK, 15);
  tft.setTouch(cal);
  _touchPrefs.begin("touch", false);
  _touchPrefs.putBytes("cal", cal, sizeof(cal));
  _touchPrefs.end();
  Serial.printf("touch: calibrated %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4]);
  tft.fillScreen(TFT_BLACK);
  halSetFlip(wasFlipped);
  // The press on the last arrow is still down; don't count it as a tap.
  _touchDown = true; _longFired = true;
}

static void touchBegin() {
  pinMode(PIN_TOUCH_IRQ, INPUT);
  uint16_t cal[5];
  _touchPrefs.begin("touch", true);
  size_t n = _touchPrefs.getBytes("cal", cal, sizeof(cal));
  _touchPrefs.end();
  if (n == sizeof(cal)) {
    tft.setTouch(cal);
    Serial.printf("touch: calibration %u %u %u %u %u\n", cal[0], cal[1], cal[2], cal[3], cal[4]);
  } else {
    halTouchCalibrate();   // first boot
  }
}

static void touchPoll() {
  uint32_t now = millis();
  if (now - _lastTouchPoll < 20) return;
  _lastTouchPoll = now;

  uint16_t x = 0, y = 0;
  // getTouch() averages several reads and rejects light presses.
  bool down = tft.getTouch(&x, &y, 400);
  if (down) {
    int sx = x, sy = y;
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

  blBegin();
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
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
