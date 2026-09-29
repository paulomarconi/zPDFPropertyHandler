#include "Crypto.h"

#include <cstring>
#include <vector>

#include "CryptoConstants.h"

namespace pdfprops {
namespace crypto {

namespace {

inline uint32_t Rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t Rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t Rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// Appends Merkle-Damgard padding. lengthBytes is 8 (MD5/SHA-256) or 16 (SHA-512).
std::string Pad(const std::string& data, size_t block, size_t lengthBytes, bool bigEndian) {
    std::string m = data;
    uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % block != block - lengthBytes) m.push_back('\0');
    std::string len(lengthBytes, '\0');
    for (int i = 0; i < 8; ++i) {
        char b = static_cast<char>((bits >> (8 * i)) & 0xff);
        if (bigEndian)
            len[lengthBytes - 1 - i] = b;
        else
            len[i] = b;
    }
    return m + len;
}

}  // namespace

// ---------------------------------------------------------------- MD5
std::string Md5(const std::string& data) {
    static const int s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::string m = Pad(data, 64, 8, false);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(m.data());
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = p[off + 4 * i] | (p[off + 4 * i + 1] << 8) | (p[off + 4 * i + 2] << 16) |
                   (static_cast<uint32_t>(p[off + 4 * i + 3]) << 24);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            f = f + a + kMd5K[i] + w[g];
            a = d;
            d = c;
            c = b;
            b = b + Rotl32(f, s[i]);
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    std::string out(16, '\0');
    for (int i = 0; i < 16; ++i) out[i] = static_cast<char>((h[i / 4] >> (8 * (i % 4))) & 0xff);
    return out;
}

// ---------------------------------------------------------------- SHA-256
std::string Sha256(const std::string& data) {
    uint32_t h[8];
    std::memcpy(h, kSha256H, sizeof(h));
    std::string m = Pad(data, 64, 8, true);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(m.data());
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(p[off + 4 * i]) << 24) | (p[off + 4 * i + 1] << 16) |
                   (p[off + 4 * i + 2] << 8) | p[off + 4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
            uint32_t S0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out(32, '\0');
    for (int i = 0; i < 32; ++i) out[i] = static_cast<char>((h[i / 4] >> (24 - 8 * (i % 4))) & 0xff);
    return out;
}

// ---------------------------------------------------------------- SHA-512 / SHA-384
namespace {
std::string Sha512Core(const std::string& data, const uint64_t* iv, size_t outLen) {
    uint64_t h[8];
    std::memcpy(h, iv, sizeof(h));
    std::string m = Pad(data, 128, 16, true);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(m.data());
    for (size_t off = 0; off < m.size(); off += 128) {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            uint64_t v = 0;
            for (int j = 0; j < 8; ++j) v = (v << 8) | p[off + 8 * i + j];
            w[i] = v;
        }
        for (int i = 16; i < 80; ++i) {
            uint64_t s0 = Rotr64(w[i - 15], 1) ^ Rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = Rotr64(w[i - 2], 19) ^ Rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            uint64_t S1 = Rotr64(e, 14) ^ Rotr64(e, 18) ^ Rotr64(e, 41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = hh + S1 + ch + kSha512K[i] + w[i];
            uint64_t S0 = Rotr64(a, 28) ^ Rotr64(a, 34) ^ Rotr64(a, 39);
            uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint64_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out(64, '\0');
    for (int i = 0; i < 64; ++i) out[i] = static_cast<char>((h[i / 8] >> (56 - 8 * (i % 8))) & 0xff);
    out.resize(outLen);
    return out;
}
}  // namespace

std::string Sha512(const std::string& data) { return Sha512Core(data, kSha512H, 64); }
std::string Sha384(const std::string& data) { return Sha512Core(data, kSha384H, 48); }

// ---------------------------------------------------------------- RC4
std::string Rc4(const std::string& key, const std::string& data) {
    unsigned char S[256];
    for (int i = 0; i < 256; ++i) S[i] = static_cast<unsigned char>(i);
    if (key.empty()) return data;
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        j = (j + S[i] + static_cast<unsigned char>(key[i % key.size()])) & 0xff;
        std::swap(S[i], S[j]);
    }
    std::string out(data.size(), '\0');
    int i = 0;
    j = 0;
    for (size_t n = 0; n < data.size(); ++n) {
        i = (i + 1) & 0xff;
        j = (j + S[i]) & 0xff;
        std::swap(S[i], S[j]);
        out[n] = static_cast<char>(static_cast<unsigned char>(data[n]) ^ S[(S[i] + S[j]) & 0xff]);
    }
    return out;
}

// ---------------------------------------------------------------- AES
namespace {

struct AesTables {
    uint8_t sbox[256];
    uint8_t inv[256];
    AesTables() {
        // Generate the S-box from its definition (multiplicative inverse in GF(2^8)
        // followed by the affine transform) rather than typing 256 constants.
        uint8_t p = 1, q = 1;
        auto rotl8 = [](uint8_t x, int s) { return static_cast<uint8_t>((x << s) | (x >> (8 - s))); };
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= static_cast<uint8_t>(q << 1);
            q ^= static_cast<uint8_t>(q << 2);
            q ^= static_cast<uint8_t>(q << 4);
            if (q & 0x80) q ^= 0x09;
            uint8_t x = static_cast<uint8_t>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
            sbox[p] = static_cast<uint8_t>(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = static_cast<uint8_t>(i);
    }
};

const AesTables& Tables() {
    static const AesTables t;
    return t;
}

inline uint8_t XTime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x >> 7) * 0x1b)); }
inline uint8_t GMul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = XTime(a);
        b >>= 1;
    }
    return r;
}

