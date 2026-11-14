# ESP32 DevKit V1 + ST25R3916: Serial-only MIFARE Classic 1K test

Arduino sketch: [`firmware/Classic1kRam/Classic1kRam.ino`](firmware/Classic1kRam/Classic1kRam.ino).

No display or buttons are required. Connect the ESP32 over USB and use the Arduino Serial Monitor (or any serial terminal) at **115200 baud**, with **Newline** or **Both NL & CR** selected. The sketch starts idle and prints a help menu; it does not start scanning until you send `READ`.

**Important:** The sketch reads a card into volatile RAM. `UID` enables ISO14443-A listen/anticollision with the saved UID, ATQA and SAK **only**. It is **not** a MIFARE Classic emulator: no on-air Crypto1 authentication or memory responses. Test only cards you own or are authorized to use.

## Wiring (module power 5 V; ESP32 logic 3.3 V; common GND)

| ELECHOUSE module pin | ESP32 DevKit V1 pin |
|---|---:|
| SCLK/SCL | GPIO18 |
| MISO/SDA | GPIO19 |
| MOSI | GPIO23 |
| CS | GPIO5 |
| IRQ | GPIO4 |
| **5V** | **5V / VIN (USB-powered board)** |
| GND | GND |

**Correction:** Earlier versions of this README incorrectly connected the module's **5V** input to ESP32 **3V3**. The photographed ELECHOUSE module is labeled `5V` and the [manufacturer's ESP32 guide](https://www.elechouse.com/docs/st25r3916/) specifies **ESP32 VIN/5V → module 5V** (on-board power regulation), while the host SPI/IRQ logic remains 3.3 V. Power everything off before rewiring. Never connect 5 V to an ESP32 GPIO; if in doubt, verify the module output levels before connecting MISO and IRQ. Make sure the ESP32 5V/VIN pin actually supplies 5 V when powered by USB.

Select **SPI mode** on the ST25R3916 module (check its jumpers). The driver uses **SPI mode 1** and defaults to **1 MHz during bring-up** (change `NFC_SPI_HZ` only after the chip is recognized; the original driver default was 5 MHz). On the photographed ELECHOUSE module the shared labels `SCLK/SCL` and `MISO/SDA` mean SPI SCLK and SPI MISO when the I2C jumper is open (default SPI mode). Do not apply 5 V signals to ESP32 GPIOs. The SSD1315 and its four buttons can be left disconnected for this test; GPIO21/22/25/26/32/33 are unused.

## Build

Use the Arduino IDE with **ESP32 Dev Module** (ESP32 Arduino core) and install both libraries from the same revision of [ELECHOUSE ST25R3916 for ESP32](https://github.com/wilson-elechouse/ST25R3916):

- `ST25R3916_ELECHOUSE`
- `NFC-RFAL` (includes the `rfal_mf1` reader implementation)

Reference revision: `76d85da7b7e9cb89042eb0e31e070fec91030abb`. No OLED/graphics libraries are required. Upload the sketch and open the serial port at 115200 baud. If boot messages are missing, press the ESP32 reset button while the monitor is open. Type commands in ASCII; they are case-insensitive. Send each command with a line ending; **No line ending** will not trigger it.

| Command | Result |
|---|---|
| `HELP` | List commands |
| `PROBE` | Read/print the ST25R3916 identity register (only while idle) |
| `READ` | Start ISO14443-A scanning and read one Classic 1K into RAM |
| `STOP` | Stop scanning/listening; keep the RAM snapshot |
| `STATUS` | Show NFC mode, UID and number of valid blocks |
| `KEY 0 FFFFFFFFFFFF` | Set sector 0 Key A in RAM (sector 0–15, exactly 12 hex digits) |
| `BLOCK 4` | Print one block (0–63), or `unread` |
| `DUMP` | Print all 64 block statuses/data (only while idle) |
| `UID` | Start **UID-only** listen mode using the last read UID; `READ` or `STOP` exits |

Suggested test: send `STATUS`, then `READ` and present a card with **4-byte UID, SAK 0x08**. Wait for `READ finished` or `PARTIAL READ`. The sketch attempts each of 16 sectors with Key A `FF FF FF FF FF FF`; before `READ`, use `KEY <sector> <12hex>` if your own card requires a different Key A. This command only works while idle, never prints the key, and the key is lost on reset. A terminal may retain the command you typed in its history/log, so do not share an unredacted serial transcript containing secret keys. It reacquires the same UID for each sector. Errors and successful blocks are printed as they occur. `STOP` can cancel a dump at the next command check. Then try `STATUS`, `BLOCK 0`, `DUMP`, and optionally `UID` with a second NFC reader. **A smartphone may not detect the UID-only target and cannot read its Classic memory.**

All 64 blocks take 1024 bytes of RAM; a new valid read clears the old snapshot and power cycling loses it. Failed blocks stay `unread`. Sector-trailer key bytes are *masked* because keys cannot be read back from a card; even a `64/64` result is not a complete clone. The physical card is never written to. Logs contain the card UID and block data: redact them before sharing if sensitive. This project has not been verified on physical hardware. At boot the sketch prints `IC_IDENTITY` (register `0x3F`) and the numeric result of `rfalNfcInitialize`. If initialization fails, send `PROBE` again. `0x00`, `0xFF` or other unexpected identity values strongly suggest a SPI/power/interface problem; `rc=0` from an SPI register read alone does **not** prove the chip is connected. `rfalNfcInitialize: 36` means `ERR_HW_MISMATCH` (the ID register is not a recognized 3916/3916B). If ID consistently reads `0x20` rather than `0x28`/`0x30`, do not disable the identity check; first verify the module is powered from its labeled 5V input, then check chip marking, board jumpers and SPI wiring. The 3916 driver accepts `(id & 0xF8) == 0x28` (ST25R3916) or `0x30` (ST25R3916B). Share the raw identity and init result, exact module model, interface jumper positions and actual wiring for diagnosis. The ESP32 boot lines preceding `ST25R3916 Serial test` are normal.

## Crypto1 groundwork (not an RF emulator yet)

`ClassicCrypto1.h` contains a transport-independent **Key-A-only, READ-only** Crypto1 prototype: challenge/response checking, encrypted `At`, CRC_A, block responses and 9-bit byte/parity packing. The sketch resets the session on mode changes, but **this code is not connected to the NFC receive/transmit path**. RFAL's listen worker drops CRC/invalid encrypted parity, and the ST25R3916 FIFO is not a transparent waveform generator simply because bytes were packed with parity. A raw RX path and a timed ESP32 transparent-mode transmitter would still be needed. Key B, WRITE, access rules and nested AUTH are not implemented.

Host-side crypto/framing test (no Arduino dependencies):

```sh
g++ -std=c++11 -Wall -Wextra -Werror tests/crypto1.cpp -o /tmp/crypto1_test && /tmp/crypto1_test
```
