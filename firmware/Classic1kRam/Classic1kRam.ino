/* ESP32 DevKit V1 + ELECHOUSE ST25R3916 (SPI): ISO14443-A UID test.
 * No screen, keys, card blocks, Crypto1 or Classic emulation.
 * The listen mode advertises only the saved UID/ATQA/SAK.
 */
#include <Arduino.h>
#include <SPI.h>
#include <rfal_nfc.h>
#include <rfal_rfst25r3916.h>
#include <st25r3916_config.h>
#include <st_errno.h>
#include <ctype.h>
#include <string.h>

namespace {
constexpr int PIN_SCK = 18, PIN_MISO = 19, PIN_MOSI = 23;
constexpr int PIN_CS = 5, PIN_IRQ = 4;

SPIClass spi(VSPI);
// Use the same SPI constructor and default clock as the working upstream
// ESP32_SPI_scan_14443A_led example. No register access before initialize().
RfalRfST25R3916Class reader(&spi, PIN_CS, PIN_IRQ);
RfalNfcClass nfc(&reader);
rfalNfcDiscoverParam discover = {};

struct UidSnapshot {
  uint8_t uid[10] = {};
  uint8_t length = 0;
  uint8_t atqa[2] = {};
  uint8_t sak = 0;
  bool valid = false;
};
UidSnapshot saved;

enum class Mode { Idle, Read, ListenUid };
Mode mode = Mode::Idle;
bool nfcReady = false;
void tryInitialize() {
  if (nfcReady) { Serial.println("NFC already initialized."); return; }
  if (mode != Mode::Idle) { Serial.println("Send STOP before INIT."); return; }
  const ReturnCode rc = nfc.rfalNfcInitialize();
  nfcReady = (rc == ERR_NONE);
  Serial.printf("rfalNfcInitialize: %d\n", (int)rc);
}
// RFAL retains these pointers for as long as listen mode is running.
uint8_t listenRx[32] = {};
uint16_t listenBits = 0;
char command[32] = {};
size_t commandLength = 0;
bool commandOverflow = false;

void printUid() {
  if (!saved.valid) { Serial.println("No UID in RAM. Send READ first."); return; }
  Serial.print("UID ("); Serial.print(saved.length); Serial.print(" bytes): ");
  for (uint8_t i = 0; i < saved.length; ++i) {
    if (i) Serial.print(':');
    if (saved.uid[i] < 0x10) Serial.print('0');
    Serial.print(saved.uid[i], HEX);
  }
  Serial.printf("  ATQA=%02X%02X SAK=%02X\n", saved.atqa[0], saved.atqa[1], saved.sak);
}
void stopNfc() {
  if (mode == Mode::ListenUid) reader.rfalListenStop();
  if (nfcReady && nfc.rfalNfcGetState() > RFAL_NFC_STATE_IDLE)
    nfc.rfalNfcDeactivate(false);
  mode = Mode::Idle; // Do not clear RAM on STOP or mode changes.
}
void beginRead() {
  if (!nfcReady) { Serial.println("NFC not initialized; send INIT to retry."); return; }
  stopNfc();
  const ReturnCode rc = nfc.rfalNfcDiscover(&discover);
  if (rc != ERR_NONE) { Serial.printf("Discover failed: %d\n", (int)rc); return; }
  mode = Mode::Read;
  Serial.println("Scanning ISO14443-A... present a card (STOP to cancel).");
}
void beginListen() {
  if (!nfcReady) { Serial.println("NFC not initialized; send INIT to retry."); return; }
  stopNfc();
  if (!saved.valid) { Serial.println("No UID in RAM. Send READ first."); return; }
  if (saved.length != 4 && saved.length != 7) {
    Serial.println("This RFAL listen mode supports only 4- or 7-byte UIDs."); return;
  }
  rfalLmConfPA conf = {};
  conf.nfcidLen = saved.length == 4 ? RFAL_LM_NFCID_LEN_04 : RFAL_LM_NFCID_LEN_07;
  memcpy(conf.nfcid, saved.uid, saved.length);
  memcpy(conf.SENS_RES, saved.atqa, sizeof(saved.atqa));
  conf.SEL_RES = saved.sak;
  const ReturnCode rc = reader.rfalListenStart(RFAL_LM_MASK_NFCA, &conf, nullptr, nullptr,
                                                listenRx, sizeof(listenRx) * 8U, &listenBits);
  if (rc != ERR_NONE) { Serial.printf("Listen failed: %d\n", (int)rc); return; }
  mode = Mode::ListenUid;
  Serial.println("UID-ONLY listen mode: no authentication or card data. STOP to exit.");
  printUid();
}
void printHelp() {
  Serial.println("Commands (115200 baud, newline):");
  Serial.println("INIT   - retry NFC initialization if startup failed");
  Serial.println("READ   - scan one ISO14443-A UID into RAM");
  Serial.println("EMU    - advertise saved UID/ATQA/SAK only (NOT a Classic card)");
  Serial.println("UID    - print saved UID/ATQA/SAK");
  Serial.println("STOP   - stop scan/listen, keep saved UID");
  Serial.println("STATUS - show current mode and saved UID");
  Serial.println("HELP   - show commands");
}
void handleCommand(char *line) {
  while (*line == ' ' || *line == '\t') ++line;
  size_t length = strlen(line);
  while (length && (line[length - 1] == ' ' || line[length - 1] == '\t')) line[--length] = '\0';
  for (char *p = line; *p; ++p) *p = (char)toupper((unsigned char)*p);
  if (!*line) return;
  if (!strcmp(line, "INIT")) tryInitialize();
  else if (!strcmp(line, "READ")) beginRead();
  else if (!strcmp(line, "EMU")) beginListen();
  else if (!strcmp(line, "STOP")) { stopNfc(); Serial.println("Stopped; UID retained in RAM."); }
  else if (!strcmp(line, "UID")) printUid();
  else if (!strcmp(line, "STATUS")) {
    Serial.printf("NFC=%s mode=%s\n", nfcReady ? "ready" : "init failed",
                  mode == Mode::Read ? "READ" : mode == Mode::ListenUid ? "UID-ONLY" : "IDLE");
    printUid();
  } else if (!strcmp(line, "HELP")) printHelp();
  else Serial.println("Unknown command. Send HELP.");
}
void serviceSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (commandOverflow) Serial.println("Command too long.");
      else if (commandLength) { command[commandLength] = '\0'; handleCommand(command); }
      commandLength = 0;
      commandOverflow = false;
    } else if (!commandOverflow) {
      if (commandLength < sizeof(command) - 1) command[commandLength++] = c;
      else commandOverflow = true;
    }
  }
}
void onCard(rfalNfcDevice *dev) {
  // A remote tag is a LISTENER even though this ESP32 is POLLING it. Match
  // the upstream scanner's isNfcaDevice() check; POLL_TYPE_NFCA describes
  // a remote reader, not the normal card we just detected.
  if (!dev || (dev->type != RFAL_NFC_LISTEN_TYPE_NFCA &&
               dev->type != RFAL_NFC_POLL_TYPE_NFCA) ||
      !dev->nfcid || dev->nfcidLen == 0 || dev->nfcidLen > sizeof(saved.uid)) {
    Serial.printf("Unsupported device: type=%d uidLen=%u (max %u)\n",
                  dev ? (int)dev->type : -1,
                  dev ? (unsigned)dev->nfcidLen : 0U, (unsigned)sizeof(saved.uid));
    stopNfc();
    return;
  }
  // Only replace the old snapshot when a valid ISO14443-A UID is acquired.
  memcpy(saved.uid, dev->nfcid, dev->nfcidLen);
  saved.length = dev->nfcidLen;
  saved.atqa[0] = dev->dev.nfca.sensRes.anticollisionInfo;
  saved.atqa[1] = dev->dev.nfca.sensRes.platformInfo;
  saved.sak = dev->dev.nfca.selRes.sak;
  saved.valid = true;
  stopNfc();
  Serial.println("UID saved to RAM (cleared by reset/power-off).");
  printUid();
}
} // namespace

