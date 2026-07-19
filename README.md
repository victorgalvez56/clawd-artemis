# Clawd Artemis 🦀⌚

Custom firmware that turns the **CircuitMess / Geek Club Artemis** smartwatch (ESP32-S3) into a
**Clawd companion watch**: animated Clawd faces, a proper watchface, wrist notifications, and a
live **Claude Code usage dashboard** fed from your Mac over **USB serial or Bluetooth LE** — styled
like the Claude model-picker menu.

Ported from [clawd-mochi](https://github.com/yousifamanuel/clawd-mochi) (the ESP32-C3 desk crab)
and adapted to real watch hardware: buttons, battery, buzzer, RTC, and BLE.

## Features

- **Watchface** — big clock (hardware BM8563 RTC + one-shot NTP sync), Spanish date, battery %,
  status icons (Bluetooth connected · charging bolt · unread-notification badge), and tiny
  blinking Clawd eyes. Plugging in the charger wakes the screen.
- **Claude usage view** — four limit rows (context, 5h window, weekly) pushed by a companion
  Mac process; menu-style UI with rounded badges and a highlight on the row closest to its limit.
  Works docked (USB) or wireless (BLE Nordic UART, advertises as `Clawd Artemis`).
- **Notifications** — press **DOWN** from the watchface. Any process can push one:
  `curl 'localhost:4848/notif?t=build+done'` → the watch chirps, wakes, and shows a badge.
  Automatic alerts when a Claude limit crosses 70% / 90% or the 5h window resets.
- **Clawd faces** — normal eyes, squish eyes (auto-blink + mood toggle), aquarium with fish and
  a little crab, magic 8-ball in Spanish.
- **Settings menu** (like the stock firmware) — brightness, sound on/off, screen-off timeout,
  screen rotation, LEDs on/off. Persisted to NVS flash, so they survive reboots and reflashes.
- Buzzer chirps, quadratic backlight dimming with idle sleep, real power-off via the power latch,
  battery reading with the board's Vref switch handled correctly.

## Controls

| Button | Short press | Long press |
|---|---|---|
| UP / DOWN | previous / next view | — |
| SELECT | action: roll 8-ball · pop hearts · clear notifications · battery detail | **settings** |
| ALT | back to watchface | **power off** (1.5 s) |

View order: watchface → notifications → eyes → squish → aquarium → 8-ball → usage → settings.
Inside settings: UP/DOWN select a row, SELECT changes the value, ALT exits.

## Hardware

CircuitMess Artemis, **board revision 2** (efuse-reported; rev 0/1 uses a different pin map — see
`Pins.cpp` in the [official firmware](https://github.com/CircuitMess/GC_Artemis-Firmware)).

| Function | GPIO |
|---|---|
| ST7735S 128×128 (SCK/MOSI/DC/RST, no CS) | 36 / 35 / 48 / 34 |
| Backlight (PWM) | 33 |
| Buttons Up/Down/Select/Alt (active-high, pull-down) | 1 / 2 / 3 / 21 |
| Buzzer | 47 |
| Battery ADC (×4 divider) / Vref switch | 5 / 4 (hold LOW) |
| USB detect | 42 |
| I2C (BM8563 RTC @0x51, LSM6DS3TR-C IMU) | SDA 40 / SCL 41 |
| Power latch (LOW = off) | 37 |
| White LEDs ×6 / RGB (active-low) | 46 45 44 43 18 17 / 14 12 13 |

## Build & flash

```bash
# 1. WiFi credentials (never committed)
cp clawd_artemis/secrets.example.h clawd_artemis/secrets.h   # then edit

# 2. Compile + flash (Arduino ESP32 core 3.x, Adafruit GFX + ST7735 libs)
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app" clawd_artemis
arduino-cli upload -p /dev/cu.usbmodemXXX --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app" clawd_artemis
```

## Protocol (USB serial 115200 and BLE NUS — identical)

Line-based `key=value` pairs separated by `;`:

| Key | Meaning |
|---|---|
| `r1..r4=LABEL:detail:pct` | usage rows (auto-opens the usage view) |
| `act=text` | footer ticker |
| `notif=text` | wrist notification (chirp + badge) |
| `cmd=X` | remote view control (`w e s m o t r h`) |
| `time=EPOCH` | set clock + RTC |

Single-char commands: `w e s m o u r h l b p` · pin-probe mode `D A X` + `led=N`
(for mapping hardware mods).

`bridge/clawd-ble.py` (Python + [bleak](https://github.com/hbldh/bleak)) forwards stdin lines to
the watch over BLE — spawned by the companion clawd-mochi server, which computes the Claude usage
rows and pushes to USB and BLE simultaneously.

## Restore stock firmware

Nothing here is destructive: grab the official binary from the
[GC_Artemis-Firmware releases](https://github.com/CircuitMess/GC_Artemis-Firmware/releases) and:

```bash
esptool --port /dev/cu.usbmodemXXX write-flash 0 Artemis-v2.1.1.bin
```

## License

MIT © 2026 Víctor Gálvez
