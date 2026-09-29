// Known-answer tests for the crypto primitives (FIPS 180/197 examples, well-known digests).
#include <cstdio>
#include <string>

#include "../src/core/Crypto.h"

using namespace pdfprops::crypto;

namespace {

std::string Hex(const std::string& s) {
    std::string o;
    char b[3];
    for (unsigned char c : s) {
        snprintf(b, sizeof b, "%02x", c);
        o += b;
    }
    return o;
}

std::string Unhex(const char* h) {
    std::string o;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        o += static_cast<char>(v);
    }
    return o;
}

int failures = 0;

void Check(const char* name, const std::string& got, const char* want) {
    bool ok = got == want;
    printf("%-10s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) {
        printf("  got  %s\n  want %s\n", got.c_str(), want);
        ++failures;
    }
}

}  // namespace

int main() {
    const std::string fox = "The quick brown fox jumps over the lazy dog";
    const std::string a1000(1000, 'a');
    Check("MD5", Hex(Md5(fox)), "9e107d9d372bb6826bd81d3542a419d6");
    Check("SHA-256", Hex(Sha256(fox)), "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    Check("SHA-384", Hex(Sha384(fox)),
          "ca737f1014a48f4c0b6dd43cb177b0afd9e5169367544c494011e3317dbf9a509cb1e5dc1e85a941bbee3d7f2afbc9b1");
    Check("SHA-512", Hex(Sha512(a1000)),
          "67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634fa7b12b47d77b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97");
    Check("RC4", Hex(Rc4("Key", "Plaintext")), "bbf316e8d940af0ad3");

    const std::string iv(16, '\0');
    const std::string pt = Unhex("00112233445566778899aabbccddeeff");
    const std::string k128 = Unhex("000102030405060708090a0b0c0d0e0f");
    const std::string k256 = Unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    Check("AES-128", Hex(AesCbcEncrypt(k128, iv, pt)), "69c4e0d86a7b0430d8cdb78070b4c55a");
    std::string c256 = AesCbcEncrypt(k256, iv, pt);
    Check("AES-256", Hex(c256), "8ea2b7ca516745bfeafc49904b496089");
    Check("AES-256-d", Hex(AesCbcDecrypt(k256, iv, c256)), "00112233445566778899aabbccddeeff");

    const std::string iv2 = Unhex("0f0e0d0c0b0a09080706050403020100");
    const std::string block4 = a1000.substr(0, 64);
    Check("AES-CBC", AesCbcDecrypt(k128, iv2, AesCbcEncrypt(k128, iv2, block4)) == block4 ? "roundtrip" : "mismatch",
          "roundtrip");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
