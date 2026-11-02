#pragma once
// Hardware layer for the Waveshare ESP32-S3-Touch-LCD-3.49 / 3.49B (V2 board,
// PCB silkscreen "Rev1.1"). Replaces the M5StickCPlus library the original
// firmware was written against. Everything board-specific lives here and in
// hal.cpp, so the UI code only sees: a 640x172 landscape sprite, two
// buttons, a tap point, an accelerometer, a clock, a battery voltage.
//
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

#include <Arduino.h>
// FS before TFT_eSPI: TFT_eSPI defines FS_NO_GLOBALS, which would hide the
// global File type that xfer.h and character.cpp use.
#include <FS.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>

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

// Logical (landscape) screen size. The panel is natively 172x640 portrait;
// halPush() rotates the sprite in software as it streams it out.
static const int SCREEN_W = 640;
static const int SCREEN_H = 172;
// Layout: the pet lives in the left PET_PANE_W pixels, everything else
// (transcript, approvals, info pages, menus, clock) in the panel to its right.
static const int PET_PANE_W = 200;

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

// Laid out like the M5StickC Plus: the big middle PWR button is A (main
// action, hold for menu) and the BOOT button (backlight icon on the 3.49B
// case) is B (second action, hold 3s to power off).
extern HalButton BtnA;   // PWR, GPIO16
extern HalButton BtnB;   // BOOT, GPIO0

void halBegin();
void halUpdate();                    // buttons + touch; call once per loop

// Display
void halPush(TFT_eSprite& spr);      // stream the 640x172 sprite to the panel
void halSetFlip(bool flipped);       // rotate output 180 degrees
void halBacklight(uint8_t percent);  // 0..100
void halScreenOn(bool on);

// Touch: true once per new touch-down, with landscape coordinates.
bool halTouchTapped(int* x, int* y);
bool halTouching();

// Sensors and power
bool  halImuOk();
void  halAccel(float* ax, float* ay, float* az);   // in g
void  halRtcGet(RTC_TimeTypeDef* t, RTC_DateTypeDef* d);
void  halRtcSet(const RTC_TimeTypeDef* t, const RTC_DateTypeDef* d);
int   halBatteryMv();
bool  halOnUsb();       // best guess: there is no VBUS sense line
void  halPowerOff();    // drops SYS_EN; only cuts power when on battery
void  halBeep(uint16_t freq, uint16_t durMs);   // ES8311 speaker, non-blocking
float halTempC();
