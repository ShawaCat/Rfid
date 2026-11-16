/* ESP32 DevKit V1: ST25R3916 Classic 1K read-only dump to ESP32 LittleFS,
 * SSD1315 UI, and UID/ATQA/SAK-only NFC-A listen mode.
 * No MIFARE Classic card-memory emulation; no card writes.
 */
#include <Arduino.h>
#include <SPI.h>
#include <LittleFS.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <rfal_nfc.h>
#include <rfal_mf1.h>
#include <rfal_rfst25r3916.h>
#include <st25r3916_config.h>
#include <st_errno.h>
#include <ctype.h>
#include <string.h>

namespace {
// Change these four pins only if your buttons are separately wired to GND.
constexpr int NFC_CS = 5, NFC_IRQ = 4;
// Use unique pin names: Arduino-ESP32 already defines SCK/MISO/MOSI and OK.
constexpr int PIN_SPI_SCK = 18, PIN_SPI_MISO = 19, PIN_SPI_MOSI = 23;
constexpr int SDA_PIN = 21, SCL_PIN = 22;
constexpr int PIN_UP = 32, PIN_DOWN = 33, PIN_OK = 25, PIN_BACK = 26;
constexpr uint8_t OLED_ADDR_PRIMARY = 0x3C, OLED_ADDR_ALTERNATE = 0x3D;
constexpr uint8_t BLOCKS = 64, SECTORS = 16;
constexpr uint8_t DEFAULT_KEY[6] = {255, 255, 255, 255, 255, 255};
constexpr uint8_t MAX_FILES = 40;
uint8_t keyA[SECTORS][6]; // edit locally for your own non-default sectors

SPIClass spi(VSPI);
RfalRfST25R3916Class reader(&spi, NFC_CS, NFC_IRQ);
RfalNfcClass nfc(&reader);
RfalMf1Class mf1(&reader);
rfalNfcDiscoverParam discover = {};
U8G2_SSD1315_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN);
bool oledReady = false, fsReady = false, nfcReady = false;

struct Dump {
  uint8_t uid[10] = {};
  uint8_t uidLen = 0;
  uint8_t atqa[2] = {};
  uint8_t sak = 0;
  uint8_t block[BLOCKS][16] = {};
  bool valid[BLOCKS] = {};
  bool hasUid = false;
};
Dump card;
char selectedFile[13] = {};
char filenames[MAX_FILES][13] = {};
uint8_t fileCount = 0, fileCursor = 0, menuCursor = 0;
uint8_t listenRx[32] = {};
uint16_t listenBits = 0;
enum class Mode { Idle, Read, Listen };
enum class Page { Home, Files, PcFiles, Result };
Mode mode = Mode::Idle;
Page page = Page::Home;
uint32_t generation = 0;
char message[28] = "Ready";
char lineBuf[64] = {};
size_t lineLen = 0;
bool lineOverflow = false;

struct Button {
  uint8_t pin;
  bool raw = false, stable = false;
  uint32_t since = 0;
  bool event() {
    const bool value = digitalRead(pin) == LOW;
    if (value != raw) { raw = value; since = millis(); }
    if (stable != raw && millis() - since >= 35) {
      stable = raw;
      return stable;
    }
    return false;
  }
};
Button up{PIN_UP}, down{PIN_DOWN}, ok{PIN_OK}, back{PIN_BACK};

