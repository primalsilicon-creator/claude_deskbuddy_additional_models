#pragma once
// Hardware layer. The UI code only sees: one or two sprites, two buttons,
// tap points, an accelerometer, a clock, a battery voltage. Everything
// board-specific lives here and in one hal_<board>.cpp. Pick the board with
// a build flag (platformio.ini sets it per env):
//
//   BOARD_WAVESHARE_349  Waveshare ESP32-S3-Touch-LCD-3.49 / 3.49B, V2
//                        board (PCB "Rev1.1"). The default if none is set.
//   BOARD_ESP32_32E      LCDwiki 3.2inch ESP32-32E (E32R32P / E32N32P).
//
// ── Waveshare 3.49 V2 (hal_waveshare349.cpp) ──
// Pin map (from waveshareteam/ESP32-S3-Touch-LCD-3.49-V2 user_config.h):
//   LCD  AXS15231B QSPI  CS 9  CLK 10  D0..D3 11..14  TE 21  BL 42 (PWM)
//   LCD reset and backlight enable sit on the TCA9554 expander.
//   I2C0 SDA 47 / SCL 48: TCA9554 0x20, PCF85063 RTC 0x51, QMI8658 IMU 0x6B
//   I2C1 SDA 17 / SCL 18: AXS15231B touch 0x3B
//   BOOT button GPIO0, PWR button GPIO16 (both active low)
//   ES8311 speaker codec 0x18 on I2C0; I2S MCLK 7, BCLK 15, WS 46, DOUT 45;
//   speaker amp enable on TCA9554 P7
//   Battery sense GPIO4 through a 1/3 divider
//   TCA9554: P0 touch int, P1 BL_EN, P2/P3 IMU int, P4 RTC int,
//            P5 LCD_RST, P6 SYS_EN (holds battery power on), P7 NS mode
// V1 boards swap LCD_TE/LCD_RESET and LCD_BL/EXIO_INT. On a V1 board the
// screen stays dark with this map.
//
// ── LCDwiki 3.2" ESP32-32E (hal_esp32_32e.cpp) ──
// Classic ESP32 (no PSRAM), 4MB flash. From lcdwiki.com/3.2inch_ESP32-32E_Display:
//   LCD  ST7789 240x320 SPI  CS 15  DC 2  SCK 14  MOSI 13  MISO 12,
//        reset tied to EN, backlight IO27 (active high)
//   Touch XPT2046 on the same SPI, CS 33, PENIRQ 36 (low while touched)
//   RGB LED (active low) R 22  G 16  B 17
//   Speaker: DAC IO26 into an FM8002E amp, amp enable IO4 (low = on)
//   Battery ADC IO34, BOOT button IO0. No IMU, no RTC chip.
// Many cases for this board cover every button, so it is run touch-only:
// tap to act, long-press for the menu.

#if !defined(BOARD_WAVESHARE_349) && !defined(BOARD_ESP32_32E)
#define BOARD_WAVESHARE_349 1
#endif

#include <Arduino.h>
// FS before TFT_eSPI: TFT_eSPI defines FS_NO_GLOBALS, which would hide the
// global File type that xfer.h and character.cpp use.
#include <FS.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>

// The TFT_eSPI object behind the sprites (defined in main.cpp). On the
// Waveshare it is never initialised: hal drives that panel itself. On the
// 32E it is the real ST7789 driver.
extern TFT_eSPI tft;

// M5StickCPlus exported these colour names; the UI code uses them.
#ifndef BLACK
#define BLACK 0x0000
#endif
#ifndef WHITE
#define WHITE 0xFFFF
#endif
#ifndef RED
#define RED   0xF800
#endif
#ifndef GREEN
#define GREEN 0x07E0
#endif

