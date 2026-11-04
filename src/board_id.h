#pragma once
// Which board this firmware was built for, and which physical unit it is.
//
// BOARD_MODEL comes from the PlatformIO env (build_flags in platformio.ini),
// since each build targets exactly one board and nothing on the board itself
// stores its make or model. The fallbacks below cover builds that don't set
// it (Arduino IDE, or a new env that forgot to).
//
// The short ID is the last two bytes of the factory-burned Bluetooth MAC, as
// four hex digits ("A1B2"). It is the same suffix the BLE name uses
// ("Claude-A1B2"), so the name in the desktop picker, the info screen and the
// serial/status output all match.
#include <Arduino.h>
#include <esp_system.h>
#include "hal.h"

#ifndef BOARD_MODEL
#  if defined(BOARD_WAVESHARE_349)
#    define BOARD_MODEL "Waveshare 3.49B V2"
#  elif defined(BOARD_ESP32_32E)
#    define BOARD_MODEL "ESP32-32E"
#  else
#    define BOARD_MODEL "unknown"
#  endif
#endif

static inline void boardMac(uint8_t mac[6]) {
  esp_read_mac(mac, ESP_MAC_BT);
}

// "A1B2". Always 4 chars + NUL.
static inline const char* boardShortId() {
  static char id[5] = "";
  if (!id[0]) {
    uint8_t mac[6] = {0};
    boardMac(mac);
    snprintf(id, sizeof(id), "%02X%02X", mac[4], mac[5]);
  }
  return id;
}

// "AA:BB:CC:DD:EE:FF"
static inline const char* boardMacStr() {
  static char s[18] = "";
  if (!s[0]) {
    uint8_t m[6] = {0};
    boardMac(m);
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
  }
  return s;
}

// One JSON line for serial: model, chip, short ID, MAC.
static inline void boardPrintInfo(Print& out) {
  out.printf("{\"board\":{\"model\":\"%s\",\"chip\":\"%s\",\"id\":\"%s\",\"mac\":\"%s\"}}\n",
             BOARD_MODEL, ESP.getChipModel(), boardShortId(), boardMacStr());
}