void notify(const char *text) {
  snprintf(message, sizeof(message), "%s", text);
  Serial.println(text);
}
void draw() {
  if (!oledReady) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  if (page == Page::Home) {
    oled.drawStr(0, 10, "Classic 1K / UID only");
    const char *items[] = {"Read + save", "Emulate file", "PC file manager"};
    for (uint8_t i = 0; i < 3; ++i) {
      oled.drawStr(0, 25 + i * 12, menuCursor == i ? ">" : " ");
      oled.drawStr(9, 25 + i * 12, items[i]);
    }
    oled.drawStr(0, 63, message);
  } else if (page == Page::Files) {
    oled.drawStr(0, 10, "Select UID dump:");
    if (!fileCount) oled.drawStr(0, 27, "No .TXT dumps");
    else {
      char info[25];
      snprintf(info, sizeof(info), "%u/%u  %s", (unsigned)(fileCursor + 1),
               (unsigned)fileCount, filenames[fileCursor]);
      oled.drawStr(0, 27, info);
    }
    oled.drawStr(0, 44, "UP/DOWN choose");
    oled.drawStr(0, 59, "OK=emu BACK=menu");
  } else if (page == Page::PcFiles) {
    oled.drawStr(0, 10, "Serial file manager");
    oled.drawStr(0, 27, "115200 baud / newline");
    oled.drawStr(0, 42, "FILES CAT RENAME");
    oled.drawStr(0, 58, "BACK=menu");
  } else {
    oled.drawStr(0, 10, "Status:");
    oled.drawStr(0, 28, message);
    oled.drawStr(0, 49, mode == Mode::Listen ? "UID ONLY - no blocks" : "BACK=menu");
  }
  oled.sendBuffer();
}
bool oledResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}
void initOled() {
  // A failed I2C probe used to silently disable every draw call.
  oledReady = false;
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  uint8_t address = 0;
  if (oledResponds(OLED_ADDR_PRIMARY)) address = OLED_ADDR_PRIMARY;
  else if (oledResponds(OLED_ADDR_ALTERNATE)) address = OLED_ADDR_ALTERNATE;
  if (!address) {
    Serial.printf("OLED: no response at 0x3C/0x3D (SDA=%d SCL=%d). I2C devices:",
                  SDA_PIN, SCL_PIN);
    bool any = false;
    for (uint8_t candidate = 0x08; candidate <= 0x77; ++candidate) {
      if (oledResponds(candidate)) { Serial.printf(" 0x%02X", candidate); any = true; }
    }
    if (!any) Serial.print(" none");
    Serial.println();
    return;
  }
  oled.setI2CAddress(address << 1); // U8g2 takes an 8-bit address
  oled.begin();
  oledReady = true;
  Serial.printf("OLED: I2C 0x%02X, SSD1315 128x64 init sent (SDA=%d SCL=%d)\n",
                address, SDA_PIN, SCL_PIN);
  draw(); // show a screen even if NFC/flash initialization later fails
}
void stopNfc() {
  ++generation;
  if (mode == Mode::Listen) reader.rfalListenStop();
  if (nfcReady && nfc.rfalNfcGetState() > RFAL_NFC_STATE_IDLE)
    nfc.rfalNfcDeactivate(false);
  mode = Mode::Idle;
}
void printUid() {
  if (!card.hasUid) { Serial.println("No UID in RAM"); return; }
  Serial.print("UID=");
  for (uint8_t i = 0; i < card.uidLen; ++i) Serial.printf("%02X", card.uid[i]);
  Serial.printf(" ATQA=%02X%02X SAK=%02X\n", card.atqa[0], card.atqa[1], card.sak);
}
uint8_t hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 255;
}
bool decodeHex(const char *s, uint8_t *dst, size_t n) {
  if (strlen(s) != n * 2) return false;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t hi = hexDigit(s[i * 2]), lo = hexDigit(s[i * 2 + 1]);
    if (hi > 15 || lo > 15) return false;
    dst[i] = (hi << 4) | lo;
  }
  return true;
}
bool filenameOK(const char *name) {
  const size_t n = strlen(name);
  if (n < 5 || n > 12 || strcmp(name + n - 4, ".TXT")) return false;
  const size_t base = n - 4;
  if (base > 8) return false;
  for (size_t i = 0; i < base; ++i)
    if (!(name[i] >= 'A' && name[i] <= 'Z') &&
        !(name[i] >= '0' && name[i] <= '9') && name[i] != '_' && name[i] != '-') return false;
  return true; // no slashes/traversal; root-only 8.3 file names
}
void pathFor(const char *name, char path[14]) { path[0] = '/'; strcpy(path + 1, name); }
const char *baseName(const char *name) {
  const char *slash = strrchr(name, '/');
  return slash ? slash + 1 : name;
}
void listFiles(bool print = true) {
  fileCount = 0;
  unsigned totalFiles = 0;
  if (!fsReady) { if (print) Serial.println("LittleFS not mounted"); return; }
  File root = LittleFS.open("/");
  if (!root) { if (print) Serial.println("LittleFS root error"); return; }
  File f;
  while ((f = root.openNextFile())) {
    const char *name = baseName(f.name());
    if (!f.isDirectory() && filenameOK(name)) {
      ++totalFiles;
      if (print) Serial.printf("%s  %lu bytes\n", name, (unsigned long)f.size());
      if (fileCount < MAX_FILES) strcpy(filenames[fileCount++], name);
    }
    f.close();
  }
  root.close();
  if (print) Serial.printf("Listed %u file(s); menu shows first %u.\n",
                           totalFiles, (unsigned)MAX_FILES);
  if (fileCursor >= fileCount) fileCursor = 0;
}
bool mountFS() {
  // Never format on mount failure: an interrupted write must not erase dumps.
  fsReady = LittleFS.begin(false);
  if (fsReady) {
    Serial.printf("LittleFS ready: %lu/%lu bytes used\n",
                  (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
  } else Serial.println("LittleFS mount failed; check partition or send FORMAT CONFIRM (ERASES dumps)");
  return fsReady;
}
bool nextName(char name[13]) {
  for (unsigned i = 1; i <= 999999; ++i) {
    snprintf(name, 13, "D%06u.TXT", i);
    char path[14]; pathFor(name, path);
    if (!LittleFS.exists(path)) return true;
  }
  return false;
}
void writeHex(File &f, const uint8_t *bytes, size_t n) {
  for (size_t i = 0; i < n; ++i) f.printf("%02X", bytes[i]);
}
bool saveDump() {
  if (!fsReady || !card.hasUid) { notify("No flash or card"); return false; }
  char name[13];
  if (!nextName(name)) { notify("No free file name"); return false; }
  char path[14]; pathFor(name, path);
  // Publish only a verified, closed file: power loss leaves at most a temp file.
  constexpr const char *temp = "/.DUMP.TMP";
  LittleFS.remove(temp); // discard an incomplete prior attempt
  File f = LittleFS.open(temp, "w");
  if (!f) { notify("Flash write error"); return false; }
  // Compare the exact expected byte count, including Arduino println() CRLF.
  // A mere nonzero file size could misreport a truncated dump as successful.
  size_t expected = strlen("RFID_UID_DUMP_V1") + 2;
  expected += 4 + card.uidLen * 2 + 2;  // UID=...\r\n
  expected += 5 + 4 + 2;                // ATQA=...\r\n
  expected += 4 + 2 + 1;                // SAK=...\n (printf)
  expected += strlen("KEYS=NOT_STORED") + 2;
  f.println("RFID_UID_DUMP_V1");
  f.print("UID="); writeHex(f, card.uid, card.uidLen); f.println();
  f.print("ATQA="); writeHex(f, card.atqa, 2); f.println();
  f.printf("SAK=%02X\n", card.sak);
  f.println("KEYS=NOT_STORED");
  for (uint8_t b = 0; b < BLOCKS; ++b) {
    expected += 4 + (card.valid[b] ? 32 : 2) + 2;
    f.printf("B%02u=", b);
    if (card.valid[b]) writeHex(f, card.block[b], 16);
    else f.print("--");
    f.println();
  }
  f.flush();
  const bool good = f.getWriteError() == 0 && f.size() == expected;
  f.close();
  if (!good) { LittleFS.remove(temp); notify("Incomplete flash write"); return false; }
  if (LittleFS.exists(path) || !LittleFS.rename(temp, path)) {
    LittleFS.remove(temp);
    notify("Flash rename failed");
    return false;
  }
  strcpy(selectedFile, name);
  unsigned validBlocks = 0;
  for (bool valid : card.valid) if (valid) ++validBlocks;
  Serial.printf("Saved %s: %u/64 blocks; %s. Keys masked.\n", name,
                validBlocks, validBlocks == BLOCKS ? "all blocks read" : "PARTIAL dump (-- = unread)");
  notify(name);
  return true;
}
bool readLine(File &f, String &line) {
  if (!f.available()) return false;
  line = f.readStringUntil('\n');
  line.trim();
  return line.length() <= 80;
}
bool loadDump(const char *name) {
  if (!fsReady || !filenameOK(name)) return false;
  char path[14]; pathFor(name, path);
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  if (f.size() > 8192) { f.close(); return false; }
  static Dump candidate;
  candidate = Dump{};
  String line;
  bool ok = readLine(f, line) && line == "RFID_UID_DUMP_V1";
  if (ok) {
    ok = readLine(f, line) && line.startsWith("UID=");
    if (ok) {
      const String hex = line.substring(4);
      candidate.uidLen = hex.length() / 2;
      ok = (candidate.uidLen == 4 || candidate.uidLen == 7 || candidate.uidLen == 10) &&
           decodeHex(hex.c_str(), candidate.uid, candidate.uidLen);
    }
  }
  if (ok) ok = readLine(f, line) && line.startsWith("ATQA=") &&
                decodeHex(line.c_str() + 5, candidate.atqa, 2);
  if (ok) ok = readLine(f, line) && line.startsWith("SAK=") &&
                decodeHex(line.c_str() + 4, &candidate.sak, 1);
  if (ok) ok = readLine(f, line) && line == "KEYS=NOT_STORED";
  for (uint8_t b = 0; ok && b < BLOCKS; ++b) {
    ok = readLine(f, line) && line.startsWith("B") && line.length() >= 5 &&
         line[1] == (char)('0' + b / 10) && line[2] == (char)('0' + b % 10) && line[3] == '=';
    if (!ok) break;
    const char *data = line.c_str() + 4;
    if (!strcmp(data, "--")) continue;
    ok = decodeHex(data, candidate.block[b], 16);
    candidate.valid[b] = ok;
  }
  if (ok) { ok = !f.available(); }
  f.close();
  if (!ok) return false; // do not replace RAM with a malformed or truncated dump
  candidate.hasUid = true;
  card = candidate;
  strcpy(selectedFile, name);
  Serial.printf("Loaded %s (UID-only emulation; no blocks transmitted)\n", name);
  printUid();
  return true;
}

void initNfc() {
  if (nfcReady) { Serial.println("NFC already ready"); return; }
  // Some boards need to settle after USB power-up; keep the tested retry path.
  delay(500);
  const ReturnCode rc = nfc.rfalNfcInitialize();
  nfcReady = rc == ERR_NONE;
  Serial.printf("NFC init: %d\n", (int)rc);
}
void beginListen() {
  stopNfc();
  if (!nfcReady || !card.hasUid) { notify("No NFC or UID"); return; }
  if (card.uidLen != 4 && card.uidLen != 7) { notify("Need 4/7-byte UID"); return; }
  rfalLmConfPA conf = {};
  conf.nfcidLen = card.uidLen == 4 ? RFAL_LM_NFCID_LEN_04 : RFAL_LM_NFCID_LEN_07;
  memcpy(conf.nfcid, card.uid, card.uidLen);
  memcpy(conf.SENS_RES, card.atqa, 2);
  conf.SEL_RES = card.sak;
  const ReturnCode rc = reader.rfalListenStart(RFAL_LM_MASK_NFCA, &conf, nullptr, nullptr,
                                                listenRx, sizeof(listenRx) * 8U, &listenBits);
  if (rc != ERR_NONE) { Serial.printf("Listen error: %d\n", (int)rc); notify("Listen failed"); return; }
  mode = Mode::Listen;
  page = Page::Result;
  notify("UID ONLY active");
  draw();
}
void startRead() {
  if (!nfcReady || !fsReady) { notify("Need NFC + flash"); draw(); return; }
  stopNfc();
  const ReturnCode rc = nfc.rfalNfcDiscover(&discover);
  if (rc != ERR_NONE) { Serial.printf("Discover error: %d\n", (int)rc); notify("Scan failed"); draw(); return; }
  mode = Mode::Read;
  page = Page::Result;
  notify("Present Classic 1K");
  draw();
}
void buttons() {
  bool a = up.event(), b = down.event(), c = ok.event(), d = back.event();
  if (d) {
    stopNfc();
    page = Page::Home;
    notify("Ready"); draw();
    return;
  }
  if (mode != Mode::Idle) return;
  if (page == Page::Home) {
    if (a) menuCursor = (menuCursor + 2) % 3;
    if (b) menuCursor = (menuCursor + 1) % 3;
    if (c) {
      if (menuCursor == 0) { startRead(); return; }
      if (menuCursor == 1) { listFiles(false); page = Page::Files; }
      if (menuCursor == 2) page = Page::PcFiles;
    }
    if (a || b || c) draw();
  } else if (page == Page::Files) {
    if (fileCount) {
      if (a) fileCursor = (fileCursor + fileCount - 1) % fileCount;
      if (b) fileCursor = (fileCursor + 1) % fileCount;
      if (c) {
        if (loadDump(filenames[fileCursor])) beginListen();
        else { notify("Invalid dump file"); draw(); }
        return;
      }
    }
    if (a || b) draw();
  }
}
// Process serial input without blocking the NFC worker. Only STOP is allowed
// while dumping; disk access is never performed in listen mode.
void handleCommand(char *input) {
  while (*input == ' ') ++input;
  size_t len = strlen(input);
  while (len && input[len - 1] == ' ') input[--len] = '\0';
  for (char *p = input; *p; ++p) *p = (char)toupper((unsigned char)*p);
  if (!strcmp(input, "STOP")) { stopNfc(); page = Page::Home; notify("Stopped"); draw(); return; }
  if (mode != Mode::Idle) { Serial.println("Busy: send STOP first"); return; }
  if (!strcmp(input, "HELP")) {
    Serial.println("READ | FILES | CAT NAME.TXT | RENAME OLD.TXT NEW.TXT");
    Serial.println("EMU NAME.TXT | UID | STATUS | OLED | FSINIT | FORMAT CONFIRM | INIT | STOP | HELP");
  } else if (!strcmp(input, "READ")) startRead();
  else if (!strcmp(input, "FILES")) listFiles();
  else if (!strcmp(input, "UID")) printUid();
  else if (!strcmp(input, "STATUS")) {
    Serial.printf("NFC=%d LittleFS=%d file=%s\n", nfcReady, fsReady, selectedFile);
    printUid();
  } else if (!strcmp(input, "OLED")) initOled();
  else if (!strcmp(input, "FSINIT")) mountFS();
  else if (!strcmp(input, "FORMAT CONFIRM")) {
    if (fsReady) { Serial.println("LittleFS is mounted; FORMAT refused. Back up files first."); return; }
    Serial.println("Formatting LittleFS partition: all stored dumps will be erased!");
    if (!LittleFS.format()) { Serial.println("Format failed"); return; }
    mountFS();
  } else if (!strcmp(input, "INIT")) initNfc();
  else if (!strncmp(input, "EMU ", 4)) {
    if (loadDump(input + 4)) beginListen();
    else Serial.println("Invalid/missing dump");
  } else if (!strncmp(input, "CAT ", 4)) {
    const char *name = input + 4;
    if (!fsReady || !filenameOK(name)) { Serial.println("Invalid filename/flash"); return; }
    char path[14]; pathFor(name, path);
    File f = LittleFS.open(path, FILE_READ);
    if (!f) { Serial.println("No such file"); return; }
    if (f.size() > 8192) Serial.println("File too large");
    else while (f.available()) Serial.write(f.read());
    f.close();
  } else if (!strncmp(input, "RENAME ", 7)) {
    char *old = input + 7, *space = strchr(old, ' ');
    if (!space) { Serial.println("Usage: RENAME OLD.TXT NEW.TXT"); return; }
    *space++ = '\0';
    if (!fsReady || !filenameOK(old) || !filenameOK(space)) {
      Serial.println("Invalid filename/flash"); return;
    }
    char from[14], to[14]; pathFor(old, from); pathFor(space, to);
    if (!LittleFS.exists(from) || LittleFS.exists(to) || !LittleFS.rename(from, to)) {
      Serial.println("Rename failed (missing source or target exists)"); return;
    }
    if (!strcmp(selectedFile, old)) strcpy(selectedFile, space);
    Serial.printf("Renamed %s -> %s\n", old, space);
    listFiles(false); draw();
  } else if (*input) Serial.println("Unknown command; HELP");
}
void serviceSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (lineOverflow) Serial.println("Command too long");
      else if (lineLen) { lineBuf[lineLen] = '\0'; handleCommand(lineBuf); }
      lineLen = 0; lineOverflow = false;
    } else if (!lineOverflow) {
      if (lineLen + 1 < sizeof(lineBuf)) lineBuf[lineLen++] = c;
      else lineOverflow = true;
    }
  }
}
void controls() { buttons(); serviceSerial(); }

