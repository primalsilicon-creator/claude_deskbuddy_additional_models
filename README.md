# claude-desktop-buddy for the Waveshare ESP32-S3-Touch-LCD-3.49

This fork runs the buddy firmware on the **Waveshare ESP32-S3-Touch-LCD-3.49 /
3.49B, V2 board** (PCB silkscreen `Rev1.1`, or a `V2` sticker on the case)
instead of the M5StickC Plus. The Bluetooth protocol, pets, stats and GIF
characters are unchanged. What changed:

- **Hardware layer.** All board code is in `src/hal.h` / `src/hal_waveshare349.cpp`:
  the AXS15231B 172×640 QSPI panel, TCA9554 expander (LCD reset, backlight
  enable, power latch), QMI8658 accelerometer, PCF85063 clock, AXS15231B
  touch, battery ADC, and the BOOT / PWR buttons. The M5StickCPlus library
  is gone. V1 boards use different backlight and reset pins and will stay
  dark with this firmware.
- **Landscape layout.** The screen is 640×172. The pet lives in the left
  200px, and the transcript, approval prompt, info pages, pet stats, menus
  and charging clock fill the panel on the right.
- **Controls.** On the 3.49B case, BOOT is the button with the
  backlight/sun icon, PWR is the middle power button, and the third
  button is RESET (it just reboots the board).

  |                  | Normal              | Approval          |
  | ---------------- | ------------------- | ----------------- |
  | **PWR** (A, middle)          | next screen         | **approve**       |
  | **BOOT** (B, backlight icon) | scroll / next page  | **deny**          |
  | **Hold PWR**                 | menu                | menu              |
  | **Hold BOOT 3s**             | power off           | power off         |
  | **Tap pet**                  | same as PWR         |                   |
  | **Tap panel**                | same as BOOT        | tap Approve / Deny buttons |

  This matches the M5StickC Plus: the big middle button is A.

  On USB power, "power off" puts the board to sleep until BOOT is pressed.
- **No LED.** "Attention" flashes an orange border around the screen
  instead (settings → flash).
- **Sound** plays the original beeps through the ES8311 codec and the
  onboard speaker (settings → sound).
- **Settings → flip** turns the picture 180° if it comes up upside down
  in your case.
- **Transcript history.** The desktop sends one merged list of recent lines
  with no session ids, and drops older sessions from it. Instead of
  replacing the screen with each list, the board keeps a rolling history of
  the last 40 lines: lines the desktop still lists are bright, older ones are
  dimmed. A header shows total / running / waiting sessions. BOOT (B)
  scrolls back.
- **Battery.** The board only reports battery voltage. It has no current
  or USB-power sensing, so "on USB" is a guess (battery above 4.15V).

### Flashing this fork

```bash
pio run -e waveshare-349-v2 -t upload
```

If the port doesn't show up, hold **BOOT**, tap **RESET** on the back, then
release BOOT and upload again. `pio device monitor` shows the serial log,
including touch coordinates and whether the IMU was found.

### Things to check on first boot

- The picture is the right way up (otherwise **settings → flip**).
- Tapping the pet side counts as PWR (A), the panel side as BOOT (B). If taps land
  on the wrong side, check the `touch:` lines in the serial log.
- Lying the board **face down** dims it (nap). The firmware treats however
  the board is lying at power-on as face up, so boot it face up or standing.

## LCDwiki ESP32-32E (2.8" and 3.2")

The same firmware also builds for the LCDwiki ESP32-32E displays. Board
code is in `src/hal_esp32_32e.cpp`. Both are a classic ESP32 with 4MB
flash and no PSRAM, a 240×320 panel and XPT2046 resistive touch. Pick the
env for your board, because the screen chip and pins differ:

