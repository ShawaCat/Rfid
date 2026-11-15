# ESP32 DevKit V1 + ST25R3916: UID-only serial test

Sketch: [`firmware/Classic1kRam/Classic1kRam.ino`](firmware/Classic1kRam/Classic1kRam.ino). The name of the sketch folder is retained for Arduino IDE compatibility; **this version reads only an ISO14443-A UID**, not MIFARE Classic blocks. It stores the UID, ATQA and SAK in RAM until reset/power-off, and can advertise them using the ST25R3916's NFC-A listen mode. **This is not MIFARE Classic emulation.** It has no Crypto1, authentication, or card memory responses. NFC readers/phones may see the UID or may not detect the incomplete target.

## Wiring

The photographed ELECHOUSE module has a **5V** power input with on-board regulation; use the ESP32's USB-powered **VIN/5V**, never a 5V signal on an ESP32 GPIO. See the [ELECHOUSE ESP32 SPI guide](https://www.elechouse.com/docs/st25r3916/). Common GND; ESP32 SPI GPIO logic is 3.3 V. Verify 5 V at the module's connector with a meter before use.

| Module pin | ESP32 DevKit V1 |
|---|---|
| SCLK/SCL | GPIO18 |
| MISO/SDA | GPIO19 |
| MOSI | GPIO23 |
| CS | GPIO5 |
| IRQ | GPIO4 |
| 5V | USB-powered VIN/5V |
| GND | GND |

The module must be in **SPI** mode (I2C solder jumper open). No buttons or display are needed. The old code's pre-initialization SPI register probe and 1 MHz clock override were removed: initialization now follows the manufacturer's working `ESP32_SPI_scan_14443A_led` example and its default SPI clock.

## Build and test

Arduino IDE: board **ESP32 Dev Module**. Install both folders `NFC-RFAL` and `ST25R3916_ELECHOUSE` from the same revision of [ELECHOUSE's ESP32 library](https://github.com/wilson-elechouse/ST25R3916) (tested library reference: `76d85da7b7e9cb89042eb0e31e070fec91030abb`). No OLED, Crypto1 or other libraries are needed. Open Serial Monitor at **115200 baud**, line ending **Newline** or **Both NL & CR** (not `No line ending`). After boot expect `ESP32 SPI ISO14443A UID-only test v2`, `rfalNfcInitialize: 0` and a command list. If initialization prints `36`, send `INIT` a few seconds after boot to retry without restarting; record both results. If the upstream `ESP32_SPI_scan_14443A_led` example works but `INIT` still fails, enable verbose Arduino compilation in both sketches and compare the selected `ST25R3916_ELECHOUSE` and `NFC-RFAL` library paths and pin lines printed at startup.

| Command | Action |
|---|---|
| `INIT` | Retry NFC initialization after a failed startup (only while idle) |
| `READ` | Wait for one ISO14443-A card; save its UID, ATQA and SAK to RAM and stop polling |
| `UID` | Print saved UID/ATQA/SAK |
| `EMU` | Start **UID-only** NFC-A listen mode; requires a saved 4- or 7-byte UID |
| `STOP` | Stop scanning/listening, retaining RAM snapshot |
| `STATUS` | Show mode and saved UID |
| `HELP` | Show commands |

Suggested sequence: `READ`, present a card, wait for `UID saved to RAM`, then `UID`, `EMU` (check with a second reader), and `STOP`. Only 4- or 7-byte UIDs are supported in listen mode; a 10-byte UID can be read/stored but `EMU` refuses it. Reading/writing MIFARE Classic memory and actual Classic emulation are deliberately out of scope. Send us the startup output and result of `READ` if initialization still fails; redact the UID if it is sensitive.
