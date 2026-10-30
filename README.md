# Door Access Controller

DIY ESP32-S3 door access controller running ESPHome. A single ESP drives everything: touchscreen PIN pad and doorbell, Wiegand card reader, face/palm scanner, presence radar, and (planned) lock relay and audio.

- **Device name:** `access-panel-front`
- **Board:** ESP32-S3 N16R8 devkit (YD-style clone)
- **Firmware:** ESPHome (ESP-IDF framework) with a custom `access_control` external component (FP008 UART driver, profile storage, on-device web page for managing users, cards, PINs, faces and palms)
- **Auth methods:** touchscreen PIN, RFID/NFC card (125 kHz + 13.56 MHz), face + palm vein. Fingerprint was dropped.
- **Home Assistant:** access events are sent as `esphome.access_panel_access`; doorbell and mail are event entities.

> Status legend: **Working** = confirmed on the bench. **Planned** = designed but not installed/verified yet.

## Pinout (ESP32-S3)

| Function | Signal | GPIO | Status |
|---|---|---|---|
| Display ST7796 (SPI) | CS | 10 | Working |
| | DC / RS | 9 | Working |
| | MOSI / SDI | 11 | Working |
| | SCK | 12 | Working |
| | LED (backlight) | 7 | Working |
| | MISO | not connected | |
| | RESET | tied to 3.3 V | |
| Touch FT6336 (I2C) | SDA | 5 | Working |
| | SCL | 6 | Working |
| | INT | 4 | Working |
| | CTP_RST | tied to 3.3 V | |
| Wiegand reader (via level shifter) | D0 (green) | 38 | Working |
| | D1 (white) | 39 | Working |
| | LED control (yellow, optional, via shifter ch3) | 21 | Proposed, untested |
| FP008 face/palm (UART 115200) | TXD (pin 3) to ESP RX | 18 | Working |
| | RXD (pin 4) from ESP TX | 17 | Working |
| | IRQ / presence (pin 5) | 16 | Working |
| HLK-LD2450 mmWave radar (UART 256000) | Radar TX to ESP RX | 8 | Working |
| | Radar RX from ESP TX | 2 | Working |
| Ambient light sensor | ADC (10k to GND) | 1 | Not working yet: part bought was an IR emitter; replace with TEPT4400 |
| MAX98357A I2S amp | LRC | 13 | Planned |
| | BCLK | 14 | Planned |
| | DIN | 15 | Planned |
| Reserved for PDM mic | | 40 / 41 / 42 | Reserved |

## Connections

### Display (ST7796 3.5" SPI)
VCC to 3.3 V, GND to GND, CS to GPIO10, DC/RS to GPIO9, MOSI/SDI to GPIO11, SCK to GPIO12, LED to GPIO7, RESET to 3.3 V, MISO unconnected. SD card pins unconnected.

### Touch (FT6336, I2C)
SDA to GPIO5, SCL to GPIO6, INT to GPIO4, CTP_RST to 3.3 V.

### Wiegand card reader (dual-frequency 125 kHz / 13.56 MHz)
Runs from 12 V; the data lines are 5 V logic so they go through a 4-channel level shifter (HV 5 V, LV 3.3 V, GND on both sides).

| Reader wire | Goes to |
|---|---|
| Red | +12 V |
| Black | GND (shared with ESP) |
| Green (D0) | Shifter ch1 HV, LV to GPIO38 |
| White (D1) | Shifter ch2 HV, LV to GPIO39 |

Channels 3 and 4 of the shifter are spare. Some readers expose a yellow LED-control wire (pull to GND for green); if used, route it through ch3 to GPIO21. Brown, where present, is the WG26/34 format select. Leave it unconnected for 26-bit.

### FP008 face/palm module (UART 115200)
| FP008 pin | Connection |
|---|---|
| 1 VCC | 5 V |
| 2 GND | GND |
| 3 TXD | GPIO18 |
| 4 RXD | GPIO17 |
| 5 IRQ | GPIO16 |

Check the module's TXD voltage before connecting; use the level shifter if it is 5 V.

### HLK-LD2450 radar
5 V, GND, radar TX to GPIO8, radar RX to GPIO2, 256000 baud, no level shifter. Drives presence-based screen dimming (5 % when nobody is near).

### Ambient light sensor
Phototransistor between 3.3 V and GPIO1, with 10 k from GPIO1 to GND. Use a visible-light part in a 3 mm LED package (Vishay TEPT4400). 940 nm IR parts read badly under LED lighting. The first part bought was an IR emitter and does not sense light.

### MAX98357A amp and speaker (planned)
VIN 5 V, GND, LRC GPIO13, BCLK GPIO14, DIN GPIO15, GAIN and SD unconnected. Speaker goes on the amp's + and - terminals only (never to GND). Add 100 uF + 100 nF across VIN/GND at the amp.
Note: on YD-ESP32-S3 clone boards the 5 V pin may need the IN-OUT solder pad bridged.

## Parts list

**Outside panel**
- ESP32-S3 N16R8 devkit
- 3.5" ST7796 IPS capacitive touch display (FT6336 touch)
- FP008 face and palm vein module + MX1.25 8-pin pigtail
- Dual-frequency Wiegand card reader (125 kHz + 13.56 MHz, with doorbell)
- HLK-LD2450 mmWave radar
- Vishay TEPT4400 3 mm ambient light phototransistor (replacement for the IR emitter bought by mistake)
- BSS138 4-channel level shifter
- Resistors (1 k, 10 k, 20 k) and capacitors (100 nF, 100 uF, 220-470 uF for the 5 V rail)

**Lock and door**
- 5 V single-channel optocoupled relay module
- 12 V fail-secure electric strike, with 1N4007 flyback diode
- Normally closed reed switch (MC-38 or recessed)
- No-touch exit button
- PC817 optocoupler (only if the exit button outputs 12 V instead of dry contacts)

**Audio / intercom (planned)**
- MAX98357A I2S amp, 4 ohm 2-3 W waterproof speaker
- Microphone (INMP441 I2S or PDM on GPIO40-42), acoustic vent membrane

**Power and build**
- 12 V supply (2 A or more) and 12 V to 5 V buck converter (2 A or more)
- Perfboard or pluggable screw terminals, hook-up wire
- 3D printed enclosure
- USB-TTL adapters (CP2102, CH340G, PL2303) for testing

## Design notes
- The outside ESP drives the lock relay directly (no separate inside controller).
- The doorbell is a button on the touchscreen, not a physical one.
- Use a fail-secure strike on a battery-backed supply, and keep a local fallback credential so the door still works if Home Assistant or the network is down.
- Wi-Fi credentials are not stored in this repo; the ESPHome config uses `!secret iot_wifi_ssid` and `!secret iot_wifi_password`.

## Firmware
- `access-panel-front.yaml`: full ESPHome config (screen UI, PIN pad, doorbell/mail events, radar, auto-brightness, speaker, FP008 + Wiegand).
- Still to add: `components/access_control/` (custom component: `__init__.py`, `access_control.cpp`, `access_control.h`, `web_page.h`) and `images/aurora.png` (background image referenced by the YAML). The YAML will not compile without them.
- Lock relay, reed switch and exit button are not in this YAML yet.