void setup() {
  Serial.begin(115200);
  // Match the working upstream SPI scanner's initialization order.
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  // Let the USB-powered NFC module settle before the first identity check.
  delay(500);
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Serial.println("ESP32 SPI ISO14443A UID-only test v2");
  Serial.printf("SCK=%d MISO=%d MOSI=%d CS=%d IRQ=%d\n",
                PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS, PIN_IRQ);
  discover.compMode = RFAL_COMPLIANCE_MODE_NFC;
  discover.devLimit = 1;
  discover.techs2Find = RFAL_NFC_POLL_TECH_A;
  discover.totalDuration = RFAL_ESP32_DEFAULT_DISCOVERY_DURATION_MS;
  tryInitialize();
  printHelp();
}
void loop() {
  serviceSerial();
  if (mode == Mode::Read) {
    nfc.rfalNfcWorker();
    if (nfc.rfalNfcGetState() == RFAL_NFC_STATE_ACTIVATED) {
      rfalNfcDevice *dev = nullptr;
      if (nfc.rfalNfcGetActiveDevice(&dev) == ERR_NONE) onCard(dev);
    }
  } else if (mode == Mode::ListenUid) {
    reader.rfalWorker();
    bool received = false;
    reader.rfalListenGetState(&received, nullptr);
    if (received) {
      // Do not answer AUTH/READ or pretend this is a MIFARE Classic card.
      reader.rfalListenSetState(RFAL_LM_STATE_IDLE);
    }
  }
  delay(1);
}
