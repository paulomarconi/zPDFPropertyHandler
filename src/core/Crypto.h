// Minimal, dependency-free crypto primitives needed to read encrypted PDFs
// (standard security handler, revisions 2-6): MD5, RC4, AES-128/256 (CBC),
// SHA-256/384/512.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pdfprops {
namespace crypto {

std::string Md5(const std::string& data);
std::string Sha256(const std::string& data);
std::string Sha384(const std::string& data);
std::string Sha512(const std::string& data);

// RC4 is symmetric: the same call encrypts and decrypts.
std::string Rc4(const std::string& key, const std::string& data);

// AES in CBC mode. Key must be 16 or 32 bytes, iv 16 bytes.
// Data length must be a multiple of 16 (any remainder is ignored). No padding is
// added or removed; callers deal with PKCS#5 padding themselves.
std::string AesCbcEncrypt(const std::string& key, const std::string& iv, const std::string& data);
std::string AesCbcDecrypt(const std::string& key, const std::string& iv, const std::string& data);

}  // namespace crypto
}  // namespace pdfprops