bool acquireSame(const uint8_t *uid, uint8_t len, rfalNfcDevice **out, uint32_t id) {
  if (nfc.rfalNfcGetState() > RFAL_NFC_STATE_IDLE) nfc.rfalNfcDeactivate(false);
  if (nfc.rfalNfcDiscover(&discover) != ERR_NONE) return false;
  const uint32_t start = millis();
  while (millis() - start < 1800) {
    controls();
    if (generation != id) return false;
    nfc.rfalNfcWorker();
    if (nfc.rfalNfcGetState() == RFAL_NFC_STATE_ACTIVATED) {
      rfalNfcDevice *dev = nullptr;
      if (nfc.rfalNfcGetActiveDevice(&dev) != ERR_NONE || !dev ||
          dev->nfcidLen != len || memcmp(dev->nfcid, uid, len)) return false;
      *out = dev;
      return true;
    }
    delay(1);
  }
  return false;
}
void readCard(rfalNfcDevice *first) {
  if (!first || first->type != RFAL_NFC_LISTEN_TYPE_NFCA ||
      first->nfcidLen == 0 || first->nfcidLen > 10) {
    notify("Unsupported NFC-A"); stopNfc(); draw(); return;
  }
  card = Dump{};
  memcpy(card.uid, first->nfcid, first->nfcidLen);
  card.uidLen = first->nfcidLen;
  card.atqa[0] = first->dev.nfca.sensRes.anticollisionInfo;
  card.atqa[1] = first->dev.nfca.sensRes.platformInfo;
  card.sak = first->dev.nfca.selRes.sak;
  card.hasUid = true;
  selectedFile[0] = '\0';
  printUid();
  const uint32_t id = generation;
  unsigned count = 0;
  if (card.uidLen == 4 && card.sak == 0x08) {
    uint8_t expected[4]; memcpy(expected, card.uid, 4);
    for (uint8_t sector = 0; sector < SECTORS; ++sector) {
      controls(); if (generation != id) return;
      rfalNfcDevice *dev = nullptr;
      if (!acquireSame(expected, 4, &dev, id)) {
        if (generation != id) return;
        Serial.printf("Sector %u: rediscovery failed\n", sector);
        break;
      }
      rfalMf1CryptoState cipher = {};
      const uint8_t base = sector * 4;
      ReturnCode rc = mf1.authenticate(&cipher, dev, base, keyA[sector], RFAL_MF1_AUTH_KEY_A);
      if (rc != ERR_NONE) { Serial.printf("Sector %u: auth %d\n", sector, (int)rc); continue; }
      for (uint8_t offset = 0; offset < 4; ++offset) {
        controls(); if (generation != id) return;
        const uint8_t block = base + offset;
        uint8_t data[16];
        rc = mf1.readBlock(&cipher, block, data);
        if (rc != ERR_NONE) { Serial.printf("Block %u: read %d\n", block, (int)rc); break; }
        memcpy(card.block[block], data, 16);
        if (offset == 3) { // trailer keys cannot be recovered by READ
          memset(card.block[block], 0, 6);
          memset(card.block[block] + 10, 0, 6);
        }
        card.valid[block] = true;
        ++count;
      }
      snprintf(message, sizeof(message), "Sector %u/16 %u/64", sector + 1, count);
      Serial.println(message); draw();
    }
  } else Serial.println("Not Classic 1K/4-byte UID; saving UID only.");
  stopNfc();
  Serial.printf("Read %u/64 blocks; incomplete sectors remain --\n", count);
  const bool saved = saveDump();
  page = Page::Result;
  if (!saved) notify("Flash save FAILED");
  draw();
}
} // namespace

