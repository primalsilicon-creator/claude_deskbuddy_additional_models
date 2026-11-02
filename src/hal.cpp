#include "hal.h"
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_sleep.h>

// ───────────────────────── pins ─────────────────────────
static const int PIN_LCD_CS   = 9;
static const int PIN_LCD_CLK  = 10;
static const int PIN_LCD_D0   = 11;
static const int PIN_LCD_D1   = 12;
static const int PIN_LCD_D2   = 13;
static const int PIN_LCD_D3   = 14;
static const int PIN_LCD_BL   = 42;
static const int PIN_I2C_SDA  = 47;
static const int PIN_I2C_SCL  = 48;
static const int PIN_TP_SDA   = 17;
static const int PIN_TP_SCL   = 18;
static const int PIN_BTN_BOOT = 0;
static const int PIN_BTN_PWR  = 16;
static const int PIN_BAT_ADC  = 4;

static const uint8_t ADDR_TCA9554 = 0x20;
static const uint8_t ADDR_RTC     = 0x51;
static const uint8_t ADDR_IMU     = 0x6B;
static const uint8_t ADDR_TOUCH   = 0x3B;

static const uint8_t EXIO_BL_EN   = 1 << 1;
static const uint8_t EXIO_LCD_RST = 1 << 5;
static const uint8_t EXIO_SYS_EN  = 1 << 6;

// Native panel geometry (portrait)
static const int PANEL_W = 172;
static const int PANEL_H = 640;

HalButton BtnA;
HalButton BtnB;

// ───────────────────────── I2C helpers ─────────────────────────
static bool i2cWrite(TwoWire& w, uint8_t addr, uint8_t reg, const uint8_t* data, size_t n) {
  w.beginTransmission(addr);
  w.write(reg);
  if (n) w.write(data, n);
  return w.endTransmission() == 0;
}
static bool i2cWrite1(TwoWire& w, uint8_t addr, uint8_t reg, uint8_t v) {
  return i2cWrite(w, addr, reg, &v, 1);
}
static bool i2cRead(TwoWire& w, uint8_t addr, uint8_t reg, uint8_t* out, size_t n) {
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom((int)addr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) out[i] = w.read();
  return true;
}

// ───────────────────────── TCA9554 expander ─────────────────────────
// Registers: 0 input, 1 output, 2 polarity, 3 config (1 = input)
static uint8_t _exioOut = 0;
static void exioWrite(uint8_t mask, bool high) {
  if (high) _exioOut |= mask; else _exioOut &= ~mask;
  i2cWrite1(Wire, ADDR_TCA9554, 0x01, _exioOut);
}
static void exioBegin() {
  // Hold power first, before anything slow, so a battery boot doesn't drop
  // out the moment the PWR button is released.
  _exioOut = EXIO_SYS_EN | EXIO_LCD_RST;
  i2cWrite1(Wire, ADDR_TCA9554, 0x01, _exioOut);
  uint8_t outputs = EXIO_BL_EN | EXIO_LCD_RST | EXIO_SYS_EN;
  i2cWrite1(Wire, ADDR_TCA9554, 0x03, (uint8_t)~outputs);
}

// ───────────────────────── AXS15231B over QSPI ─────────────────────────
// Command/param writes go out on one data line with opcode 0x02; pixel data
// goes out on four lines with opcode 0x32. In QSPI mode the panel has no
// row-address command, so a frame is always sent top to bottom: the first
// chunk with RAMWR (0x2C), every following chunk with RAMWRC (0x3C).
static spi_device_handle_t _lcd = nullptr;
static const int CHUNK_ROWS = 32;                       // 640 / 32 = 20 chunks
static const size_t CHUNK_PX = PANEL_W * CHUNK_ROWS;
static uint16_t* _chunk[2] = { nullptr, nullptr };
static bool _flip = false;