class Aes {
public:
    explicit Aes(const std::string& key) {
        const AesTables& T = Tables();
        int nk = static_cast<int>(key.size() / 4);
        rounds_ = nk + 6;
        int total = 4 * (rounds_ + 1);
        rk_.assign(total * 4, 0);
        for (int i = 0; i < nk * 4; ++i) rk_[i] = static_cast<uint8_t>(key[i]);
        uint8_t rcon = 1;
        for (int i = nk; i < total; ++i) {
            uint8_t t[4];
            for (int k = 0; k < 4; ++k) t[k] = rk_[(i - 1) * 4 + k];
            if (i % nk == 0) {
                uint8_t tmp = t[0];
                t[0] = static_cast<uint8_t>(T.sbox[t[1]] ^ rcon);
                t[1] = T.sbox[t[2]];
                t[2] = T.sbox[t[3]];
                t[3] = T.sbox[tmp];
                rcon = XTime(rcon);
            } else if (nk > 6 && i % nk == 4) {
                for (int k = 0; k < 4; ++k) t[k] = T.sbox[t[k]];
            }
            for (int k = 0; k < 4; ++k) rk_[i * 4 + k] = rk_[(i - nk) * 4 + k] ^ t[k];
        }
    }

    void Encrypt(uint8_t s[16]) const {
        const AesTables& T = Tables();
        AddRoundKey(s, 0);
        for (int r = 1; r <= rounds_; ++r) {
            for (int i = 0; i < 16; ++i) s[i] = T.sbox[s[i]];
            ShiftRows(s);
            if (r != rounds_) MixColumns(s);
            AddRoundKey(s, r);
        }
    }

    void Decrypt(uint8_t s[16]) const {
        const AesTables& T = Tables();
        AddRoundKey(s, rounds_);
        for (int r = rounds_ - 1; r >= 0; --r) {
            InvShiftRows(s);
            for (int i = 0; i < 16; ++i) s[i] = T.inv[s[i]];
            AddRoundKey(s, r);
            if (r != 0) InvMixColumns(s);
        }
    }

private:
    void AddRoundKey(uint8_t s[16], int r) const {
        for (int i = 0; i < 16; ++i) s[i] ^= rk_[r * 16 + i];
    }
    static void ShiftRows(uint8_t s[16]) {
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) t[r + 4 * c] = s[r + 4 * ((c + r) % 4)];
        std::memcpy(s, t, 16);
    }
    static void InvShiftRows(uint8_t s[16]) {
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) t[r + 4 * ((c + r) % 4)] = s[r + 4 * c];
        std::memcpy(s, t, 16);
    }
    static void MixColumns(uint8_t s[16]) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* a = s + 4 * c;
            uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = static_cast<uint8_t>(XTime(a0) ^ (XTime(a1) ^ a1) ^ a2 ^ a3);
            a[1] = static_cast<uint8_t>(a0 ^ XTime(a1) ^ (XTime(a2) ^ a2) ^ a3);
            a[2] = static_cast<uint8_t>(a0 ^ a1 ^ XTime(a2) ^ (XTime(a3) ^ a3));
            a[3] = static_cast<uint8_t>((XTime(a0) ^ a0) ^ a1 ^ a2 ^ XTime(a3));
        }
    }
    static void InvMixColumns(uint8_t s[16]) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* a = s + 4 * c;
            uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            a[0] = static_cast<uint8_t>(GMul(a0, 14) ^ GMul(a1, 11) ^ GMul(a2, 13) ^ GMul(a3, 9));
            a[1] = static_cast<uint8_t>(GMul(a0, 9) ^ GMul(a1, 14) ^ GMul(a2, 11) ^ GMul(a3, 13));
            a[2] = static_cast<uint8_t>(GMul(a0, 13) ^ GMul(a1, 9) ^ GMul(a2, 14) ^ GMul(a3, 11));
            a[3] = static_cast<uint8_t>(GMul(a0, 11) ^ GMul(a1, 13) ^ GMul(a2, 9) ^ GMul(a3, 14));
        }
    }

    int rounds_ = 0;
    std::vector<uint8_t> rk_;
};

}  // namespace

std::string AesCbcEncrypt(const std::string& key, const std::string& iv, const std::string& data) {
    if ((key.size() != 16 && key.size() != 32) || iv.size() != 16) return std::string();
    Aes aes(key);
    std::string out;
    size_t n = data.size() / 16 * 16;
    out.reserve(n);
    uint8_t prev[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off < n; off += 16) {
        uint8_t b[16];
        for (int i = 0; i < 16; ++i) b[i] = static_cast<uint8_t>(data[off + i]) ^ prev[i];
        aes.Encrypt(b);
        std::memcpy(prev, b, 16);
        out.append(reinterpret_cast<char*>(b), 16);
    }
    return out;
}

std::string AesCbcDecrypt(const std::string& key, const std::string& iv, const std::string& data) {
    if ((key.size() != 16 && key.size() != 32) || iv.size() != 16) return std::string();
    Aes aes(key);
    std::string out;
    size_t n = data.size() / 16 * 16;
    out.reserve(n);
    uint8_t prev[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t off = 0; off < n; off += 16) {
        uint8_t b[16], c[16];
        std::memcpy(c, data.data() + off, 16);
        std::memcpy(b, c, 16);
        aes.Decrypt(b);
        for (int i = 0; i < 16; ++i) b[i] ^= prev[i];
        std::memcpy(prev, c, 16);
        out.append(reinterpret_cast<char*>(b), 16);
    }
    return out;
}

}  // namespace crypto
}  // namespace pdfprops
