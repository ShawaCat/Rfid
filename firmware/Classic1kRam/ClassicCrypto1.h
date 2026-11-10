#pragma once
// Transport-independent MIFARE Classic Crypto1 primitives. No RF timing or
// physical-layer framing is handled here. For authorized test cards only.
#include <stddef.h>
#include <stdint.h>

namespace classic {
inline uint8_t oddParity(uint8_t b) {
  b ^= b >> 4; b ^= b >> 2; b ^= b >> 1;
  return (uint8_t)((b ^ 1U) & 1U);
}
inline uint16_t crcA(const uint8_t* bytes, size_t len) {
  uint16_t crc = 0x6363U;
  while (len--) {
    uint8_t v = (uint8_t)(*bytes++ ^ (uint8_t)crc);
    v ^= (uint8_t)(v << 4);
    crc = (uint16_t)((crc >> 8) ^ ((uint16_t)v << 8) ^
                     ((uint16_t)v << 3) ^ ((uint16_t)v >> 4));
  }
  return crc;
}
inline uint32_t prngSuccessor(uint32_t v, unsigned clocks) {
  v = __builtin_bswap32(v);
  while (clocks--) v = (v >> 1) |
    (((v >> 16 ^ v >> 18 ^ v >> 19 ^ v >> 21) & 1U) << 31);
  return __builtin_bswap32(v);
}
inline uint32_t readBe32(const uint8_t* b) {
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
inline void writeBe32(uint8_t* b, uint32_t v) {
  b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}

struct Cipher {
  uint32_t odd = 0, even = 0;
  void init(const uint8_t key[6]) {
    uint64_t k = 0;
    for (unsigned i = 0; i < 6; ++i) k = (k << 8) | key[i];
    odd = even = 0;
    for (int i = 47; i > 0; i -= 2) {
      odd = (odd << 1) | ((k >> ((i - 1) ^ 7)) & 1U);
      even = (even << 1) | ((k >> (i ^ 7)) & 1U);
    }
  }
  uint8_t filter() const {
    uint32_t f = (0x000F22C0U >> (odd & 15U)) & 0x10U;
    f |= (0x0006C9C0U >> ((odd >> 4) & 15U)) & 0x08U;
    f |= (0x0003C8B0U >> ((odd >> 8) & 15U)) & 0x04U;
    f |= (0x0001E458U >> ((odd >> 12) & 15U)) & 0x02U;
    f |= (0x0000D938U >> ((odd >> 16) & 15U)) & 0x01U;
    return (uint8_t)((0xEC57E80AU >> f) & 1U);
  }
  uint8_t bit(uint8_t in, bool ciphertextInput = false) {
    uint8_t out = filter();
    uint32_t fb = (in & 1U) ^ (ciphertextInput ? out : 0U);
    fb ^= odd & 0x0029CE5CU;
    fb ^= even & 0x00870804U;
    uint32_t next = ((even << 1) & 0x00FFFFFFU) | (__builtin_parity(fb) & 1U);
    even = odd; odd = next;
    return out;
  }
  uint8_t byte(uint8_t in, bool ciphertextInput = false) {
    uint8_t stream = 0;
    for (unsigned i = 0; i < 8; ++i)
      stream |= bit((in >> i) & 1U, ciphertextInput) << i;
    return stream;
  }
  uint32_t word(uint32_t in, bool ciphertextInput = false) {
    uint32_t stream = 0;
    for (unsigned i = 0; i < 32; ++i)
      stream |= (uint32_t)bit((in >> (i ^ 24U)) & 1U, ciphertextInput) << (i ^ 24U);
    return stream;
  }
  // In the encrypted session, input to the LFSR is zero. Crypto1 does not
  // clock on parity bits; the parity keystream bit is filter() after each byte.
  uint8_t encryptByte(uint8_t plain, uint8_t& parityBit) {
    uint8_t cipher = plain ^ byte(0);
    parityBit = oddParity(plain) ^ filter();
    return cipher;
  }
  bool decryptByte(uint8_t cipher, uint8_t encryptedParity, uint8_t& plain) {
    plain = cipher ^ byte(0);
    return (uint8_t)(oddParity(plain) ^ filter()) == encryptedParity;
  }
};

// Standard ISO14443A CRC must be appended BEFORE encryption. Explicitly
// separate encrypted parity from each byte for transparent-mode transmission.
// Each ISO14443A data byte is sent LSB first, followed by its parity bit.
// 18 bytes + parity = 162 bits (21 packed bytes). The PHY still must generate
// proper load modulation / framing; packed bits cannot simply be written to
// the ST25R3916 FIFO as ordinary bytes.
inline void pack9(const uint8_t* data, const uint8_t* parity, size_t count,
                  uint8_t* packed) {
  for (size_t i = 0; i < (count * 9 + 7) / 8; ++i) packed[i] = 0;
  for (size_t i = 0; i < count; ++i) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      const size_t pos = 9 * i + bit;
      packed[pos / 8] |= ((data[i] >> bit) & 1U) << (pos % 8);
    }
    const size_t pos = 9 * i + 8;
    packed[pos / 8] |= (parity[i] & 1U) << (pos % 8);
  }
}
inline void unpack9(const uint8_t* packed, size_t count, uint8_t* data,
                    uint8_t* parity) {
  for (size_t i = 0; i < count; ++i) {
    data[i] = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
      const size_t pos = 9 * i + bit;
      data[i] |= ((packed[pos / 8] >> (pos % 8)) & 1U) << bit;
    }
    const size_t pos = 9 * i + 8;
    parity[i] = (packed[pos / 8] >> (pos % 8)) & 1U;
  }
}
struct Frame {
  uint8_t data[18] = {};
  uint8_t parity[18] = {}; // low bit of each element
  size_t length = 0;
};

