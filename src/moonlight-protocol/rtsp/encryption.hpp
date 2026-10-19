#pragma once

#include <boost/endian/conversion.hpp>
#include <crypto/crypto.hpp>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

/**
 * Sunshine's encrypted RTSP (advertised to the client as an `rtspenc://` sessionUrl0).
 *
 * Every message on the wire is:
 *   uint32 typeAndLength (big endian, top bit set, low 31 bits = ciphertext length)
 *   uint32 sequenceNumber (big endian)
 *   uint8  tag[16]        (AES-GCM tag)
 *   ...    ciphertext     (the plaintext RTSP message, AES-128-GCM with the launch's rikey)
 *
 * The 12-byte IV is the sequence number (little endian) in bytes 0-3, then 'C'/'H' (client/host originated) in
 * byte 10 and 'R' (RTSP) in byte 11. See moonlight-common-c RtspConnection.c (sealRtspMessage/unsealRtspMessage).
 */
namespace rtsp::encryption {

constexpr std::uint32_t ENCRYPTED_RTSP_BIT = 0x80000000;
constexpr std::size_t GCM_TAG_SIZE = 16;
constexpr std::size_t HEADER_SIZE = sizeof(std::uint32_t) * 2 + GCM_TAG_SIZE;

constexpr char CLIENT_ORIGINATED = 'C';
constexpr char HOST_ORIGINATED = 'H';

/**
 * A plaintext RTSP message starts with an ASCII method or "RTSP/1.0", so the top bit of the first byte is only ever
 * set for an encrypted one.
 */
inline bool is_encrypted(std::string_view raw) {
  return !raw.empty() && (static_cast<std::uint8_t>(raw[0]) & 0x80);
}

inline std::uint32_t read_be32(std::string_view raw, std::size_t offset) {
  std::uint32_t value;
  std::memcpy(&value, raw.data() + offset, sizeof(value));
  return boost::endian::big_to_native(value);
}

/**
 * The total size of the encrypted message that starts in raw, or nullopt while the header hasn't fully arrived yet.
 */
inline std::optional<std::size_t> full_message_size(std::string_view raw) {
  if (raw.size() < HEADER_SIZE) {
    return std::nullopt;
  }
  return HEADER_SIZE + (read_be32(raw, 0) & ~ENCRYPTED_RTSP_BIT);
}

inline std::string make_iv(std::uint32_t seq, char origin) {
  std::string iv(12, '\0');
  auto seq_le = boost::endian::native_to_little(seq);
  std::memcpy(iv.data(), &seq_le, sizeof(seq_le));
  iv[10] = origin;
  iv[11] = 'R';
  return iv;
}

/**
 * Encrypts a full RTSP message into the wire format above.
 *
 * @param key: the raw 16 bytes AES key (not hex)
 * @param seq: must never repeat for the same key and origin, it's the GCM IV
 */
inline std::string seal(std::string_view plaintext, std::string_view key, std::uint32_t seq, char origin) {
  auto [ciphertext, tag] = crypto::aes_encrypt_gcm(plaintext, key, make_iv(seq, origin), 12);

  auto type_and_length =
      boost::endian::native_to_big(ENCRYPTED_RTSP_BIT | static_cast<std::uint32_t>(ciphertext.size()));
  auto seq_be = boost::endian::native_to_big(seq);

  std::string msg;
  msg.reserve(HEADER_SIZE + ciphertext.size());
  msg.append(reinterpret_cast<const char *>(&type_and_length), sizeof(type_and_length));
  msg.append(reinterpret_cast<const char *>(&seq_be), sizeof(seq_be));
  msg.append(tag);
  msg.append(ciphertext);
  return msg;
}

/**
 * Decrypts and authenticates a full message in the wire format above.
 *
 * @param key: the raw 16 bytes AES key (not hex)
 * @return the plaintext RTSP message, or nullopt when raw isn't exactly one well formed message sealed with this key
 */
inline std::optional<std::string> unseal(std::string_view raw, std::string_view key, char origin) {
  auto size = full_message_size(raw);
  if (key.size() != 16 || !is_encrypted(raw) || !size || *size != raw.size() || *size == HEADER_SIZE) {
    return std::nullopt;
  }
  auto seq = read_be32(raw, sizeof(std::uint32_t));
  auto tag = raw.substr(sizeof(std::uint32_t) * 2, GCM_TAG_SIZE);
  auto ciphertext = raw.substr(HEADER_SIZE);
  try {
    return crypto::aes_decrypt_gcm(ciphertext, key, tag, make_iv(seq, origin), 12);
  } catch (const std::runtime_error &) { // wrong key or tampered message: the tag doesn't verify
    return std::nullopt;
  }
}

} // namespace rtsp::encryption
