// Small DEFLATE decoder modelled on Mark Adler's public-domain "puff".
// Speed is not critical: it only decodes cross-reference streams, object
// streams and XMP metadata, never page content.
#include "Inflate.h"

#include <cstdint>

namespace pdfprops {

namespace {

const int kMaxBits = 15;

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[288];
};

class Inflater {
public:
    Inflater(const std::string& in, size_t start, std::string& out, size_t maxOut)
        : in_(reinterpret_cast<const unsigned char*>(in.data())), len_(in.size()), pos_(start),
          out_(out), maxOut_(maxOut) {}

    // Returns 0 on clean end, >0 if output limit was hit, <0 on error.
    int Run() {
        int last;
        do {
            last = Bits(1);
            int type = Bits(2);
            if (err_) return -1;
            int r;
            if (type == 0)
                r = Stored();
            else if (type == 1)
                r = Fixed();
            else if (type == 2)
                r = Dynamic();
            else
                r = -1;
            if (r != 0) return r;
        } while (!last);
        return 0;
    }

private:
    int Bits(int need) {
        uint32_t val = bitbuf_;
        while (bitcnt_ < need) {
            if (pos_ >= len_) {
                err_ = true;
                return 0;
            }
            val |= static_cast<uint32_t>(in_[pos_++]) << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = val >> need;
        bitcnt_ -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    bool Put(unsigned char c) {
        if (out_.size() >= maxOut_) return false;
        out_.push_back(static_cast<char>(c));
        return true;
    }

    int Stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        if (pos_ + 4 > len_) return -1;
        unsigned len = in_[pos_] | (in_[pos_ + 1] << 8);
        unsigned nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if (len != (~nlen & 0xffff)) return -1;
        while (len--) {
            if (pos_ >= len_) return -1;
            if (!Put(in_[pos_++])) return 1;
        }
        return 0;
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= Bits(1);
            if (err_) return -1;
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    static int Construct(Huffman& h, const short* length, int n) {
        for (int len = 0; len <= kMaxBits; ++len) h.count[len] = 0;
        for (int symbol = 0; symbol < n; ++symbol) h.count[length[symbol]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        short offs[kMaxBits + 1];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = offs[len] + h.count[len];
        for (int symbol = 0; symbol < n; ++symbol)
            if (length[symbol] != 0) h.symbol[offs[length[symbol]]++] = static_cast<short>(symbol);
        return left;
    }

    int Codes(const Huffman& lencode, const Huffman& distcode) {
        static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                        33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        int symbol;
        do {
            symbol = Decode(lencode);
            if (symbol < 0) return -1;
            if (symbol < 256) {
                if (!Put(static_cast<unsigned char>(symbol))) return 1;
            } else if (symbol > 256) {
                symbol -= 257;
                if (symbol >= 29) return -1;
                int len = lbase[symbol] + Bits(lext[symbol]);
                int dsym = Decode(distcode);
                if (dsym < 0 || dsym >= 30) return -1;
                size_t dist = static_cast<size_t>(dbase[dsym] + Bits(dext[dsym]));
                if (err_) return -1;
                if (dist > out_.size()) return -1;
                while (len--) {
                    if (!Put(static_cast<unsigned char>(out_[out_.size() - dist]))) return 1;
                }
            }
        } while (symbol != 256);
        return 0;
    }

    struct FixedTables {
        Huffman lencode, distcode;
        FixedTables() {
            short lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            Construct(lencode, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            Construct(distcode, lengths, 30);
        }
    };

    int Fixed() {
        static const FixedTables t;  // thread-safe one-time init (C++11 magic statics)
        return Codes(t.lencode, t.distcode);
    }

    int Dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        int nlen = Bits(5) + 257;
        int ndist = Bits(5) + 1;
        int ncode = Bits(4) + 4;
        if (err_ || nlen > 286 || ndist > 30) return -1;
        int index;
        for (index = 0; index < ncode; ++index) lengths[order[index]] = static_cast<short>(Bits(3));
        for (; index < 19; ++index) lengths[order[index]] = 0;
        if (err_) return -1;
        Huffman lencode, distcode;
        if (Construct(lencode, lengths, 19) != 0) return -1;
        index = 0;
        while (index < nlen + ndist) {
            int symbol = Decode(lencode);
            if (symbol < 0) return -1;
            if (symbol < 16) {
                lengths[index++] = static_cast<short>(symbol);
            } else {
                int len = 0;
                if (symbol == 16) {
                    if (index == 0) return -1;
                    len = lengths[index - 1];
                    symbol = 3 + Bits(2);
                } else if (symbol == 17) {
                    symbol = 3 + Bits(3);
                } else {
                    symbol = 11 + Bits(7);
                }
                if (err_ || index + symbol > nlen + ndist) return -1;
                while (symbol--) lengths[index++] = static_cast<short>(len);
            }
        }
        if (lengths[256] == 0) return -1;
        int err = Construct(lencode, lengths, nlen);
        if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -1;
        err = Construct(distcode, lengths + nlen, ndist);
        if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -1;
        return Codes(lencode, distcode);
    }

    const unsigned char* in_;
    size_t len_;
    size_t pos_;
    uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;
    bool err_ = false;
    std::string& out_;
    size_t maxOut_;
};

}  // namespace

bool Inflate(const std::string& in, std::string& out, size_t maxOut) {
    out.clear();
    if (in.size() < 2) return false;
    size_t start = 0;
    unsigned cmf = static_cast<unsigned char>(in[0]);
    unsigned flg = static_cast<unsigned char>(in[1]);
    if ((cmf & 0x0f) == 8 && ((cmf << 8) | flg) % 31 == 0) {
        start = 2;
        if (flg & 0x20) start += 4;  // preset dictionary id (not used by PDF writers)
    }
    Inflater inf(in, start, out, maxOut);
    int r = inf.Run();
    if (r >= 0) return true;
    return !out.empty();  // keep partial output from damaged streams
}

}  // namespace pdfprops