// Single-sector, Key-A, READ-only prototype. The caller must supply actual
// ciphertext bytes and encrypted parity from the NFC frontend, and transmit
// Frame bits including parity at the required ISO14443A frame delay.
class Session {
 public:
  enum class State { Idle, Challenge, Authenticated };
  State state = State::Idle;
  uint8_t sector = 0;
  uint32_t nonce = 0;
  Cipher crypto;

  void reset() { state = State::Idle; }
  // Called for a *plaintext*, CRC-verified 0x60 command. Does not send RF.
  bool begin(uint8_t block, const uint8_t uid[4], const uint8_t keyA[6], uint32_t nt,
             uint8_t challenge[4]) {
    reset();
    if (block >= 64) return false;
    sector = block / 4;
    nonce = nt;
    crypto.init(keyA);
    crypto.word(nt ^ readBe32(uid));
    writeBe32(challenge, nt); // sent in clear, with ordinary odd parity
    state = State::Challenge;
    return true;
  }
  // nrAr: 8 ciphertext bytes plus one parity bit per byte (9-bit framing).
  // On success produces the encrypted At (including parity). The caller MUST
  // discard the session and remain silent on malformed/failed authentication.
  bool finish(const uint8_t nrAr[8], const uint8_t par[8], Frame& answer) {
    if (state != State::Challenge) return false;
    bool parityOK = true;
    for (unsigned i = 0; i < 4; ++i) {
      uint8_t plain = nrAr[i] ^ crypto.byte(nrAr[i], true);
      parityOK &= (uint8_t)(oddParity(plain) ^ crypto.filter()) == par[i];
    }
    uint8_t ar[4];
    for (unsigned i = 0; i < 4; ++i)
      parityOK &= crypto.decryptByte(nrAr[4 + i], par[4 + i], ar[i]);
    if (!parityOK || readBe32(ar) != prngSuccessor(nonce, 64)) {
      reset(); return false;
    }
    uint8_t at[4]; writeBe32(at, prngSuccessor(nonce, 96));
    answer.length = 4;
    for (unsigned i = 0; i < 4; ++i)
      answer.data[i] = crypto.encryptByte(at[i], answer.parity[i]);
    state = State::Authenticated;
    return true;
  }
  // A 4-byte ciphertext command (command, block, CRC_A) and its four parity
  // bits. Invalid sector/block/CRC => remain silent (do not leak RAM content).
  bool read(const uint8_t cmd[4], const uint8_t par[4],
            const uint8_t blocks[64][16], const bool valid[64], Frame& answer) {
    if (state != State::Authenticated) return false;
    uint8_t plain[4];
    for (unsigned i = 0; i < 4; ++i)
      if (!crypto.decryptByte(cmd[i], par[i], plain[i])) { reset(); return false; }
    uint16_t crc = crcA(plain, 2);
    if (plain[0] != 0x30 || plain[1] >= 64 || plain[1] / 4 != sector ||
        !valid[plain[1]] || plain[2] != (uint8_t)crc || plain[3] != (uint8_t)(crc >> 8)) {
      reset(); return false;
    }
    answer.length = 18;
    crc = crcA(blocks[plain[1]], 16);
    for (unsigned i = 0; i < 18; ++i) {
      uint8_t v = i < 16 ? blocks[plain[1]][i] :
                  i == 16 ? (uint8_t)crc : (uint8_t)(crc >> 8);
      answer.data[i] = crypto.encryptByte(v, answer.parity[i]);
    }
    return true;
  }
};
} // namespace classic
