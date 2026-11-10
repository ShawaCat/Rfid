/* ESP32 DevKit V1 + ST25R3916 (SPI) + SSD1315 128x64 (I2C).
 * Read-only MIFARE Classic 1K dump in RAM; listen mode advertises the saved
 * ISO14443-A UID only. Crypto1 software is in ClassicCrypto1.h but the
 * hardware transport for encrypted parity is NOT implemented yet.
 */
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "ClassicCrypto1.h"
#include <rfal_mf1.h>
#include <rfal_nfc.h>
#include <rfal_rfst25r3916.h>
#include <st_errno.h>
#include <string.h>

namespace {
constexpr int PIN_SCK = 18, PIN_MISO = 19, PIN_MOSI = 23;
constexpr int PIN_CS = 5, PIN_IRQ = 4;
constexpr int PIN_SDA = 21, PIN_SCL = 22;
// Default for four independent buttons. Change to match YOUR module pinout.
constexpr int PIN_READ = 32, PIN_LISTEN = 33;
constexpr int PIN_PREV = 25, PIN_NEXT = 26; // active low, internal pullups
constexpr uint8_t OLED_ADDR = 0x3C;
constexpr uint8_t SECTORS = 16, BLOCKS = 64, BLOCK_SIZE = 16;
constexpr uint8_t DEFAULT_KEY[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
// Replace individual sector keys as needed. Keys are NEVER recovered from a
// Classic sector trailer by reading it.
uint8_t keyA[SECTORS][6];

struct RamCard {
  uint8_t blocks[BLOCKS][BLOCK_SIZE];
  bool valid[BLOCKS];
  uint8_t uid[4];
  uint8_t atqa[2];
  uint8_t sak;
  bool hasUid;
};
RamCard card = {};
classic::Session cryptoSession; // reset on every mode change; no RF transport yet

SPIClass spi(VSPI);
RfalRfST25R3916Class reader(&spi, PIN_CS, PIN_IRQ);
RfalNfcClass nfc(&reader);
RfalMf1Class mf1(&reader);
U8G2_SSD1315_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
bool oledOK = false, nfcOK = false;
rfalNfcDiscoverParam discover = {};
// Buffers passed to RFAL must outlive rfalListenStart(). Length is in bits.
uint8_t listenRx[32] = {};
uint16_t listenBits = 0;

enum class Mode { Idle, Read, ListenUid };
Mode mode = Mode::Idle;
uint32_t modeGeneration = 0; // cancel an in-progress dump on any button press

struct Button {
  int pin;
  bool raw = false, stable = false;
  uint32_t changedAt = 0;
  bool pressed() {
    bool now = (digitalRead(pin) == LOW);
    if (now != raw) { raw = now; changedAt = millis(); }
    if (now != stable && millis() - changedAt >= 35) {
      stable = now;
      return stable; // one event per debounced press
    }
    return false;
  }
};
Button readButton{PIN_READ}, listenButton{PIN_LISTEN};
Button prevButton{PIN_PREV}, nextButton{PIN_NEXT};
uint8_t selectedBlock = 0;

void screen(const char *a, const char *b = "", const char *c = "", const char *d = "") {
  Serial.printf("%s | %s | %s | %s\n", a, b, c, d);
  if (!oledOK) return;
  display.clearBuffer();
  display.drawStr(0, 10, a);
  display.drawStr(0, 26, b);
  display.drawStr(0, 39, c);
  display.drawStr(0, 52, d);
  display.sendBuffer();
}
void uidText(char *out, size_t n) {
  snprintf(out, n, "%02X:%02X:%02X:%02X", card.uid[0], card.uid[1], card.uid[2], card.uid[3]);
}
void showBlock() {
  if (!card.hasUid) return;
  char title[24], first[17], second[17];
  snprintf(title, sizeof(title), "Block %02u/63 %s", selectedBlock,
           card.valid[selectedBlock] ? "OK" : "unread");
  if (card.valid[selectedBlock]) {
    for (uint8_t i = 0; i < 8; ++i) {
      snprintf(first + 2 * i, 3, "%02X", card.blocks[selectedBlock][i]);
      snprintf(second + 2 * i, 3, "%02X", card.blocks[selectedBlock][i + 8]);
    }
  } else {
    strcpy(first, "No block data");
    second[0] = '\0';
  }
  screen(title, first, second, "25=prev  26=next");
}
void stopPoll() {
  if (nfc.rfalNfcGetState() > RFAL_NFC_STATE_IDLE) nfc.rfalNfcDeactivate(false);
}
void startRead() {
  ++modeGeneration;
  cryptoSession.reset();
  if (!nfcOK) { screen("ST25R3916 error", "Check SPI + IRQ"); return; }
  if (mode == Mode::ListenUid) reader.rfalListenStop();
  stopPoll();
  mode = Mode::Read;
  const ReturnCode rc = nfc.rfalNfcDiscover(&discover);
  if (rc != ERR_NONE) {
    mode = Mode::Idle;
    screen("Poll failed", "See Serial");
    Serial.printf("rfalNfcDiscover: %d\n", (int)rc);
  } else {
    screen("READ Classic 1K", "Present 4-byte UID", "Default Key A: FF*6", "32=read 33=listen");
  }
}
void startListen() {
  ++modeGeneration;
  cryptoSession.reset();
  if (!nfcOK) { screen("ST25R3916 error", "Check SPI + IRQ"); return; }
  if (mode == Mode::ListenUid) reader.rfalListenStop();
  stopPoll();
  mode = Mode::Idle;
  if (!card.hasUid) { screen("NO CARD IN RAM", "Read a card first"); return; }

  rfalLmConfPA conf = {};
  conf.nfcidLen = RFAL_LM_NFCID_LEN_04;
  memcpy(conf.nfcid, card.uid, 4);
  memcpy(conf.SENS_RES, card.atqa, 2);
  conf.SEL_RES = card.sak;
  // The chip handles anticollision; the firmware has NO Crypto1 PICC handler.
  const ReturnCode rc = reader.rfalListenStart(RFAL_LM_MASK_NFCA, &conf, nullptr, nullptr,
                                                listenRx, sizeof(listenRx) * 8U, &listenBits);
  if (rc != ERR_NONE) {
    screen("Listen failed", "See Serial");
    Serial.printf("rfalListenStart: %d\n", (int)rc);
    return;
  }
  mode = Mode::ListenUid;
  char uid[20]; uidText(uid, sizeof(uid));
  screen("UID ONLY - NOT MF1", uid, "No auth / no blocks", "READ to exit");
}

// Returns false if a mode button was pressed during a blocking dump.
bool checkButtons() {
  bool r = readButton.pressed(), l = listenButton.pressed();
  bool prev = prevButton.pressed(), next = nextButton.pressed();
  if (l) { startListen(); return false; }
  if (r) { startRead(); return false; }
  // Browsing is deliberately disabled during NFC operations (no OLED I2C
  // traffic in the response path). It is only available after a read.
  if (mode == Mode::Idle && card.hasUid && (prev || next)) {
    selectedBlock = (selectedBlock + (next ? 1U : 63U)) % BLOCKS;
    showBlock();
  }
  return true;
}

bool acquire(const uint8_t expectedUid[4], rfalNfcDevice **out, uint32_t generation) {
  stopPoll();
  if (nfc.rfalNfcDiscover(&discover) != ERR_NONE) return false;
  const uint32_t start = millis();
  while (millis() - start < 2000) {
    if (!checkButtons() || modeGeneration != generation) return false;
    nfc.rfalNfcWorker();
    if (nfc.rfalNfcGetState() == RFAL_NFC_STATE_ACTIVATED) {
      rfalNfcDevice *dev = nullptr;
      if (nfc.rfalNfcGetActiveDevice(&dev) == ERR_NONE && dev && dev->nfcidLen == 4 &&
          memcmp(dev->nfcid, expectedUid, 4) == 0) { *out = dev; return true; }
      return false; // different card: never combine dumps
    }
    delay(1);
  }
  return false;
}

void dumpCard(rfalNfcDevice *first) {
  if (!first || !RfalMf1Class::isNfcaDevice(first) || first->nfcidLen != 4 ||
      first->dev.nfca.selRes.sak != 0x08) {
    screen("Not Classic 1K", "Need 4-byte UID", "and SAK 08");
    stopPoll(); mode = Mode::Idle; return;
  }
  selectedBlock = 0;
  // Invalidate the old snapshot before any new attempt. A partial dump is
  // visible on Serial but cannot silently masquerade as a complete dump.
  memset(&card, 0, sizeof(card));
  memcpy(card.uid, first->nfcid, 4);
  card.atqa[0] = first->dev.nfca.sensRes.anticollisionInfo;
  card.atqa[1] = first->dev.nfca.sensRes.platformInfo;
  card.sak = first->dev.nfca.selRes.sak;
  card.hasUid = true;
  uint8_t expected[4]; memcpy(expected, card.uid, 4);
  char uid[20]; uidText(uid, sizeof(uid));
  Serial.printf("Classic 1K UID=%s ATQA=%02X %02X SAK=%02X\n",
                uid, card.atqa[0], card.atqa[1], card.sak);
  const uint32_t generation = modeGeneration;
  unsigned good = 0;
  for (uint8_t sector = 0; sector < SECTORS; ++sector) {
    if (!checkButtons() || modeGeneration != generation) return;
    rfalNfcDevice *dev = nullptr;
    // MF1 sessions may be invalidated by an error/end of sector. Always
    // reactivate before authenticating the next sector.
    if (!acquire(expected, &dev, generation)) {
      if (modeGeneration != generation) return;
      Serial.printf("Sector %u: reacquire failed\n", sector);
      break;
    }
    rfalMf1CryptoState crypto = {};
    const uint8_t base = sector * 4;
    ReturnCode rc = mf1.authenticate(&crypto, dev, base, keyA[sector], RFAL_MF1_AUTH_KEY_A);
    if (rc != ERR_NONE) {
      Serial.printf("Sector %u: auth failed (%d)\n", sector, (int)rc);
      continue;
    }
    for (uint8_t offset = 0; offset < 4; ++offset) {
      if (!checkButtons() || modeGeneration != generation) return;
      const uint8_t block = base + offset;
      uint8_t data[BLOCK_SIZE] = {};
      rc = mf1.readBlock(&crypto, block, data);
      if (rc != ERR_NONE) {
        Serial.printf("Block %u: read failed (%d)\n", block, (int)rc);
        break;
      }
      memcpy(card.blocks[block], data, BLOCK_SIZE);
      card.valid[block] = true;
      ++good;
      if (offset == 3) {
        // Trailer reads do not reveal Key A (nor necessarily Key B). Mask
        // both key regions: they are NOT a cloneable sector trailer.
        memset(card.blocks[block], 0, 6);
        memset(card.blocks[block] + 10, 0, 6);
      }
      Serial.printf("%02u%s: ", block, offset == 3 ? " trailer (keys masked)" : "");
      for (uint8_t v : card.blocks[block]) Serial.printf("%02X", v);
      Serial.println();
    }
    char progress[24]; snprintf(progress, sizeof(progress), "Sector %u/16", sector + 1);
    char count[24]; snprintf(count, sizeof(count), "Blocks %u/64", good);
    screen("READ Classic 1K", uid, progress, count);
  }
  stopPoll(); mode = Mode::Idle;
  char count[24]; snprintf(count, sizeof(count), "%u/64 blocks in RAM", good);
  screen(good == BLOCKS ? "READ finished" : "PARTIAL READ", uid, count,
         "25/26 browse blocks");
  Serial.println("RAM is volatile. Trailer keys are not recovered. UID-only listen is NOT Classic emulation.");
}
} // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_READ, INPUT_PULLUP);
  pinMode(PIN_LISTEN, INPUT_PULLUP);
  pinMode(PIN_PREV, INPUT_PULLUP);
  pinMode(PIN_NEXT, INPUT_PULLUP);
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  for (auto &key : keyA) memcpy(key, DEFAULT_KEY, sizeof(DEFAULT_KEY));
  Wire.begin(PIN_SDA, PIN_SCL);
  // U8g2 expects an 8-bit I2C address, unlike Wire's 7-bit address.
  display.setI2CAddress(OLED_ADDR << 1);
  Wire.beginTransmission(OLED_ADDR);
  oledOK = (Wire.endTransmission() == 0);
  if (oledOK) {
    display.begin();
    display.setFont(u8g2_font_6x10_tf);
  } else {
    Serial.printf("SSD1315 not found at I2C 0x%02X\n", OLED_ADDR);
  }
  screen("Initializing NFC...");
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  nfcOK = (nfc.rfalNfcInitialize() == ERR_NONE);
  if (!nfcOK) { screen("ST25R3916 error", "Check wiring / 3.3V"); return; }
  discover.compMode = RFAL_COMPLIANCE_MODE_NFC;
  discover.devLimit = 1;
  discover.nfcfBR = RFAL_BR_212;
  discover.ap2pBR = RFAL_BR_424;
  discover.totalDuration = 1000;
  discover.techs2Find = RFAL_NFC_POLL_TECH_A;
  startRead();
}

void loop() {
  if (!checkButtons()) return;
  if (mode == Mode::Read) {
    nfc.rfalNfcWorker();
    if (nfc.rfalNfcGetState() == RFAL_NFC_STATE_ACTIVATED) {
      rfalNfcDevice *dev = nullptr;
      if (nfc.rfalNfcGetActiveDevice(&dev) == ERR_NONE) dumpCard(dev);
    }
  } else if (mode == Mode::ListenUid) {
    reader.rfalWorker();
    bool received = false;
    const rfalLmState state = reader.rfalListenGetState(&received, nullptr);
    if (received) {
      // Do not respond to Classic AUTH/READ; this is UID-only discovery.
      Serial.printf("Listen request: %u bits (state %u), unsupported\n", listenBits, (unsigned)state);
      reader.rfalListenSetState(RFAL_LM_STATE_IDLE);
    }
  }
  delay(1);
}