static void lcdCmd(uint8_t cmd, const uint8_t* data = nullptr, size_t n = 0) {
  spi_transaction_t t = {};
  t.cmd = 0x02;
  t.addr = (uint32_t)cmd << 8;
  t.length = n * 8;
  t.tx_buffer = n ? data : nullptr;
  spi_device_polling_transmit(_lcd, &t);
}

static void lcdBegin() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num   = PIN_LCD_D0;
  bus.miso_io_num   = PIN_LCD_D1;
  bus.quadwp_io_num = PIN_LCD_D2;
  bus.quadhd_io_num = PIN_LCD_D3;
  bus.sclk_io_num   = PIN_LCD_CLK;
  bus.max_transfer_sz = CHUNK_PX * 2;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev = {};
  dev.command_bits = 8;
  dev.address_bits = 24;
  dev.mode = 3;
  dev.clock_speed_hz = 40 * 1000 * 1000;
  dev.spics_io_num = PIN_LCD_CS;
  dev.queue_size = 2;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &_lcd));

  for (int i = 0; i < 2; i++) {
    _chunk[i] = (uint16_t*)heap_caps_malloc(CHUNK_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  }

  // Hardware reset through the expander, same timing as Waveshare's demo.
  exioWrite(EXIO_LCD_RST, true);  delay(30);
  exioWrite(EXIO_LCD_RST, false); delay(250);
  exioWrite(EXIO_LCD_RST, true);  delay(30);

  lcdCmd(0x11); delay(100);                      // sleep out
  uint8_t madctl = 0x00; lcdCmd(0x36, &madctl, 1);
  uint8_t colmod = 0x55; lcdCmd(0x3A, &colmod, 1);   // RGB565
  lcdCmd(0x11); delay(100);
  lcdCmd(0x29); delay(100);                      // display on
}

// Fill one native chunk (rows ny0..ny0+CHUNK_ROWS) from the landscape
// sprite. Native (nx, ny) shows logical (ny, 171 - nx), or the 180-degree
// version of that when flipped. The sprite buffer is already byte-swapped
// (big-endian), which is what the panel wants, so pixels copy straight.
static void fillChunk(const uint16_t* fb, uint16_t* out, int ny0) {
  for (int r = 0; r < CHUNK_ROWS; r++) {
    int ny = ny0 + r;
    uint16_t* o = out + r * PANEL_W;
    if (!_flip) {
      const uint16_t* col = fb + ny;                       // logical x = ny
      for (int nx = 0; nx < PANEL_W; nx++) o[nx] = col[(SCREEN_H - 1 - nx) * SCREEN_W];
    } else {
      const uint16_t* col = fb + (SCREEN_W - 1 - ny);      // logical x = 639 - ny
      for (int nx = 0; nx < PANEL_W; nx++) o[nx] = col[nx * SCREEN_W];
    }
  }
}

void halPush(TFT_eSprite& spr) {
  const uint16_t* fb = (const uint16_t*)spr.getPointer();
  if (!fb || !_lcd || !_chunk[0] || !_chunk[1]) return;

  static const uint8_t caset[4] = { 0x00, 0x00, (PANEL_W - 1) >> 8, (PANEL_W - 1) & 0xFF };
  spi_transaction_t tr[2] = {};
  bool inFlight = false;
  int buf = 0;
  for (int ny0 = 0; ny0 < PANEL_H; ny0 += CHUNK_ROWS) {
    fillChunk(fb, _chunk[buf], ny0);        // overlaps with the previous DMA
    if (inFlight) {
      spi_transaction_t* done;
      spi_device_get_trans_result(_lcd, &done, portMAX_DELAY);
    }
    lcdCmd(0x2A, caset, 4);
    spi_transaction_t& t = tr[buf];
    t = {};
    t.flags = SPI_TRANS_MODE_QIO;
    t.cmd = 0x32;
    t.addr = (ny0 == 0 ? 0x2C : 0x3C) << 8;
    t.length = CHUNK_PX * 16;
    t.tx_buffer = _chunk[buf];
    spi_device_queue_trans(_lcd, &t, portMAX_DELAY);
    inFlight = true;
    buf ^= 1;
  }
  if (inFlight) {
    spi_transaction_t* done;
    spi_device_get_trans_result(_lcd, &done, portMAX_DELAY);
  }
}

