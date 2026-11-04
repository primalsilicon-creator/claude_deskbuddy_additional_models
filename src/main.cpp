#include "hal.h"
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include "ble_bridge.h"
#include "board_id.h"
#include "data.h"
#include "buddy.h"

// Waveshare: TFT_eSPI is only the drawing library. The sprite lives in
// PSRAM and halPush() streams it to the AXS15231B panel; `tft` is never
// initialised, so it never touches any pins.
// ESP32-32E: `tft` is the real ST7789 driver. `spr` is the panel below the
// pet (8-bit to save RAM) and `petSpr` is the pet pane in full colour.
#if defined(TFT_PANEL_W)
TFT_eSPI tft(TFT_PANEL_W, TFT_PANEL_H);   // panel size the driver header can't express
#else
TFT_eSPI tft;
#endif
TFT_eSprite spr = TFT_eSprite(&tft);
#if defined(BOARD_ESP32_32E)
static TFT_eSprite _petSprite = TFT_eSprite(&tft);
TFT_eSprite& petSpr = _petSprite;
#else
TFT_eSprite& petSpr = spr;
#endif

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  // The desktop only lists devices whose name starts with "Claude", so the
  // board's short ID goes after that prefix.
  snprintf(btName, sizeof(btName), "Claude-%s", boardShortId());
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
#if defined(BOARD_WAVESHARE_349)
// Landscape 640x172. The pet draws in the left pane (x < PET_PANE_W) in
// absolute coordinates. Everything else draws inside a viewport over the
// right-hand panel, so W/H/CX below are the panel's size, not the screen's.
// The case bezel covers the panel's outer few pixels, so everything is
// inset: the panel by PANEL_PAD_X on each side and PANEL_TOP / PANEL_BOTTOM
// vertically, the pet by PET_TOP.
const int PANEL_PAD_X = 6, PANEL_TOP = 8, PANEL_BOTTOM = 6, PET_TOP = 8;
const int PANEL_X0 = PET_PANE_W;          // panel origin in the main sprite
const int W = SCREEN_W - PET_PANE_W - 2 * PANEL_PAD_X;
const int H = SCREEN_H - PANEL_TOP - PANEL_BOTTOM;
// Centre of full-screen messages (splash, "bye!") in the main sprite
const int MSG_CX = SCREEN_W / 2, MSG_CY = SCREEN_H / 2;
#else
// Portrait 240x320: the pet sprite is the top PET_PANE_H rows and `spr`
// (the panel) is everything below it, so panel coordinates start at the
// top-left of `spr`.
const int PANEL_PAD_X = 6, PANEL_TOP = 4, PANEL_BOTTOM = 6, PET_TOP = 0;
const int PANEL_X0 = 0;
const int W = SCREEN_W - 2 * PANEL_PAD_X;
const int H = PANEL_SPR_H - PANEL_TOP - PANEL_BOTTOM;
const int MSG_CX = SCREEN_W / 2, MSG_CY = PANEL_SPR_H / 2;
#endif
const int CX = W / 2;
// "Attention" flashes a border around the screen (and the hidden LED on
// the 32E).
const int FLASH_PX = 6;   // thick enough to show past the bezel
// QMI8658 Z axis sign when the screen faces down. Calibrated at boot: if the
// board is lying flat when it starts, that is taken as face-up, and face-down
// is the opposite sign. Otherwise this default is used.
float FACE_DOWN_Z_SIGN = -1.0f;

static void panelBegin() { spr.setViewport(PANEL_X0 + PANEL_PAD_X, PANEL_TOP, W, H, true); }
#if defined(BOARD_WAVESHARE_349)
static void petBegin()   { spr.setViewport(0, PET_TOP, PET_PANE_W - 1, SCREEN_H - PET_TOP, true); }
#else
static void petBegin()   {}   // the pet has its own sprite
#endif
static void panelEnd()   { spr.resetViewport(); }

// Send the frame to the screen: on the 32E the pet sprite, then the panel.
static void pushFrame() {
  halPushPet(petSpr);
  halPush(spr);
}