#if defined(BOARD_WAVESHARE_349)
// Logical (landscape) screen size. The panel is natively 172x640 portrait;
// halPush() rotates the sprite in software as it streams it out.
static const int SCREEN_W = 640;
static const int SCREEN_H = 172;
// Layout: the pet lives in the left PET_PANE_W pixels, everything else
// (transcript, approvals, info pages, menus, clock) in the panel to its right.
// Both share one 640x172 sprite in PSRAM.
static const int PET_PANE_W = 200;
static const int PET_PANE_H = SCREEN_H;
static const bool TOUCH_ONLY = false;
#else
// Portrait 240x320. With no PSRAM a full-screen 16-bit sprite (150KB) does
// not fit next to BLE, so the screen is two sprites: the pet in full colour
// across the top PET_PANE_H rows, and the panel (transcript, approvals,
// menus...) below it in an 8-bit sprite.
static const int SCREEN_W = 240;
static const int SCREEN_H = 320;
static const int PET_PANE_W = 240;
static const int PET_PANE_H = 130;
static const int PANEL_SPR_H = SCREEN_H - PET_PANE_H;
static const bool TOUCH_ONLY = true;
#endif

// Same field order as the M5 types, so the brace-initialisers in data.h
// keep working.
struct RTC_TimeTypeDef { uint8_t Hours, Minutes, Seconds; };
struct RTC_DateTypeDef { uint8_t WeekDay, Month, Date; uint16_t Year; };

// Debounced button with the subset of the M5 Button API the firmware uses.
class HalButton {
public:
  void begin(uint8_t pin);
  void update();
  bool isPressed() const { return _pressed && !_locked; }
  bool wasPressed() const { return _wasPressed; }
  bool wasReleased() const { return _wasReleased; }
  bool pressedFor(uint32_t ms) const { return isPressed() && millis() - _since >= ms; }
  // Ignore this button until it has been seen released once. Used for PWR,
  // which is still held down from switching the board on when setup() runs.
  void lockUntilReleased() { _locked = true; }
private:
  uint8_t  _pin = 0xFF;
  bool     _pressed = false, _raw = false, _wasPressed = false, _wasReleased = false;
  bool     _locked = false;
  uint32_t _since = 0, _rawSince = 0;
};

// Waveshare: laid out like the M5StickC Plus. The big middle PWR button is
// A (main action, hold for menu) and the BOOT button (backlight icon on the
// 3.49B case) is B (second action, hold 3s to power off).
// ESP32-32E: BOOT (IO0, usually hidden by the case) is A, and B has no
// button behind it.
extern HalButton BtnA;
extern HalButton BtnB;

void halBegin();
void halUpdate();                    // buttons + touch; call once per loop

// Display
void halPush(TFT_eSprite& spr);      // stream the main sprite (Waveshare: whole screen; 32E: panel)
void halPushPet(TFT_eSprite& pet);   // 32E: the pet sprite at the top. No-op on Waveshare.
void halSetFlip(bool flipped);       // rotate output 180 degrees
void halBacklight(uint8_t percent);  // 0..100
void halScreenOn(bool on);

// Touch: true once per tap, in screen coordinates. Waveshare reports it on
// touch-down; the 32E reports it on release, so a long press isn't a tap.
bool halTouchTapped(int* x, int* y);
bool halTouching();
bool halTouchLongPressed();          // once per press held past 600ms (32E only)
// 32E resistive touch: run the four-corner calibration on screen and save
// it. Runs by itself on first boot. No-op on Waveshare.
void halTouchCalibrate();

// Sensors and power
bool  halImuOk();
void  halAccel(float* ax, float* ay, float* az);   // in g
void  halRtcGet(RTC_TimeTypeDef* t, RTC_DateTypeDef* d);
void  halRtcSet(const RTC_TimeTypeDef* t, const RTC_DateTypeDef* d);
int   halBatteryMv();
bool  halOnUsb();       // best guess: there is no VBUS sense line
void  halPowerOff();    // drops SYS_EN; only cuts power when on battery
void  halBeep(uint16_t freq, uint16_t durMs);   // speaker, non-blocking
void  halLed(bool on);  // 32E: red LED for "attention". No-op on Waveshare.
float halTempC();
