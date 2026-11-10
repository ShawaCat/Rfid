# ESP32 DevKit V1 + ST25R3916 + MIFARE Classic 1K

Arduino sketch: [`firmware/Classic1kRam/Classic1kRam.ino`](firmware/Classic1kRam/Classic1kRam.ino).

**Important:** Reading the card into RAM is implemented. The **emulation button only starts ISO14443-A listen/anticollision with the saved UID, ATQA and SAK**. It is **not** a MIFARE Classic 1K emulator: there is no PICC-side Crypto1 authentication, encrypted READ/WRITE, access-bit handling, or block responses. A reader may see the UID but cannot use it as a Classic card. The ST25R3916's RFAL listen mode alone does not provide those features. Do not rely on this for access-control cloning. Only test cards you own or are authorized to use.

## Connections (3.3 V logic; common GND)

| ST25R3916 SPI module | ESP32 GPIO |
|---|---:|
| SCK | 18 |
| MISO/SDO | 19 |
| MOSI/SDI | 23 |
| CS/SS | 5 |
| IRQ | 4 |
| VCC/GND | 3V3/GND |

Select **SPI mode** on the ST25R3916 module (check its jumpers). SSD1306 128×64 I2C OLED: SDA→21, SCL→22, VCC→3V3, GND→GND; address `0x3C` (change `OLED_ADDR` for `0x3D`). Connect one normally-open button from GPIO32 to GND (READ), the other from GPIO33 to GND (UID-only listen). Both use `INPUT_PULLUP` and are active low. No 5 V signals on ESP32 pins.

## Build / use

Use the Arduino IDE with **ESP32 Dev Module** (ESP32 Arduino core) and install:

- [ELECHOUSE ST25R3916 for ESP32](https://github.com/wilson-elechouse/ST25R3916) — install the repository's `ST25R3916_ELECHOUSE` folder as an Arduino library;
- from the **same repository revision**, install its `NFC-RFAL` folder as another library (the sketch uses its `rfal_mf1` reader implementation);
- Adafruit GFX Library and Adafruit SSD1306 (including their dependency Adafruit BusIO).

Reference revision of the two ELECHOUSE libraries: `76d85da7b7e9cb89042eb0e31e070fec91030abb`. Open the sketch, choose the ESP32 board/port and upload. Serial Monitor: **115200 baud**. The OLED uses the built-in ASCII font, so its labels are English.

On boot, or after pressing READ (GPIO32), present a **4-byte UID, SAK 0x08** Classic 1K. The firmware authenticates each of 16 sectors with Key A `FF FF FF FF FF FF` by default; configure `keyA[sector]` in the sketch if your own card uses different keys. Every sector is reacquired and UID-checked before authentication; failed sectors are reported and left invalid. Successful 16-byte blocks and progress appear on Serial/OLED. All 64 blocks occupy 1024 bytes in RAM and are cleared before a new read. Sector-trailer key bytes are *masked* because keys cannot be read back from the card; the RAM image is not a complete clone even after 64 successful reads. No data is written to the physical card.

Press GPIO33 to start **UID-only** listen mode using the most recent read UID. The display explicitly says `UID ONLY - NOT MF1`; it does **not** send memory contents to a reader. Press GPIO32 to leave listen mode and read again. RAM contents disappear on reset/power-off. This project has not been verified on physical hardware; verify SPI bring-up and card detection with the upstream examples first.

## Crypto1 groundwork (not an RF emulator yet)

`ClassicCrypto1.h` now contains a transport-independent, **Key-A-only, READ-only** Crypto1 prototype: 4-byte UID/nonce challenge, reader response and encrypted-parity verification, encrypted `At`, CRC_A, encrypted block responses, and 9-bit byte/parity packing. The sketch resets this state when switching modes. **It is not connected to the NFC receive/transmit path**: RFAL's current listen worker strips CRC and rejects encrypted-parity frames, and the ST25R3916 FIFO cannot be treated as a transparent waveform generator just by packing 9-bit bytes. Until a dedicated raw RX path and correctly timed ESP32 transparent-mode TX (GPIO/RMT) are implemented and tested on real hardware, GPIO33 remains **UID-only**; AUTH and READ will not succeed over RF. Key B, WRITE, access rules and nested AUTH are not implemented. Avoid assuming a successful host test proves radio compatibility.

Host-side crypto/framing regression test (no Arduino dependencies):

```sh
g++ -std=c++11 -Wall -Wextra -Werror tests/crypto1.cpp -o /tmp/crypto1_test && /tmp/crypto1_test
```