// Border flash around the whole screen. On the 32E it spans both sprites.
static void drawFlashBorder(uint16_t c) {
#if defined(BOARD_WAVESHARE_349)
  for (int i = 0; i < FLASH_PX; i++) spr.drawRect(i, i, SCREEN_W - 2 * i, SCREEN_H - 2 * i, c);
#else
  petSpr.fillRect(0, 0, SCREEN_W, FLASH_PX, c);
  petSpr.fillRect(0, 0, FLASH_PX, PET_PANE_H, c);
  petSpr.fillRect(SCREEN_W - FLASH_PX, 0, FLASH_PX, PET_PANE_H, c);
  spr.fillRect(0, PANEL_SPR_H - FLASH_PX, SCREEN_W, FLASH_PX, c);
  spr.fillRect(0, 0, FLASH_PX, PANEL_SPR_H, c);
  spr.fillRect(SCREEN_W - FLASH_PX, 0, FLASH_PX, PANEL_SPR_H, c);
#endif
}

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  halAccel(&ax, &ay, &az);
  return az * FACE_DOWN_Z_SIGN > 0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { halBacklight(20 + brightLevel * 20); }

static uint32_t screenOffAt = 0;
static void wake() {
  lastInteractMs = millis();
  if (screenOff && !halImuOk()) {
    // No accelerometer, so no face-down nap: a long enough screen-off
    // counts as one instead, so energy still refills.
    uint32_t off = (millis() - screenOffAt) / 1000;
    if (off >= 60) { statsOnNapEnd(off); statsOnWake(); }
  }
  if (screenOff) {
    halScreenOn(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) halBeep(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  // The pet has its own pane now, so it never shrinks into "peek" mode.
  characterSetPeek(false);
  buddySetPeek(false);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
// The original firmware's "bluetooth" and "wifi" items only stored a
// preference (BLE is always on, there is no WiFi), so they are gone.
enum {
  SET_BRIGHT, SET_SOUND, SET_FLASH, SET_HUD, SET_FLIP, SET_PET, SET_RESET,
#if defined(BOARD_ESP32_32E)
  SET_TOUCHCAL,
#endif
  SET_BACK, SETTINGS_COUNT
};
const char* settingsItems[] = { "brightness", "sound", "flash", "transcript", "flip", "ascii pet", "reset",
#if defined(BOARD_ESP32_32E)
  "touch cal",
#endif
  "back" };
const uint8_t SETTINGS_N = SETTINGS_COUNT;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case SET_BRIGHT:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case SET_SOUND: s.sound = !s.sound; break;
    case SET_FLASH: s.led = !s.led; break;
    case SET_HUD:   s.hud = !s.hud; break;
    case SET_FLIP:  s.clockRot = (s.clockRot + 1) % 2; halSetFlip(s.clockRot); break;
    case SET_PET:   nextPet(); return;
    case SET_RESET: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
#if defined(BOARD_ESP32_32E)
    case SET_TOUCHCAL:
      halTouchCalibrate();
      settingsOpen = false;
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      return;
#endif
    case SET_BACK:  settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
const int MENU_ROW = 12;   // item pitch; 10 settings rows fit the inset panel
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

#if defined(BOARD_ESP32_32E)
// Touch-only: menus are a grid of big buttons filling the panel instead of
// a list stepped through with A and picked with B. A tap picks a button,
// a long press closes the menu.
const int GRID_TOP = 20, GRID_GAP = 4;
static void gridCell(uint8_t n, uint8_t i, int* x, int* y, int* w, int* h) {
  int cols = n > 3 ? (W > 280 ? 3 : 2) : 1, rows = (n + cols - 1) / cols;
  int cw = (W - (cols - 1) * GRID_GAP) / cols;
  int ch = (H - GRID_TOP - (rows - 1) * GRID_GAP) / rows;
  if (ch > 44) ch = 44;
  *x = (i % cols) * (cw + GRID_GAP);
  *y = GRID_TOP + (i / cols) * (ch + GRID_GAP);
  *w = cw; *h = ch;
}
// Panel coordinates to item index, or -1 for a miss.
static int gridHit(uint8_t n, int px, int py) {
  for (uint8_t i = 0; i < n; i++) {
    int x, y, w, h; gridCell(n, i, &x, &y, &w, &h);
    if (px >= x && px < x + w && py >= y && py < y + h) return i;
  }
  return -1;
}
// values may be null; hot is an item to show armed ("really?").
static void drawGrid(const char* title, const char* const* labels, char values[][8],
                     uint8_t n, uint16_t border, int hot = -1) {
  const Palette& p = characterPalette();
  spr.fillRect(0, 0, W, H, PANEL);
  spr.setTextFont(2); spr.setTextSize(1);
  spr.setTextColor(p.text, PANEL);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(title, 2, 0);
  spr.setTextFont(1);
  spr.setTextColor(p.textDim, PANEL);
  spr.setTextDatum(TR_DATUM);
  spr.drawString("hold: close", W - 2, 4);
  for (uint8_t i = 0; i < n; i++) {
    int x, y, w, h; gridCell(n, i, &x, &y, &w, &h);
    bool armed = (int)i == hot;
    spr.fillRoundRect(x, y, w, h, 5, 0x0000);
    spr.drawRoundRect(x, y, w, h, 5, armed ? HOT : border);
    bool hasVal = values && values[i][0];
    spr.setTextDatum(MC_DATUM);
    spr.setTextFont(2);
    spr.setTextColor(armed ? HOT : p.text, 0x0000);
    spr.drawString(armed ? "really?" : labels[i], x + w / 2, y + h / 2 - (hasVal ? 5 : 0));
    if (hasVal) {
      spr.setTextFont(1);
      bool on = !strcmp(values[i], "on");
      spr.setTextColor(on ? GREEN : p.textDim, 0x0000);
      spr.drawString(values[i], x + w / 2, y + h / 2 + 9);
    }
  }
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(1);
}
#endif

static void drawSettings() {
#if defined(BOARD_ESP32_32E)
  {
    Settings& s = settings();
    static char v[SETTINGS_N][8];
    for (int i = 0; i < SETTINGS_N; i++) v[i][0] = 0;
    snprintf(v[SET_BRIGHT], 8, "%u/4", brightLevel);
    strcpy(v[SET_SOUND], s.sound ? "on" : "off");
    strcpy(v[SET_FLASH], s.led ? "on" : "off");
    strcpy(v[SET_HUD],   s.hud ? "on" : "off");
    strcpy(v[SET_FLIP],  s.clockRot ? "on" : "off");
    uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
    uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
    snprintf(v[SET_PET], 8, "%u/%u", pos, total);
    drawGrid("Settings", settingsItems, v, SETTINGS_N, characterPalette().textDim);
    return;
  }
#endif
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + SETTINGS_N * MENU_ROW + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * MENU_ROW);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * MENU_ROW);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 3) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 4) {
      spr.setTextColor(s.clockRot ? GREEN : p.textDim, PANEL);
      spr.print(s.clockRot ? " on" : "off");
    } else if (i == 5) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawReset() {
#if defined(BOARD_ESP32_32E)
  {
    int hot = ((int32_t)(millis() - resetConfirmUntil) < 0) ? resetConfirmIdx : -1;
    drawGrid("Reset", resetItems, nullptr, RESET_N, HOT, hot);
    return;
  }
#endif
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * MENU_ROW + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * MENU_ROW);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: halPowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
#if defined(BOARD_ESP32_32E)
  {
    static char v[MENU_N][8];
    for (int i = 0; i < MENU_N; i++) v[i][0] = 0;
    strcpy(v[4], dataDemo() ? "on" : "off");
    drawGrid("Menu", menuItems, v, MENU_N, characterPalette().textDim);
    return;
  }