| Board | Env | Panel | Notes |
| ----- | --- | ----- | ----- |
| [2.8inch](https://www.lcdwiki.com/2.8inch_ESP32-32E_Display) (E32R28T, also sold as "ZLX") | `esp32-32e-28` | ILI9341V | backlight IO21, touch on its own pins |
| [3.2inch](https://www.lcdwiki.com/3.2inch_ESP32-32E_Display) (E32R32P) | `esp32-32e` | ST7789 IPS | backlight IO27, touch shares the LCD bus |

```bash
pio run -e esp32-32e-28 -t upload     # or -e esp32-32e for the 3.2"
# optional, the bufo GIF character over USB (or send it from the desktop):
python3 tools/flash_character.py characters/bufo esp32-32e-28
```

- **Portrait layout.** The pet lives in the top 130px and the transcript,
  prompts, pages and menus fill the panel below. With no PSRAM the screen is
  two sprites: the pet in full colour and the panel in 8-bit colour, so UI
  colours are slightly coarser than on the Waveshare.
- **Touch-only.** The case this board ships in covers every button, so
  everything is done by touch:

  |                          | Normal                       | Approval        |
  | ------------------------ | ---------------------------- | --------------- |
  | **Tap pet**              | next screen                  |                 |
  | **Tap panel**            | next page (transcript: top half scrolls back, bottom half forward) | tap **Approve** / **Deny** |
  | **Hold anywhere**        | menu (tap a button to pick it, tap the pet or hold to close) | |

  BOOT still works as the A button if you can reach it.
- **Touch calibration.** On first boot it asks you to press three
  crosshairs (it gives up after 20s per point and uses rough defaults). Redo it any time from **settings → touch cal**.
- **No IMU,** so no shake or face-down nap. Instead, a screen-off of a
  minute or more on battery counts as a nap.
- **No clock chip.** The time comes from the desktop and is kept in the
  ESP32's own clock until the next reboot.
- **Turn off** (menu) puts the board in deep sleep. Touch the screen to wake
  it.
- **Attention** flashes the screen border and the red RGB LED.
- **Sound** comes out of the 1.25mm speaker connector (FM8002E amp) if a
  small 8Ω speaker is plugged in. It is quiet when nothing is connected.

Things to check on first boot:

- If red and blue are swapped or the colours look inverted, there are
  commented-out flags for that in each env of `platformio.ini`.
- The serial log prints free heap after the sprites and after BLE start. If
  BLE fails to start, that number is the first thing to look at.
- The battery reading assumes a 1/2 divider on IO34. If DEVICE info shows
  a voltage that is off by a constant factor, change `BAT_DIVIDER` in
  `src/hal_esp32_32e.cpp`.

The original README for the M5StickC Plus follows.

---

# claude-desktop-buddy

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions. We've
been impressed by the creativity of the maker community around Claude -
providing a lightweight, opt-in API is our way of making it easier to build
fun little hardware devices that integrate with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet on ESP32 that lives off permission
approvals and interaction with Claude. It sleeps when nothing's happening,
wakes when sessions start, gets visibly impatient when an approval prompt is
waiting, and lets you approve or deny right from the device.

<p align="center">
  <img src="docs/device.jpg" alt="M5StickC Plus running the buddy firmware" width="500">
</p>

## Hardware

The firmware targets ESP32 with the Arduino framework. As written, it
depends on the M5StickCPlus library for its display, IMU, and button
drivers—so you'll need that board, or a fork that swaps those drivers for
your own pin layout.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
pio run -t upload
```

If you're starting from a previously-flashed device, wipe it first:

```bash
pio run -t erase && pio run -t upload
```

Once running, you can also wipe everything from the device itself: **hold A
→ settings → reset → factory reset → tap twice**.

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the stick:

- Make sure it's awake (any button press)
- Check the stick's settings menu → bluetooth is on (the Waveshare build has no such setting; Bluetooth is always on)

## Controls

|                         | Normal               | Pet         | Info        | Approval    |
| ----------------------- | -------------------- | ----------- | ----------- | ----------- |
| **A** (front)           | next screen          | next screen | next screen | **approve** |
| **B** (right)           | scroll transcript    | next page   | next page   | **deny**    |
| **Hold A**              | menu                 | menu        | menu        | menu        |
| **Power** (left, short) | toggle screen off    |             |             |             |
| **Power** (left, ~6s)   | hard power off       |             |             |             |
| **Shake**               | dizzy                |             |             | —           |
| **Face-down**           | nap (energy refills) |             |             |             |

The screen auto-powers-off after 30s of no interaction (kept on while an
approval prompt is up). Any button press wakes it.

## ASCII pets

Eighteen pets, each with seven animations (sleep, idle, busy, attention,
celebrate, dizzy, heart). Menu → "next pet" cycles them with a counter.
Choice persists to NVS.

## GIF pets

If you want a custom GIF character instead of an ASCII buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window. The
app streams it over BLE and the stick switches to GIF mode live. **Settings
→ delete char** reverts to ASCII mode.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are 96px wide; height up to ~140px stays on a 135×240 portrait screen.
Crop tight to the character — transparent margins waste screen and shrink
the sprite. `tools/prep_character.py` handles the resize: feed it source
GIFs at any sizes and it produces a 96px-wide set where the character is the
same scale in every state.

The whole folder must fit under 1.8MB —
`gifsicle --lossy=80 -O3 --colors 64` typically cuts 40–60%.

See `characters/bufo/` for a working example.

If you're iterating on a character and would rather skip the BLE round-trip,
`tools/flash_character.py characters/bufo [pio-env]` stages it into `data/` and runs
`pio run -e <env> -t uploadfs` directly over USB (env defaults to `waveshare-349-v2`).

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, **LED blinks**       |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | you shook the stick         | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

## Project layout

```
src/
  main.cpp       — loop, state machine, UI screens
  buddy.cpp      — ASCII species dispatch + render helpers
  buddies/       — one file per species, seven anim functions each
  ble_bridge.cpp — Nordic UART service, line-buffered TX/RX
  character.cpp  — GIF decode + render
  data.h         — wire protocol, JSON parse
  xfer.h         — folder push receiver
  stats.h        — NVS-backed stats, settings, owner, species choice
characters/      — example GIF character packs
tools/           — generators and converters
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.