void setup() {
  Serial.begin(115200);
  pinMode(NFC_CS, OUTPUT); digitalWrite(NFC_CS, HIGH);
  for (auto &key : keyA) memcpy(key, DEFAULT_KEY, 6);
  for (uint8_t pin : {PIN_UP, PIN_DOWN, PIN_OK, PIN_BACK}) pinMode(pin, INPUT_PULLUP);
  initOled();
  spi.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, NFC_CS);
  // Keep the working upstream initialization order: no register probe.
  initNfc();
  discover.compMode = RFAL_COMPLIANCE_MODE_NFC;
  discover.devLimit = 1;
  discover.techs2Find = RFAL_NFC_POLL_TECH_A;
  discover.totalDuration = RFAL_ESP32_DEFAULT_DISCOVERY_DURATION_MS;
  mountFS();
  notify("Ready: READ/HELP"); draw();
}
void loop() {
  controls();
  if (mode == Mode::Read) {
    nfc.rfalNfcWorker();
    if (nfc.rfalNfcGetState() == RFAL_NFC_STATE_ACTIVATED) {
      rfalNfcDevice *dev = nullptr;
      if (nfc.rfalNfcGetActiveDevice(&dev) == ERR_NONE) readCard(dev);
    }
  } else if (mode == Mode::Listen) {
    reader.rfalWorker();
    bool data = false;
    reader.rfalListenGetState(&data, nullptr);
    if (data) reader.rfalListenSetState(RFAL_LM_STATE_IDLE); // no Classic AUTH/READ
  }
  delay(1);
}
