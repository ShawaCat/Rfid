#include "../firmware/Classic1kRam/ClassicCrypto1.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace classic;

int main() {
  const uint8_t uid[4] = {0x11, 0x22, 0x33, 0x44};
  const uint8_t key[6] = {255, 255, 255, 255, 255, 255};
  const uint32_t nt = 0x12345678;
  assert(crcA((const uint8_t*)"123456789", 9) == 0xBF05); // CRC_A fixture
  uint8_t input[2] = {0xA5, 0x3C}, inputParity[2] = {1, 0};
  uint8_t packed[3], recovered[2], recoveredParity[2];
  pack9(input, inputParity, 2, packed);
  unpack9(packed, 2, recovered, recoveredParity);
  assert(!memcmp(input, recovered, 2) && !memcmp(inputParity, recoveredParity, 2));
  Session picc;
  uint8_t challenge[4] = {};
  assert(picc.begin(4, uid, key, nt, challenge));
  assert(readBe32(challenge) == nt);
  // Simulate a reader using its own, independently clocked cipher instance.
  Cipher reader;
  reader.init(key);
  reader.word(nt ^ readBe32(uid));
  uint8_t nrAr[8], parity[8];
  const uint32_t nr = 0x89ABCDEF;
  uint8_t nrPlain[4]; writeBe32(nrPlain, nr);
  for (unsigned i = 0; i < 4; ++i) {
    uint8_t p = nrPlain[i];
    nrAr[i] = p ^ reader.byte(p);
    parity[i] = oddParity(p) ^ reader.filter();
  }
  uint8_t arPlain[4]; writeBe32(arPlain, prngSuccessor(nt, 64));
  for (unsigned i = 0; i < 4; ++i)
    nrAr[4 + i] = reader.encryptByte(arPlain[i], parity[4 + i]);
  Session wrongKey;
  uint8_t otherKey[6] = {1, 2, 3, 4, 5, 6};
  assert(wrongKey.begin(4, uid, otherKey, nt, challenge));
  Frame wrongAnswer;
  assert(!wrongKey.finish(nrAr, parity, wrongAnswer));
  Session bad = picc;
  Frame out;
  uint8_t badPar[8]; memcpy(badPar, parity, sizeof parity); badPar[7] ^= 1;
  assert(!bad.finish(nrAr, badPar, out));
  assert(bad.state == Session::State::Idle);
  assert(picc.finish(nrAr, parity, out) && out.length == 4);
  for (unsigned i = 0; i < 4; ++i) {
    uint8_t plain;
    assert(reader.decryptByte(out.data[i], out.parity[i], plain));
    assert(plain == (uint8_t)(prngSuccessor(nt, 96) >> (24 - 8 * i)));
  }
  uint8_t blocks[64][16] = {};
  bool valid[64] = {};
  valid[4] = true;
  for (unsigned i = 0; i < 16; ++i) blocks[4][i] = (uint8_t)i;
  uint8_t cmdPlain[4] = {0x30, 4};
  uint16_t crc = crcA(cmdPlain, 2);
  cmdPlain[2] = (uint8_t)crc; cmdPlain[3] = (uint8_t)(crc >> 8);
  uint8_t cmd[4], cmdPar[4];
  for (unsigned i = 0; i < 4; ++i)
    cmd[i] = reader.encryptByte(cmdPlain[i], cmdPar[i]);
  Session missing = picc;
  bool missingValid[64] = {};
  Frame missingAnswer;
  assert(!missing.read(cmd, cmdPar, blocks, missingValid, missingAnswer));
  assert(picc.read(cmd, cmdPar, blocks, valid, out) && out.length == 18);
  uint8_t decoded[18];
  for (unsigned i = 0; i < 18; ++i)
    assert(reader.decryptByte(out.data[i], out.parity[i], decoded[i]));
  assert(!memcmp(decoded, blocks[4], 16));
  assert(decoded[16] == (uint8_t)crcA(decoded, 16));
  assert(decoded[17] == (uint8_t)(crcA(decoded, 16) >> 8));
  std::puts("Crypto1 auth/READ/parity/CRC tests passed");
}