#endif
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * MENU_ROW + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * MENU_ROW);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads. Cache the time once per second; mood logic and drawClock both
// read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = halOnUsb();
  halRtcGet(&_clkTm, &_clkDt);
}

// Clock face: shown when on USB power with nothing else going on. Fills the
// right-hand panel; the pet keeps living in its own pane.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[16]; snprintf(dl, sizeof(dl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);

  spr.fillRect(0, 0, W, H, p.bg);
  spr.setTextDatum(MC_DATUM);
#if defined(BOARD_WAVESHARE_349)
  spr.setTextSize(8); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX - 24, 66);
  spr.setTextSize(3); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX + 150, 82);
  spr.setTextSize(2);                                     spr.drawString(dl, CX, 140);
#else
  spr.setTextSize(6); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 50);
  spr.setTextSize(3); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss + 1, CX, 102);
  spr.setTextSize(2);                                     spr.drawString(dl, CX, 146);
#endif
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  halAccel(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillRect(0, 0, W, H, p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("BLUETOOTH PAIRING", CX, 24);
  spr.drawString("enter on desktop:", CX, 146);
  spr.setTextSize(6);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.drawString(b, CX, 86);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 0;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
#if defined(BOARD_WAVESHARE_349)
    ln("Press PWR or tap Approve on a");
    ln("prompt to approve from here.");
#else
    ln("Tap Approve or Deny on a");
    ln("prompt to answer from here.");
#endif
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
#if defined(BOARD_ESP32_32E)
    _infoHeader(p, y, "TOUCH", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("Tap the pet");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Tap the panel");
    spr.setTextColor(p.textDim, p.bg); ln("    next page; on the transcript,");
    ln("    top half older, bottom newer"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Hold anywhere");
    spr.setTextColor(p.textDim, p.bg); ln("    menu (tap a button to pick)"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Prompts");
    spr.setTextColor(p.textDim, p.bg); ln("    tap Approve / Deny"); y += 4;
    ln("Touch off? settings > touch cal");
#else
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("PWR (A, middle)");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen / approve prompt");
    ln("    hold: menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("BOOT (B, backlight icon)");
    spr.setTextColor(p.textDim, p.bg); ln("    next page / deny prompt");
    ln("    hold 3s: power off"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Touch");
    spr.setTextColor(p.textDim, p.bg); ln("    tap pet = A, tap panel = B");
    ln("    prompts: tap Approve / Deny");
#endif

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    // No current or VBUS sensing on this board: battery voltage only.
    int vBat_mV = halBatteryMv();
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = halOnUsb();
    bool charging = false;
    bool full = usb;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "usb / full" : (charging ? "charging" : "battery"));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", dataBtActive() ? "linked" : "on");
    ln("  temp     %dC (chip)", (int)halTempC());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = dataBtActive();

    spr.setTextColor(linked ? GREEN : HOT, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : "discover");
    // This unit's short ID, big enough to read across a desk.
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(W - 4 - 4 * 12, y);
    spr.print(boardShortId());
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    ln("  %s", boardMacStr());
    ln("  %s", BOARD_MODEL);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("%s", BOARD_MODEL);
    ln("%s  id %s", ESP.getChipModel(), boardShortId());
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
const uint8_t WRAP_MAX = 70;   // widest wrap: 428px panel text / 6px glyphs
static uint8_t wrapInto(const char* in, char out[][WRAP_MAX + 2], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// Approve / Deny touch targets along the bottom of the panel (panel coords).
#if defined(BOARD_WAVESHARE_349)
static const char* const APPROVE_LBL = "Approve (PWR)";
static const char* const DENY_LBL    = "Deny (BOOT)";
const uint8_t HINT_WRAP = WRAP_MAX;
const int HINT_ROWS_MAX = 5;
#else
static const char* const APPROVE_LBL = "Approve";
static const char* const DENY_LBL    = "Deny";
const uint8_t HINT_WRAP = (W - 12) / 6;
const int HINT_ROWS_MAX = 8;
#endif
const int BTN_H = 40, BTN_Y = H - BTN_H - 4;
const int BTN_APPROVE_X = 4,         BTN_DENY_X = W / 2 + 4;
const int BTN_W = W / 2 - 8;

static void drawApproval() {
  const Palette& p = characterPalette();
  spr.fillRect(0, 0, W, H, p.bg);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, 6);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Tool name: size 3 if it fits the panel width, else size 2.
  int toolLen = strlen(tama.promptTool);
  uint8_t ts = (toolLen * 18 <= W - 12) ? 3 : 2;
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(ts);
  spr.setCursor(6, 20);
  int toolMax = (W - 12) / (6 * ts);
  if (toolMax > 35) toolMax = 35;
  spr.printf("%.*s", toolMax, tama.promptTool);
  spr.setTextSize(1);

  // Hint: wrapped to the panel width, as many rows as fit above the buttons.
  const int hintY = 20 + ts * 8 + 6;
  int maxRows = (BTN_Y - 2 - hintY) / 10;
  if (maxRows > HINT_ROWS_MAX) maxRows = HINT_ROWS_MAX;
  static char rows[8][WRAP_MAX + 2];
  uint8_t n = wrapInto(tama.promptHint, rows, maxRows, HINT_WRAP);
  spr.setTextColor(p.textDim, p.bg);
  for (uint8_t i = 0; i < n; i++) {
    spr.setCursor(6, hintY + i * 10);
    spr.print(rows[i]);
  }

  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("sent...", CX, BTN_Y + BTN_H / 2);
  } else {
    spr.fillRoundRect(BTN_APPROVE_X, BTN_Y, BTN_W, BTN_H, 6, GREEN);
    spr.setTextColor(BLACK, GREEN);
    spr.drawString(APPROVE_LBL, BTN_APPROVE_X + BTN_W / 2, BTN_Y + BTN_H / 2);
    spr.fillRoundRect(BTN_DENY_X, BTN_Y, BTN_W, BTN_H, 6, HOT);
    spr.setTextColor(BLACK, HOT);
    spr.drawString(DENY_LBL, BTN_DENY_X + BTN_W / 2, BTN_Y + BTN_H / 2);
  }
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 0;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 0;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
#if defined(BOARD_WAVESHARE_349)
  ln(p.textDim, " face-down to nap");
#else
  ln(p.textDim, " screen off 1min+ = nap");
#endif
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s on battery = screen off");
  ln(p.textDim, "any button or touch = wake"); gap();

#if defined(BOARD_WAVESHARE_349)
  ln(p.textDim, "PWR: screens  BOOT: page  hold PWR: menu");
#else
  ln(p.textDim, "tap pet: screens  hold: menu");
#endif
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 0;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

// Full-width banner along the bottom of the panel, pulsing gently so it
// catches the eye from across the desk.
static void drawDeskWaitBanner(int h) {
  bool on = (millis() / 700) % 2;
  uint16_t bg = on ? HOT : 0x8000;
  spr.fillRoundRect(0, H - h, W, h, 6, bg);
  spr.setTextColor(WHITE, bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextFont(4);
  if (spr.textWidth("Approve on desktop") > W - 8) spr.setTextFont(2);
  spr.drawString("Approve on desktop", W / 2, H - h / 2 + 1);
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(2);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  // A one-line session summary, then the transcript history in TFT_eSPI's
  // 16px font 2: about 8 rows of up to ~50 characters. Font 2 is
  // proportional, so 50 chars of normal text fits the 428px panel.
  const int HDR = 12;
  // About 8.5px per character of font 2: 50 on the Waveshare, 26 on the 32E.
  const int LH = 17, WIDTH = W * 2 / 17;
  // The desktop says a session is waiting but sent no prompt to answer
  // here (it only relays prompts for sessions it tracks). Show a big banner
  // at the bottom so it isn't missed, and keep the transcript above it.
  const bool deskWait = tama.connected && tama.sessionsWaiting > 0;
  const int BANNER = 34;
  const int SHOW = (H - HDR - 2 - (deskWait ? BANNER : 0)) / LH;
  const int AREA = H - HDR;
  spr.fillRect(0, 0, W, H, p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, 1);
  if (tama.connected) {
#if defined(BOARD_WAVESHARE_349)
    spr.printf("sessions %u   running %u   waiting %u", tama.sessionsTotal,
               tama.sessionsRunning, tama.sessionsWaiting);
#else
    spr.printf("sess %u  run %u  wait %u", tama.sessionsTotal,
               tama.sessionsRunning, tama.sessionsWaiting);
#endif
    if (tama.nLines && !deskWait) {
      spr.setCursor(W - 6 * 12, 1);
      spr.printf("%2u lines", tama.nLines);
    }
  }
  spr.drawFastHLine(0, HDR - 2, W, p.textDim);
  spr.setTextFont(2);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2 - (deskWait ? BANNER : 0));
    spr.print(tama.msg);
    if (deskWait) drawDeskWaitBanner(BANNER);
    spr.setTextFont(1);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  // Wrap newest-first so a long history keeps its newest lines when the
  // row buffer fills, then display in oldest-to-newest order.
  const uint8_t MAXROWS = 96;
  static char disp[MAXROWS][WRAP_MAX + 2];
  static uint8_t srcOf[MAXROWS];
  static char tmp[6][WRAP_MAX + 2];
  uint8_t nDisp = 0;
  int firstLine = 0;
  {
    uint8_t rows = 0;
    for (int i = tama.nLines - 1; i >= 0; i--) {
      uint8_t got = wrapInto(tama.lines[i], tmp, 6, WIDTH);
      if (rows + got > MAXROWS) break;
      rows += got; firstLine = i;
    }
  }
  for (uint8_t i = firstLine; i < tama.nLines && nDisp < MAXROWS; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], MAXROWS - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  // Lines the desktop still lists are bright; older history is dimmed.
  int currentFrom = (int)tama.nLines - tama.linesFromLatest;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool current = tama.connected && srcOf[row] >= currentFrom;
    spr.setTextColor(current ? p.text : p.textDim, p.bg);
    spr.setCursor(4, H - AREA + 1 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 24, H - LH - 2 - (deskWait ? BANNER : 0));
    spr.printf("-%u", msgScroll);
  }
  if (deskWait) drawDeskWaitBanner(BANNER);
  spr.setTextFont(1);
}

// Approve or deny the pending prompt. Shared by buttons and touch.
static void answerPrompt(bool approve) {
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
           tama.promptId, approve ? "once" : "deny");
  sendCmd(cmd);
  responseSent = true;
  if (approve) {
    uint32_t tookS = (millis() - promptArrivedMs) / 1000;
    statsOnApproval(tookS);
    beep(2400, 60);
    if (tookS < 5) triggerOneShot(P_HEART, 2000);
  } else {
    statsOnDenial();
    beep(600, 60);
  }
}

// Short-press actions, shared by the buttons and touch taps.
static void actionA(bool inPrompt) {
  if (inPrompt) {
    answerPrompt(true);
  } else if (resetOpen) {
    beep(1800, 30);
    resetSel = (resetSel + 1) % RESET_N;
    resetConfirmIdx = 0xFF;
  } else if (settingsOpen) {
    beep(1800, 30);
    settingsSel = (settingsSel + 1) % SETTINGS_N;
  } else if (menuOpen) {
    beep(1800, 30);
    menuSel = (menuSel + 1) % MENU_N;
  } else {
    beep(1800, 30);
    displayMode = (displayMode + 1) % DISP_COUNT;
    applyDisplayMode();
  }
}

static void actionB(bool inPrompt) {
  if (inPrompt) {
    answerPrompt(false);
  } else if (resetOpen) {
    beep(2400, 30);
    applyReset(resetSel);
  } else if (settingsOpen) {
    beep(2400, 30);
    applySetting(settingsSel);
  } else if (menuOpen) {
    beep(2400, 30);
    menuConfirm();
  } else if (displayMode == DISP_INFO) {
    beep(2400, 30);
    infoPage = (infoPage + 1) % INFO_PAGES;
  } else if (displayMode == DISP_PET) {
    beep(2400, 30);
    petPage = (petPage + 1) % PET_PAGES;
    applyDisplayMode();
  } else {
    beep(2400, 30);
    msgScroll = (msgScroll >= 90) ? 0 : msgScroll + 1;   // clamped to history in drawHUD
  }
}

void setup() {
  Serial.begin(115200);
#if defined(BOARD_ESP32_32E)
  // BLE only: hand the classic-BT controller memory back to the heap now,
  // before anything else allocates. (BLEDevice::init does this too, but
  // only after the heap has been carved up.)
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
#endif
  halBegin();          // latches battery power, brings up panel, I2C, buttons
  boardPrintInfo(Serial);
#if defined(BOARD_WAVESHARE_349)
  // 640x172x2 = 220KB: TFT_eSprite puts it in PSRAM.
  if (!spr.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("FATAL: could not allocate the frame sprite (PSRAM enabled?)");
  }
#endif
  startBt();
#if defined(BOARD_ESP32_32E)
  // No PSRAM. Bluedroid needs its heap first (allocating the sprites before
  // it crashed BLE start), then the frame sprites take what's left: pet
  // 240x130 at 16 bits (61KB) if it fits, else 8 bits; panel 240x190 at
  // 8 bits (45KB).
  Serial.printf("ble up: heap free %u, largest block %u\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  spr.setColorDepth(8);
  if (!spr.createSprite(SCREEN_W, PANEL_SPR_H)) Serial.println("FATAL: no panel sprite");
  petSpr.setColorDepth(16);
  if (!petSpr.createSprite(PET_PANE_W, PET_PANE_H)) {
    Serial.println("pet sprite: no room at 16 bits, trying 8");
    petSpr.setColorDepth(8);
    if (!petSpr.createSprite(PET_PANE_W, PET_PANE_H)) Serial.println("FATAL: no pet sprite");
  }
  Serial.printf("sprites: heap free %u, largest block %u\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
  statsLoad();
  settingsLoad();
  halSetFlip(settings().clockRot == 1);
  petNameLoad();
  buddyInit();
  lastInteractMs = millis();

  // BLE is always on.
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    petSpr.fillSprite(p.bg);
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(3);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, MSG_CX, MSG_CY - 18);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), MSG_CX, MSG_CY + 18);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", MSG_CX, MSG_CY - 18);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", MSG_CX, MSG_CY + 20);
    }
    // Board model and this unit's ID along the bottom, so it's obvious at
    // power-on which board is which.
    spr.setTextSize(1);
    spr.setTextColor(p.textDim, p.bg);
    {
      char line[48];
      snprintf(line, sizeof(line), "%s  -  %s", BOARD_MODEL, boardShortId());
      spr.setTextDatum(BC_DATUM);
      spr.drawString(line, MSG_CX, 2 * MSG_CY - PANEL_BOTTOM - 2);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    pushFrame();
    // Backlight only after the first real frame, so no power-on garbage.
    halScreenOn(true);
    applyBrightness();
    delay(1800);
    spr.fillSprite(p.bg);
    petSpr.fillSprite(p.bg);
  }

  {
    float ax, ay, az; halAccel(&ax, &ay, &az);
    if (halImuOk() && fabsf(az) > 0.7f) FACE_DOWN_Z_SIGN = az > 0 ? -1.0f : 1.0f;
    Serial.printf("imu at boot: %.2f %.2f %.2f, face-down sign %+.0f\n", ax, ay, az, FACE_DOWN_Z_SIGN);
  }
  {
    static const char* const RR[] = { "unknown", "power-on", "external pin", "software",
      "panic/crash", "interrupt watchdog", "task watchdog", "other watchdog",
      "deep sleep", "brownout", "SDIO", "USB", "JTAG" };
    int rr = (int)esp_reset_reason();
    Serial.printf("reset reason: %d (%s)\n", rr, rr < 13 ? RR[rr] : "?");
  }
  Serial.printf("buddy: %s, imu %s, psram free %u, heap free %u (largest %u)\n",
                buddyMode ? "ASCII mode" : "GIF character loaded",
                halImuOk() ? "ok" : "missing", ESP.getFreePsram(),
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void loop() {
  halUpdate();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  // A session started waiting with no prompt relayed to us: wake the
  // screen, chirp, and go back to the transcript so the banner shows.
  static uint8_t lastWaiting = 0;
  if (tama.sessionsWaiting > lastWaiting && !tama.promptId[0] && tama.connected) {
    wake();
    beep(1200, 80);
    displayMode = DISP_NORMAL;
    menuOpen = settingsOpen = resetOpen = false;
    applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }
  lastWaiting = tama.sessionsWaiting;

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (BtnA.isPressed() || BtnB.isPressed()) {
    if (screenOff) {
      if (BtnA.isPressed()) swallowBtnA = true;
      if (BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // B (BOOT) held 3s: power off. On battery the board switches off; on USB
  // it goes into deep sleep until BOOT is pressed.
  static bool btnBLong = false;
  if (BtnB.pressedFor(3000) && !btnBLong) {
    btnBLong = true;
    const Palette& p = characterPalette();
    petSpr.fillSprite(p.bg);
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM); spr.setTextSize(3);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("bye!", MSG_CX, MSG_CY);
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    pushFrame();
    delay(600);
    halPowerOff();
  }

  // Long press (A held, or a held touch on the 32E): open the menu, or
  // back out of whichever menu is open.
  auto longPress = [&]() {
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  };
  if (BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    longPress();
  }
  if (BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) actionA(inPrompt);
    btnALong = false;
    swallowBtnA = false;
  }

  // B acts on release, so holding it for power-off doesn't also fire it.
  if (BtnB.wasReleased()) {
    if (!btnBLong && !swallowBtnB) actionB(inPrompt);
    btnBLong = false;
    swallowBtnB = false;
  }

  int tx, ty;
#if defined(BOARD_ESP32_32E)
  // Touch-only. Any tap or hold wakes the screen (and is swallowed if it
  // was off). Hold anywhere: menu. During a prompt only the Approve / Deny
  // boxes respond. With a menu open, a tap picks a button and a tap on the
  // pet closes it. Otherwise a tap on the pet is A (next screen) and a tap
  // on the panel is B (next page), except on the transcript, where the top
  // half scrolls back and the bottom half forward.
  if (halTouchLongPressed()) {
    bool wasOff = screenOff;
    wake();
    if (!wasOff && !inPrompt) longPress();
  }
  if (halTouchTapped(&tx, &ty)) {
    bool wasOff = screenOff;
    wake();
    if (!wasOff) {
      bool inPanel = ty >= PET_PANE_H;
      int px = tx - PANEL_PAD_X, py = ty - PET_PANE_H - PANEL_TOP;   // panel coords
      if (inPrompt) {
        if (py >= BTN_Y && py < BTN_Y + BTN_H) {
          if (px >= BTN_APPROVE_X && px < BTN_APPROVE_X + BTN_W) answerPrompt(true);
          else if (px >= BTN_DENY_X && px < BTN_DENY_X + BTN_W) answerPrompt(false);
        }
      } else if (resetOpen || settingsOpen || menuOpen) {
        if (!inPanel) {
          beep(800, 60);
          resetOpen = settingsOpen = menuOpen = false;
          characterInvalidate();
        } else {
          uint8_t n = resetOpen ? RESET_N : settingsOpen ? SETTINGS_N : MENU_N;
          int i = gridHit(n, px, py);
          if (i >= 0) {
            beep(2400, 30);
            if (resetOpen)         { resetSel = i; applyReset(i); }
            else if (settingsOpen) { settingsSel = i; applySetting(i); }
            else                   { menuSel = i; menuConfirm(); }
          }
        }
      } else if (!inPanel) {
        actionA(false);
      } else if (displayMode == DISP_NORMAL && settings().hud && !blePasskey()) {
        beep(2400, 30);
        if (py < H / 2) { if (msgScroll < 90) msgScroll++; }   // clamped in drawHUD
        else if (msgScroll > 0) msgScroll--;
      } else {
        actionB(false);
      }
    }
  }
#else
  // Touch. Any tap wakes the screen (and is swallowed if it was off).
  // During a prompt only the Approve / Deny boxes respond. Otherwise a tap
  // on the pet acts like BOOT and a tap on the panel acts like PWR.
  if (halTouchTapped(&tx, &ty)) {
    bool wasOff = screenOff;
    wake();
    if (!wasOff) {
      int px = tx - PET_PANE_W;   // panel coordinates
      if (inPrompt) {
        if (ty >= BTN_Y && ty < BTN_Y + BTN_H) {
          if (px >= BTN_APPROVE_X && px < BTN_APPROVE_X + BTN_W) answerPrompt(true);
          else if (px >= BTN_DENY_X && px < BTN_DENY_X + BTN_W) answerPrompt(false);
        }
      } else if (tx < PET_PANE_W) {
        actionA(false);
      } else {
        actionB(false);
      }
    }
  }
#endif

  // Charging clock: takes over the panel when on USB power, no overlays,
  // no prompt, no live Claude data, and the RTC has been set by the bridge.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;

  static bool wasClocking = false;
  if (clocking != wasClocking) {
    applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // Pet pane, inset from the top bezel
  petBegin();
  if (napping || screenOff) {
    // skip render — face-down or screen off
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    petSpr.fillRect(0, 0, PET_PANE_W, PET_PANE_H, p.bg);
    petSpr.setTextColor(p.textDim, p.bg);
    petSpr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      petSpr.setCursor(8, 66);
      petSpr.print("installing");
      petSpr.setCursor(8, 78);
      petSpr.printf("%luK / %luK", done/1024, total/1024);
      int barW = PET_PANE_W - 16;
      petSpr.drawRect(8, 92, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) petSpr.fillRect(9, 93, fill - 1, 6, p.body);
      }
    } else {
      petSpr.setCursor(8, 80);
      petSpr.print("no character loaded");
    }
  }
  panelEnd();

  if (!napping && !screenOff) {
    // Right-hand panel
    const Palette& p = characterPalette();
    panelBegin();
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    else spr.fillRect(0, 0, W, H, p.bg);
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    panelEnd();

    // Divider between pet and panel
#if defined(BOARD_WAVESHARE_349)
    spr.drawFastVLine(PET_PANE_W - 1, 8, SCREEN_H - 16, p.textDim);
#else
    spr.drawFastHLine(8, 1, SCREEN_W - 16, p.textDim);
#endif

    // Stand-in for the M5's red LED: flash a border while a prompt waits.
    bool flashOn = activeState == P_ATTENTION && settings().led && (now / 400) % 2;
    drawFlashBorder(flashOn ? HOT : p.bg);
    halLed(flashOn);

    pushFrame();
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    { float ax, ay, az; halAccel(&ax, &ay, &az);
      Serial.printf("nap: face-down detected (accel %.2f %.2f %.2f) -> dimming\n", ax, ay, az); }
    halBacklight(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    halScreenOn(false);
    screenOff = true;
    screenOffAt = millis();
    Serial.printf("screen off: idle %lus on battery (vbat %dmV, usb guess %s)\n",
                  SCREEN_OFF_MS / 1000, halBatteryMv(), _onUsb ? "yes" : "no");
  }

  // Once every 5s: what the power and tilt heuristics are seeing.
  static uint32_t lastDiag = 0;
  if (now - lastDiag > 5000) {
    lastDiag = now;
    float ax, ay, az; halAccel(&ax, &ay, &az);
    Serial.printf("diag: vbat %dmV usb=%d accel %.2f %.2f %.2f screenOff=%d napping=%d\n",
                  halBatteryMv(), _onUsb, ax, ay, az, screenOff, napping);
  }

  delay(screenOff ? 100 : 16);
}