void halSetFlip(bool flipped) { _flip = flipped; }

// ───────────────────────── backlight ─────────────────────────
static const int BL_CH = 1;
static uint8_t _blPercent = 100;
static bool _screenOn = true;

// The backlight PWM is inverted on this board: duty 255 is dark, 0 is full
// brightness (Waveshare's demo defines LCD_PWM_MODE_255 as 0xff - 255).
static void blApply() {
  uint32_t level = _screenOn ? (uint32_t)_blPercent * 255 / 100 : 0;
  uint32_t duty = 255 - level;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LCD_BL, duty);
#else
  ledcWrite(BL_CH, duty);
#endif
  exioWrite(EXIO_BL_EN, _screenOn && _blPercent > 0);
}
static void blBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LCD_BL, 50000, 8);
#else
  ledcSetup(BL_CH, 50000, 8);
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
  pinMode(pin, INPUT_PULLUP);
  _raw = _pressed = digitalRead(pin) == LOW;
  _since = _rawSince = millis();
}
void HalButton::update() {
  _wasPressed = _wasReleased = false;
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
static bool _touchDown = false, _tapPending = false;
static int  _tapX = 0, _tapY = 0;
static uint32_t _lastTouchPoll = 0;

static void touchPoll() {
  if (millis() - _lastTouchPoll < 25) return;
  _lastTouchPoll = millis();
  static const uint8_t cmd[11] = { 0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 };
  Wire1.beginTransmission(ADDR_TOUCH);
  Wire1.write(cmd, sizeof(cmd));
  if (Wire1.endTransmission(false) != 0) return;
  uint8_t b[14] = {0};
  if (Wire1.requestFrom((int)ADDR_TOUCH, (int)sizeof(b)) != (int)sizeof(b)) return;
  for (size_t i = 0; i < sizeof(b); i++) b[i] = Wire1.read();

  bool down = b[1] > 0 && b[1] < 5;
  if (down && !_touchDown) {
    int tx = ((b[2] & 0x0F) << 8) | b[3];   // along the long edge
    int ty = ((b[4] & 0x0F) << 8) | b[5];   // along the short edge
    if (tx > PANEL_H - 1) tx = PANEL_H - 1;
    if (ty > PANEL_W - 1) ty = PANEL_W - 1;
    // Native panel coordinates, as Waveshare's LVGL demo maps them...
    int nx = ty, ny = PANEL_H - 1 - tx;
    // ...then the same rotation halPush() uses.
    if (!_flip) { _tapX = ny;                _tapY = SCREEN_H - 1 - nx; }
    else        { _tapX = SCREEN_W - 1 - ny; _tapY = nx; }
    _tapPending = true;
    Serial.printf("touch: raw %d,%d -> screen %d,%d\n", tx, ty, _tapX, _tapY);
  }
  _touchDown = down;
}

bool halTouchTapped(int* x, int* y) {
  if (!_tapPending) return false;
  _tapPending = false;
  if (x) *x = _tapX;
  if (y) *y = _tapY;
  return true;
}
bool halTouching() { return _touchDown; }

// ───────────────────────── QMI8658 accelerometer ─────────────────────────
static bool _imuOk = false;
static void imuBegin() {
  uint8_t id = 0;
  if (!i2cRead(Wire, ADDR_IMU, 0x00, &id, 1) || id != 0x05) {
    Serial.printf("imu: QMI8658 not found (id 0x%02X)\n", id);
    return;
  }
  i2cWrite1(Wire, ADDR_IMU, 0x60, 0xB0);   // soft reset
  delay(20);
  i2cWrite1(Wire, ADDR_IMU, 0x02, 0x40);   // CTRL1: register auto-increment
  i2cWrite1(Wire, ADDR_IMU, 0x03, 0x16);   // CTRL2: accel +-4g, ~117Hz
  i2cWrite1(Wire, ADDR_IMU, 0x08, 0x01);   // CTRL7: accel on
  _imuOk = true;
}
bool halImuOk() { return _imuOk; }

void halAccel(float* ax, float* ay, float* az) {
  // Upright default so the face-down and shake logic sees "at rest".
  *ax = 0; *ay = 0; *az = 1.0f;
  if (!_imuOk) return;
  uint8_t b[6];
  if (!i2cRead(Wire, ADDR_IMU, 0x35, b, 6)) return;
  const float LSB_PER_G = 8192.0f;
  *ax = (int16_t)(b[1] << 8 | b[0]) / LSB_PER_G;
  *ay = (int16_t)(b[3] << 8 | b[2]) / LSB_PER_G;
  *az = (int16_t)(b[5] << 8 | b[4]) / LSB_PER_G;
}

// ───────────────────────── PCF85063 RTC ─────────────────────────
static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

void halRtcGet(RTC_TimeTypeDef* t, RTC_DateTypeDef* d) {
  uint8_t b[7];
  if (!i2cRead(Wire, ADDR_RTC, 0x04, b, 7)) return;
  t->Seconds = bcd2bin(b[0] & 0x7F);
  t->Minutes = bcd2bin(b[1] & 0x7F);
  t->Hours   = bcd2bin(b[2] & 0x3F);
  d->Date    = bcd2bin(b[3] & 0x3F);
  d->WeekDay = b[4] & 0x07;
  d->Month   = bcd2bin(b[5] & 0x1F);
  d->Year    = 2000 + bcd2bin(b[6]);
}
void halRtcSet(const RTC_TimeTypeDef* t, const RTC_DateTypeDef* d) {
  uint8_t b[7] = {
    bin2bcd(t->Seconds), bin2bcd(t->Minutes), bin2bcd(t->Hours),
    bin2bcd(d->Date), (uint8_t)(d->WeekDay & 0x07), bin2bcd(d->Month),
    bin2bcd((uint8_t)(d->Year % 100)),
  };
  i2cWrite(Wire, ADDR_RTC, 0x04, b, 7);
}

// ───────────────────────── battery / power ─────────────────────────
static int _batMv = 0;
static uint32_t _lastBatRead = 0;
int halBatteryMv() {
  if (_batMv == 0 || millis() - _lastBatRead > 500) {
    _lastBatRead = millis();
    int mv = analogReadMilliVolts(PIN_BAT_ADC) * 3;   // 1/3 divider
    _batMv = _batMv == 0 ? mv : (_batMv * 3 + mv) / 4;
  }
  return _batMv;
}
// No VBUS sense on this board. A battery on the charger (or no battery at
// all) reads close to 4.2V, so treat that as "on USB".
bool halOnUsb() { return halBatteryMv() > 4150; }

void halPowerOff() {
  halScreenOn(false);
  exioWrite(EXIO_SYS_EN, false);   // on battery, power drops here
  delay(500);
  // Still running, so we're on USB power. Sleep until BOOT is pressed.
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_BOOT, 0);
  esp_deep_sleep_start();
}

void halBeep(uint16_t, uint16_t) {
  // The board has an ES8311 codec and speaker but no simple buzzer.
  // Sound is not wired up yet.
}

float halTempC() { return temperatureRead(); }

// ───────────────────────── init / update ─────────────────────────
void halBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  exioBegin();
  Wire1.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);

  blBegin();
  lcdBegin();

  BtnA.begin(PIN_BTN_BOOT);
  BtnB.begin(PIN_BTN_PWR);
  if (BtnB.isPressed()) BtnB.lockUntilReleased();

  analogReadResolution(12);
  imuBegin();
}

void halUpdate() {
  BtnA.update();
  BtnB.update();
  touchPoll();
}
