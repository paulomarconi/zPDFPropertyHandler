#include "PdfParser.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Inflate.h"
#include "PdfSecurity.h"

namespace pdfprops {

// ---------------------------------------------------------------------------
// Byte access
// ---------------------------------------------------------------------------

size_t MemorySource::ReadAt(uint64_t offset, void* buf, size_t len) {
    if (offset >= data_.size()) return 0;
    size_t n = static_cast<size_t>(std::min<uint64_t>(len, data_.size() - offset));
    memcpy(buf, data_.data() + offset, n);
    return n;
}

size_t ByteReader::Read(uint64_t pos, size_t n, std::string& out) {
    size_t got = 0;
    for (; got < n; ++got) {
        int c = At(pos + got);
        if (c < 0) break;
        out.push_back(static_cast<char>(c));
    }
    return got;
}

BlockCache::BlockCache(ByteSource& src, size_t blockSize, size_t maxBlocks)
    : src_(src), size_(src.Size()), blockSize_(blockSize), maxBlocks_(std::max<size_t>(maxBlocks, 2)) {}

const std::string* BlockCache::Block(uint64_t index) {
    if (index == lastIndex_ && last_) return last_;
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        if (it->first == index) {
            blocks_.splice(blocks_.begin(), blocks_, it);
            lastIndex_ = index;
            last_ = &blocks_.front().second;
            return last_;
        }
    }
    uint64_t off = index * blockSize_;
    if (off >= size_) return nullptr;
    std::string data(static_cast<size_t>(std::min<uint64_t>(blockSize_, size_ - off)), '\0');
    size_t got = 0;
    while (got < data.size()) {
        size_t n = src_.ReadAt(off + got, &data[got], data.size() - got);
        if (n == 0) break;
        got += n;
    }
    data.resize(got);
    if (blocks_.size() >= maxBlocks_) blocks_.pop_back();
    blocks_.emplace_front(index, std::move(data));
    lastIndex_ = index;
    last_ = &blocks_.front().second;
    return last_;
}

int BlockCache::At(uint64_t pos) {
    if (pos >= size_) return -1;
    const std::string* b = Block(pos / blockSize_);
    size_t i = static_cast<size_t>(pos % blockSize_);
    if (!b || i >= b->size()) return -1;
    return static_cast<unsigned char>((*b)[i]);
}

size_t BlockCache::Read(uint64_t pos, size_t n, std::string& out) {
    size_t total = 0;
    while (total < n && pos < size_) {
        const std::string* b = Block(pos / blockSize_);
        size_t i = static_cast<size_t>(pos % blockSize_);
        if (!b || i >= b->size()) break;
        size_t take = std::min(n - total, b->size() - i);
        out.append(*b, i, take);
        total += take;
        pos += take;
    }
    return total;
}

// ---------------------------------------------------------------------------
// PdfObj
// ---------------------------------------------------------------------------

namespace {
const PdfObj kNullObj;
}

const PdfObj& PdfObj::Get(const char* key) const {
    if (!IsDict()) return kNullObj;
    for (auto it = dict->rbegin(); it != dict->rend(); ++it)  // last duplicate wins
        if (it->first == key) return it->second;
    return kNullObj;
}

const PdfObj& PdfObj::At(size_t i) const {
    if (!IsArray() || i >= array->size()) return kNullObj;
    return (*array)[i];
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

namespace {
int HexVal(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

void Lexer::SkipWhitespace() {
    for (;;) {
        int c = r_.At(pos_);
        if (c < 0) return;
        if (IsWhite(c)) {
            ++pos_;
        } else if (c == '%') {
            while ((c = r_.At(pos_)) >= 0 && c != '\r' && c != '\n') ++pos_;
        } else {
            return;
        }
    }
}

void Lexer::ReadLiteralString(std::string& out) {
    // pos_ is just after '('
    int depth = 1;
    const uint64_t scanLimit = pos_ + 4 * PdfLimits::kMaxString;
    auto put = [&](int c) {
        if (out.size() < PdfLimits::kMaxString) out.push_back(static_cast<char>(c));
    };
    while (pos_ < scanLimit) {
        int c = r_.At(pos_++);
        if (c < 0) return;
        if (c == '(') {
            ++depth;
            put(c);
        } else if (c == ')') {
            if (--depth == 0) return;
            put(c);
        } else if (c == '\\') {
            c = r_.At(pos_++);
            if (c < 0) return;
            switch (c) {
                case 'n': put('\n'); break;
                case 'r': put('\r'); break;
                case 't': put('\t'); break;
                case 'b': put('\b'); break;
                case 'f': put('\f'); break;
                case '\r':
                    if (r_.At(pos_) == '\n') ++pos_;
                    break;
                case '\n': break;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0';
                        for (int i = 0; i < 2; ++i) {
                            int d = r_.At(pos_);
                            if (d < '0' || d > '7') break;
                            v = v * 8 + (d - '0');
                            ++pos_;
                        }
                        put(v & 0xff);
                    } else {
                        put(c);  // \( \) \\ and unknown escapes
                    }
            }
        } else if (c == '\r') {
            if (r_.At(pos_) == '\n') ++pos_;
            put('\n');
        } else {
            put(c);
        }
    }
}

void Lexer::ReadHexString(std::string& out) {
    // pos_ is just after '<'
    int hi = -1;
    const uint64_t scanLimit = pos_ + 4 * PdfLimits::kMaxString;
    while (pos_ < scanLimit) {
        int c = r_.At(pos_++);
        if (c < 0 || c == '>') break;
        int v = HexVal(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            if (out.size() < PdfLimits::kMaxString) out.push_back(static_cast<char>(hi * 16 + v));
            hi = -1;
        }
    }
    if (hi >= 0 && out.size() < PdfLimits::kMaxString) out.push_back(static_cast<char>(hi * 16));
}

void Lexer::ReadName(std::string& out) {
    // pos_ is just after '/'
    for (;;) {
        int c = r_.At(pos_);
        if (c < 0 || IsWhite(c) || IsDelim(c)) return;
        ++pos_;
        if (c == '#') {
            int h1 = HexVal(r_.At(pos_)), h2 = HexVal(r_.At(pos_ + 1));
            if (h1 >= 0 && h2 >= 0) {
                c = h1 * 16 + h2;
                pos_ += 2;
            }
        }
        if (out.size() < 4096) out.push_back(static_cast<char>(c));
    }
}

Lexer::Token Lexer::Next() {
    Token t;
    SkipWhitespace();
    t.pos = pos_;
    int c = r_.At(pos_);
    if (c < 0) {
        t.type = kEof;
        return t;
    }
    switch (c) {
        case '[': ++pos_; t.type = kArrayBegin; return t;
        case ']': ++pos_; t.type = kArrayEnd; return t;
        case '(':
            ++pos_;
            t.type = kString;
            ReadLiteralString(t.text);
            return t;
        case '<':
            ++pos_;
            if (r_.At(pos_) == '<') {
                ++pos_;
                t.type = kDictBegin;
            } else {
                t.type = kString;
                ReadHexString(t.text);
            }
            return t;
        case '>':
            ++pos_;
            if (r_.At(pos_) == '>') {
                ++pos_;
                t.type = kDictEnd;
            } else {
                t.type = kError;
            }
            return t;
        case '/':
            ++pos_;
            t.type = kName;
            ReadName(t.text);
            return t;
        case ')': case '{': case '}':
            ++pos_;
            t.type = kKeyword;
            t.text = static_cast<char>(c);
            return t;
        default: break;
    }
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        std::string s;
        while ((c = r_.At(pos_)) >= 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
            if (s.size() < 64) s.push_back(static_cast<char>(c));
            ++pos_;
        }
        bool real = s.find('.') != std::string::npos || s.size() > 18;
        // Tolerate junk like "--5" or "1.2.3": take the longest sensible prefix.
        size_t start = 0;
        while (start + 1 < s.size() && (s[start] == '+' || s[start] == '-') &&
               (s[start + 1] == '+' || s[start + 1] == '-'))
            ++start;
        const char* p = s.c_str() + start;
        if (real) {
            t.type = kReal;
            t.real = strtod(p, nullptr);
        } else {
            t.type = kInt;
            t.integer = strtoll(p, nullptr, 10);
        }
        return t;
    }
    t.type = kKeyword;
    while ((c = r_.At(pos_)) >= 0 && !IsWhite(c) && !IsDelim(c)) {
        if (t.text.size() < 256) t.text.push_back(static_cast<char>(c));
        ++pos_;
    }
    if (t.text.empty()) {  // should not happen; make progress anyway
        ++pos_;
        t.type = kError;
    }
    return t;
}

bool Lexer::ParseObject(PdfObj& obj, int depth) {
    Token t = Next();
    return ParseFromToken(t, obj, depth);
}

bool Lexer::ParseFromToken(const Token& t, PdfObj& obj, int depth) {
    obj = PdfObj();
    if (depth > PdfLimits::kMaxDepth) return false;
    switch (t.type) {
        case kInt: {
            uint64_t save = pos_;
            Token t2 = Next();
            if (t2.type == kInt && t.integer >= 0 && t2.integer >= 0 && t.integer < (1ll << 32)) {
                Token t3 = Next();
                if (t3.type == kKeyword && t3.text == "R") {
                    obj.type = PdfType::Ref;
                    obj.num = static_cast<uint32_t>(t.integer);
                    obj.gen = static_cast<uint32_t>(t2.integer & 0xffffffff);
                    return true;
                }
            }
            pos_ = save;
            obj.type = PdfType::Int;
            obj.integer = t.integer;
            return true;
        }
        case kReal:
            obj.type = PdfType::Real;
            obj.real = t.real;
            return true;
        case kString:
            obj.type = PdfType::String;
            obj.str = t.text;
            return true;
        case kName:
            obj.type = PdfType::Name;
            obj.str = t.text;
            return true;
        case kKeyword:
            if (t.text == "true" || t.text == "false") {
                obj.type = PdfType::Bool;
                obj.boolean = t.text == "true";
                return true;
            }
            if (t.text == "null") return true;
            return false;
        case kArrayBegin: {
            obj.type = PdfType::Array;
            obj.array = std::make_shared<PdfArray>();
            for (;;) {
                uint64_t before = pos_;
                Token it = Next();
                if (it.type == kArrayEnd || it.type == kEof) break;
                if (it.type == kDictEnd || (it.type == kKeyword && (it.text == "endobj" || it.text == "stream" ||
                                                                    it.text == "obj" || it.text == "trailer"))) {
                    pos_ = before;  // unterminated array: leave the token for the caller
                    break;
                }
                PdfObj item;
                if (!ParseFromToken(it, item, depth + 1) && it.type != kKeyword && it.type != kError) break;
                if (obj.array->size() < PdfLimits::kMaxArray) obj.array->push_back(std::move(item));
            }
            return true;
        }
        case kDictBegin: {
            obj.type = PdfType::Dict;
            obj.dict = std::make_shared<PdfDict>();
            for (;;) {
                uint64_t before = pos_;
                Token k = Next();
                if (k.type == kDictEnd || k.type == kEof) break;
                if (k.type == kKeyword && (k.text == "endobj" || k.text == "stream" || k.text == "obj" ||
                                           k.text == "trailer" || k.text == "startxref" || k.text == "xref")) {
                    pos_ = before;
                    break;
                }
                if (k.type != kName) continue;  // skip garbage
                uint64_t valPos = pos_;
                Token v = Next();
                if (v.type == kDictEnd) {
                    if (obj.dict->size() < PdfLimits::kMaxDict) obj.dict->emplace_back(k.text, PdfObj());
                    break;
                }
                PdfObj val;
                if (!ParseFromToken(v, val, depth + 1)) {
                    if (v.type == kKeyword && (v.text == "endobj" || v.text == "stream")) {
                        pos_ = valPos;
                        break;
                    }
                    continue;
                }
                if (obj.dict->size() < PdfLimits::kMaxDict) obj.dict->emplace_back(std::move(k.text), std::move(val));
            }
            // Stream data follows?
            uint64_t save = pos_;
            Token s = Next();
            if (s.type == kKeyword && s.text == "stream") {
                uint64_t p = pos_;
                while (r_.At(p) == ' ') ++p;  // tolerate trailing spaces
                if (r_.At(p) == '\r') {
                    ++p;
                    if (r_.At(p) == '\n') ++p;
                } else if (r_.At(p) == '\n') {
                    ++p;
                }
                obj.type = PdfType::Stream;
                obj.streamPos = p;
                pos_ = p;
            } else {
                pos_ = save;
            }
            return true;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

namespace {

int64_t IntParam(PdfDocument& doc, const PdfObj& parms, const char* key, int64_t def) {
    if (!parms.IsDict()) return def;
    PdfObj v = doc.GetResolved(parms, key);
    return v.IsInt() ? v.integer : def;
}

bool ApplyPredictor(PdfDocument& doc, const PdfObj& parms, std::string& data) {
    int64_t pred = IntParam(doc, parms, "Predictor", 1);
    if (pred <= 1) return true;
    int64_t colors = IntParam(doc, parms, "Colors", 1);
    int64_t bpc = IntParam(doc, parms, "BitsPerComponent", 8);
    int64_t cols = IntParam(doc, parms, "Columns", 1);
    if (colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || cols < 1 || cols > (1 << 24)) return false;
    size_t bpp = static_cast<size_t>(std::max<int64_t>(1, (colors * bpc + 7) / 8));
    size_t rowLen = static_cast<size_t>((colors * bpc * cols + 7) / 8);
    if (pred == 2) {
        if (bpc != 8) return true;  // rare; leave as is
        for (size_t row = 0; row + rowLen <= data.size(); row += rowLen)
            for (size_t i = bpp; i < rowLen; ++i) data[row + i] = static_cast<char>(data[row + i] + data[row + i - bpp]);
        return true;
    }
    if (pred < 10) return false;
    std::string out;
    out.reserve(data.size());
    std::vector<unsigned char> prev(rowLen, 0), cur(rowLen);
    size_t pos = 0;
    while (pos < data.size()) {
        int type = static_cast<unsigned char>(data[pos++]);
        size_t n = std::min(rowLen, data.size() - pos);
        for (size_t i = 0; i < rowLen; ++i) cur[i] = i < n ? static_cast<unsigned char>(data[pos + i]) : 0;
        pos += n;
        for (size_t i = 0; i < rowLen; ++i) {
            unsigned a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            switch (type) {
                case 1: cur[i] = static_cast<unsigned char>(cur[i] + a); break;
                case 2: cur[i] = static_cast<unsigned char>(cur[i] + b); break;
                case 3: cur[i] = static_cast<unsigned char>(cur[i] + (a + b) / 2); break;
                case 4: {
                    int p = static_cast<int>(a + b) - static_cast<int>(c);
                    int pa = std::abs(p - static_cast<int>(a)), pb = std::abs(p - static_cast<int>(b)),
                        pc = std::abs(p - static_cast<int>(c));
                    unsigned pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    cur[i] = static_cast<unsigned char>(cur[i] + pr);
                    break;
                }
                default: break;
            }
        }
        out.append(reinterpret_cast<const char*>(cur.data()), n);
        prev.swap(cur);
    }
    data.swap(out);
    return true;
}

bool AsciiHexDecode(const std::string& in, std::string& out) {
    int hi = -1;
    for (char ch : in) {
        if (ch == '>') break;
        int v = HexVal(static_cast<unsigned char>(ch));
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<char>(hi * 16 + v));
            hi = -1;
        }
    }
    if (hi >= 0) out.push_back(static_cast<char>(hi * 16));
    return true;
}

bool Ascii85Decode(const std::string& in, std::string& out) {
    size_t i = 0;
    while (i < in.size() && Lexer::IsWhite(static_cast<unsigned char>(in[i]))) ++i;
    if (in.compare(i, 2, "<~") == 0) i += 2;
    uint32_t tuple = 0;
    int count = 0;
    for (; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '~') break;
        if (Lexer::IsWhite(c)) continue;
        if (c == 'z' && count == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') return !out.empty();
        tuple = tuple * 85 + (c - '!');
        if (++count == 5) {
            for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((tuple >> s) & 0xff));
            tuple = 0;
            count = 0;
        }
    }
    if (count > 1) {
        for (int k = count; k < 5; ++k) tuple = tuple * 85 + 84;
        for (int k = 0; k < count - 1; ++k) out.push_back(static_cast<char>((tuple >> (24 - 8 * k)) & 0xff));
    }
    return true;
}

bool LzwDecode(const std::string& in, std::string& out, size_t maxOut, int earlyChange) {
    std::vector<std::string> table;
    auto reset = [&]() {
        table.clear();
        for (int i = 0; i < 256; ++i) table.emplace_back(1, static_cast<char>(i));
        table.emplace_back();  // 256 clear
        table.emplace_back();  // 257 EOD
    };
    reset();
    int codeLen = 9;
    uint32_t buf = 0;
    int bits = 0;
    size_t pos = 0;
    std::string prev;
    bool havePrev = false;
    for (;;) {
        while (bits < codeLen) {
            if (pos >= in.size()) return true;
            buf = (buf << 8) | static_cast<unsigned char>(in[pos++]);
            bits += 8;
        }
        int code = static_cast<int>((buf >> (bits - codeLen)) & ((1u << codeLen) - 1));
        bits -= codeLen;
        if (code == 256) {
            reset();
            codeLen = 9;
            havePrev = false;
            continue;
        }
        if (code == 257) return true;
        std::string entry;
        if (code < static_cast<int>(table.size()) && code != 256 && code != 257) {
            entry = table[code];
        } else if (code == static_cast<int>(table.size()) && havePrev) {
            entry = prev + prev[0];
        } else {
            return !out.empty();
        }
        if (out.size() + entry.size() > maxOut) {
            out.append(entry, 0, maxOut - out.size());
            return true;
        }
        out += entry;
        if (havePrev && table.size() < 4096) table.push_back(prev + entry[0]);
        prev = entry;
        havePrev = true;
        size_t next = table.size() + static_cast<size_t>(earlyChange);
        codeLen = next >= 2048 ? 12 : next >= 1024 ? 11 : next >= 512 ? 10 : 9;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// PdfDocument
// ---------------------------------------------------------------------------

PdfDocument::PdfDocument(ByteSource& src) : src_(src), cache_(src) {}
PdfDocument::~PdfDocument() = default;

bool PdfDocument::CanDecrypt() const { return !encrypted_ || (sec_ && sec_->Authenticated()); }

std::string PdfDocument::EncryptionInfo() const {
    if (!encrypted_) return std::string();
    return sec_ ? sec_->Info() : "unsupported security handler";
}

bool PdfDocument::ReadHeader() {
    std::string head;
    cache_.Read(0, 1024, head);
    size_t p = head.find("%PDF-");
    if (p == std::string::npos) return false;
    base_ = p;
    size_t e = p + 5;
    while (e < head.size() && ((head[e] >= '0' && head[e] <= '9') || head[e] == '.')) ++e;
    headerVersion_ = head.substr(p + 5, e - p - 5);
    return true;
}

bool PdfDocument::FindStartXref(uint64_t& off) {
    uint64_t size = cache_.Size();
    uint64_t start = size > 4096 ? size - 4096 : 0;
    std::string tail;
    cache_.Read(start, static_cast<size_t>(size - start), tail);
    size_t p = tail.rfind("startxref");
    if (p == std::string::npos) return false;
    StringReader sr(tail);
    Lexer lx(sr, p + 9);
    Lexer::Token t = lx.Next();
    if (t.type != Lexer::kInt || t.integer <= 0) return false;
    off = static_cast<uint64_t>(t.integer);
    return true;
}

void PdfDocument::AddEntry(uint32_t num, const XrefEntry& e) {
    if (num > 16u * 1024 * 1024) return;
    xref_.emplace(num, e);  // does not overwrite: newer sections are loaded first
}

void PdfDocument::MergeTrailer(const PdfObj& t) {
    if (!t.IsDict()) return;
    if (!trailer_.IsDict()) {
        trailer_.type = PdfType::Dict;
        trailer_.dict = std::make_shared<PdfDict>();
    }
    for (const auto& kv : *t.dict)
        if (trailer_.Get(kv.first.c_str()).IsNull()) trailer_.dict->push_back(kv);
}

bool PdfDocument::LoadXrefTable(Lexer& lx, PdfObj& trailer) {
    for (int guard = 0; guard < 1000000; ++guard) {
        Lexer::Token t = lx.Next();
        if (t.type == Lexer::kEof) return false;
        if (t.type == Lexer::kKeyword && t.text == "trailer") {
            PdfObj d;
            lx.ParseObject(d);
            if (!d.IsDict()) return false;
            trailer = d;
            trailer.type = PdfType::Dict;
            return true;
        }
        if (t.type != Lexer::kInt) continue;
        Lexer::Token c = lx.Next();
        if (c.type != Lexer::kInt || t.integer < 0 || c.integer < 0 || c.integer > 16 * 1024 * 1024) continue;
        int64_t start = t.integer;
        for (int64_t i = 0; i < c.integer; ++i) {
            uint64_t save = lx.Pos();
            Lexer::Token a = lx.Next(), b = lx.Next(), k = lx.Next();
            if (a.type != Lexer::kInt || b.type != Lexer::kInt || k.type != Lexer::kKeyword ||
                (k.text != "n" && k.text != "f")) {
                lx.Seek(save);
                break;
            }
            // Common writer bug: first subsection says "1 N" but starts with object 0.
            if (i == 0 && start == 1 && k.text == "f" && b.integer == 65535) {
                start = 0;
                xrefRepaired_ = true;  // other readers may not apply this fix: don't build updates on it
            }
            if (k.text == "n" && a.integer > 0)
                AddEntry(static_cast<uint32_t>(start + i),
                         XrefEntry{1, static_cast<uint64_t>(a.integer), static_cast<uint32_t>(b.integer)});
        }
    }
    return false;
}

bool PdfDocument::LoadXrefStream(const PdfObj& stm) {
    std::string data;
    if (!ReadStream(stm, data, PdfLimits::kMaxStreamOut, false)) return false;
    PdfObj w = GetResolved(stm, "W");
    if (!w.IsArray() || w.Count() < 3) return false;
    int wd[3];
    for (int i = 0; i < 3; ++i) {
        PdfObj x = Resolve(w.At(i));
        wd[i] = x.IsInt() ? static_cast<int>(x.integer) : -1;
        if (wd[i] < 0 || wd[i] > 8) return false;
    }
    size_t rowLen = static_cast<size_t>(wd[0] + wd[1] + wd[2]);
    if (rowLen == 0) return false;
    PdfObj sizeObj = GetResolved(stm, "Size");
    int64_t size = sizeObj.IsInt() ? sizeObj.integer : 0;
    std::vector<int64_t> index;
    PdfObj idx = GetResolved(stm, "Index");
    if (idx.IsArray()) {
        for (size_t i = 0; i + 1 < idx.Count(); i += 2) {
            PdfObj a = Resolve(idx.At(i)), b = Resolve(idx.At(i + 1));
            if (!a.IsInt() || !b.IsInt()) break;
            index.push_back(a.integer);
            index.push_back(b.integer);
        }
    } else {
        index.push_back(0);
        index.push_back(size > 0 ? size : static_cast<int64_t>(data.size() / rowLen));
    }
    auto field = [&](size_t off, int width) {
        uint64_t v = 0;
        for (int i = 0; i < width; ++i) v = (v << 8) | static_cast<unsigned char>(data[off + i]);
        return v;
    };
    size_t pos = 0;
    for (size_t s = 0; s + 1 < index.size(); s += 2) {
        int64_t first = index[s], count = index[s + 1];
        if (first < 0 || count < 0) continue;
        for (int64_t i = 0; i < count; ++i, pos += rowLen) {
            if (pos + rowLen > data.size()) return true;
            uint64_t type = wd[0] ? field(pos, wd[0]) : 1;
            uint64_t f2 = field(pos + wd[0], wd[1]);
            uint64_t f3 = field(pos + wd[0] + wd[1], wd[2]);
            uint32_t num = static_cast<uint32_t>(first + i);
            if (type == 1 && f2 > 0)
                AddEntry(num, XrefEntry{1, f2, static_cast<uint32_t>(f3)});
            else if (type == 2)
                AddEntry(num, XrefEntry{2, f2, static_cast<uint32_t>(f3)});
        }
    }
    return true;
}

bool PdfDocument::LoadXrefAt(uint64_t off, PdfObj& trailer, bool& isStream) {
    if (off >= cache_.Size()) return false;
    Lexer lx(cache_, off);
    Lexer::Token t = lx.Next();
    if (t.type == Lexer::kKeyword && t.text == "xref") {
        isStream = false;
        return LoadXrefTable(lx, trailer);
    }
    if (t.type == Lexer::kInt) {
        Lexer::Token g = lx.Next(), o = lx.Next();
        if (g.type != Lexer::kInt || o.type != Lexer::kKeyword || o.text != "obj") return false;
        PdfObj stm;
        lx.ParseObject(stm);
        if (!stm.IsStream() || !stm.Get("Type").IsName("XRef")) return false;
        stm.num = static_cast<uint32_t>(t.integer);
        isStream = true;
        if (!LoadXrefStream(stm)) return false;
        trailer = stm;
        trailer.type = PdfType::Dict;
        return true;
    }
    return false;
}

bool PdfDocument::LoadXrefChain(uint64_t off) {
    std::set<uint64_t> visited;
    bool any = false;
    while (off > 0 && visited.size() < 512 && visited.insert(off).second) {
        PdfObj t;
        bool isStream = false;
        bool ok = LoadXrefAt(off, t, isStream);
        if (!any) {  // newest section: remember where it is, for incremental updates
            lastXrefExact_ = ok;
            lastXrefOffset_ = off;
            lastXrefIsStream_ = isStream;
        }
        if (!ok && base_ > 0) ok = LoadXrefAt(off + base_, t, isStream);
        if (!ok) {
            // Offset slightly wrong? Look for "xref" nearby.
            std::string win;
            uint64_t from = off > 64 ? off - 64 : 0;
            cache_.Read(from, 128, win);
            size_t p = win.find("xref");
            if (p != std::string::npos && (p == 0 || win[p - 1] != 't'))  // not "startxref"
                ok = LoadXrefAt(from + p, t, isStream);
        }
        if (!ok) break;
        any = true;
        MergeTrailer(t);
        if (!isStream) {
            PdfObj xs = t.Get("XRefStm");
            if (xs.IsInt() && xs.integer > 0) {
                PdfObj t2;
                bool s2 = false;
                LoadXrefAt(static_cast<uint64_t>(xs.integer), t2, s2);
            }
        }
        PdfObj prev = t.Get("Prev");
        if (!prev.IsInt() || prev.integer <= 0) break;
        off = static_cast<uint64_t>(prev.integer);
    }
    return any;
}

bool PdfDocument::ParseIndirectAt(uint64_t off, uint32_t expectNum, PdfObj& obj, uint32_t* numOut,
                                  uint32_t* genOut) {
    if (off >= cache_.Size()) return false;
    Lexer lx(cache_, off);
    Lexer::Token n = lx.Next(), g = lx.Next(), o = lx.Next();
    if (n.type != Lexer::kInt || g.type != Lexer::kInt || o.type != Lexer::kKeyword || o.text != "obj") return false;
    if (expectNum != UINT32_MAX && n.integer != expectNum) return false;
    if (!lx.ParseObject(obj)) {
        // "N G obj endobj" means null; anything else unparseable is an error
        return false;
    }
    if (numOut) *numOut = static_cast<uint32_t>(n.integer);
    if (genOut) *genOut = static_cast<uint32_t>(g.integer);
    return true;
}

PdfObj PdfDocument::GetFromObjStm(uint32_t stmNum, uint32_t index, uint32_t num) {
    std::shared_ptr<ObjStm> os;
    auto it = objStms_.find(stmNum);
    if (it != objStms_.end()) {
        os = it->second;
    } else {
        PdfObj s = GetObject(stmNum);
        if (!s.IsStream()) return PdfObj();
        os = std::make_shared<ObjStm>();
        if (!ReadStream(s, os->data, PdfLimits::kMaxStreamOut)) return PdfObj();
        PdfObj n = GetResolved(s, "N"), first = GetResolved(s, "First");
        if (!n.IsInt() || !first.IsInt() || n.integer < 0 || first.integer < 0) return PdfObj();
        os->first = static_cast<uint64_t>(first.integer);
        StringReader sr(os->data);
        Lexer lx(sr, 0);
        for (int64_t i = 0; i < n.integer && i < 1000000; ++i) {
            Lexer::Token a = lx.Next(), b = lx.Next();
            if (a.type != Lexer::kInt || b.type != Lexer::kInt) break;
            os->offsets.emplace_back(static_cast<uint32_t>(a.integer), static_cast<uint64_t>(b.integer));
        }
        if (objStms_.size() >= 4) objStms_.erase(objStms_.begin());
        objStms_[stmNum] = os;
    }
    uint64_t off = UINT64_MAX;
    if (index < os->offsets.size() && os->offsets[index].first == num) {
        off = os->offsets[index].second;
    } else {
        for (const auto& p : os->offsets)
            if (p.first == num) {
                off = p.second;
                break;
            }
    }
    if (off == UINT64_MAX) return PdfObj();
    StringReader sr(os->data);
    Lexer lx(sr, os->first + off);
    PdfObj obj;
    lx.ParseObject(obj);
    if (obj.type == PdfType::Stream) obj = PdfObj();  // streams cannot live in object streams
    return obj;
}

void PdfDocument::DecryptStrings(PdfObj& o, uint32_t num, uint32_t gen, int depth) {
    if (depth > PdfLimits::kMaxDepth) return;
    switch (o.type) {
        case PdfType::String: {
            std::string out;
            sec_->DecryptString(o.str, num, gen, out);
            o.str.swap(out);  // undecryptable strings become empty
            break;
        }
        case PdfType::Array:
            if (o.array)
                for (auto& x : *o.array) DecryptStrings(x, num, gen, depth + 1);
            break;
        case PdfType::Dict:
        case PdfType::Stream:
            if (o.dict)
                for (auto& kv : *o.dict) DecryptStrings(kv.second, num, gen, depth + 1);
            break;
        default: break;
    }
}

PdfObj PdfDocument::GetObject(uint32_t num) {
    auto c = objCache_.find(num);
    if (c != objCache_.end()) return c->second;
    if (resolving_.count(num) || resolving_.size() > 64) return PdfObj();
    auto it = xref_.find(num);
    if (it == xref_.end()) {
        // In a reconstructed file the object may live in an object stream not indexed yet.
        if (!reconstructed_ || objectsScanned_) return PdfObj();
        ScanObjects(false);
        it = xref_.find(num);
        if (it == xref_.end()) return PdfObj();
    }
    XrefEntry e = it->second;
    resolving_.insert(num);
    PdfObj obj;
    bool ok = false;
    if (e.type == 1) {
        uint32_t gen = 0;
        ok = ParseIndirectAt(e.offset, num, obj, nullptr, &gen);
        if (!ok && base_ > 0) ok = ParseIndirectAt(e.offset + base_, num, obj, nullptr, &gen);
        if (!ok && !reconstructed_) {
            resolving_.erase(num);
            Reconstruct();
            return GetObject(num);
        }
        if (ok) {
            if (obj.type == PdfType::Stream) {
                obj.num = num;
                obj.gen = gen;
            }
            if (sec_ && num != encryptNum_) DecryptStrings(obj, num, gen, 0);
        }
    } else if (e.type == 2) {
        obj = GetFromObjStm(static_cast<uint32_t>(e.offset), e.gen, num);
        ok = true;
    }
    resolving_.erase(num);
    if (ok) {
        if (objCache_.size() > 4096) objCache_.clear();
        objCache_[num] = obj;
    }
    return obj;
}

PdfObj PdfDocument::Resolve(const PdfObj& o) {
    PdfObj cur = o;
    for (int i = 0; i < 32 && cur.IsRef(); ++i) cur = GetObject(cur.num);
    if (cur.IsRef()) return PdfObj();
    return cur;
}

PdfObj PdfDocument::Root() { return GetResolved(trailer_, "Root"); }

uint32_t PdfDocument::MaxObjectNumber() const {
    uint32_t m = 0;
    for (const auto& kv : xref_) m = std::max(m, kv.first);
    return m;
}

bool PdfDocument::CleanXref() const { return lastXrefExact_ && !reconstructed_ && !xrefRepaired_ && base_ == 0; }

bool PdfDocument::StreamRawRange(const PdfObj& stream, uint64_t& start, uint64_t& len) {
    uint64_t size = cache_.Size();
    start = stream.streamPos;
    if (start > size) return false;
    PdfObj l = GetResolved(stream, "Length");
    auto endstreamAt = [&](uint64_t p) {
        std::string s;
        cache_.Read(p, 32, s);
        size_t i = 0;
        while (i < s.size() && Lexer::IsWhite(static_cast<unsigned char>(s[i]))) ++i;
        return s.compare(i, 9, "endstream") == 0;
    };
    if (l.IsInt() && l.integer >= 0 && start + static_cast<uint64_t>(l.integer) <= size &&
        endstreamAt(start + static_cast<uint64_t>(l.integer))) {
        len = static_cast<uint64_t>(l.integer);
        return true;
    }
    // Length missing or wrong: search for "endstream".
    const size_t chunk = 64 * 1024;
    uint64_t pos = start;
    while (pos < size && pos - start < PdfLimits::kMaxRawStream) {
        std::string buf;
        cache_.Read(pos, chunk + 16, buf);
        size_t f = buf.find("endstream");
        if (f != std::string::npos) {
            uint64_t end = pos + f;
            // trim the EOL before "endstream"
            if (end > start && cache_.At(end - 1) == '\n') --end;
            if (end > start && cache_.At(end - 1) == '\r') --end;
            len = end - start;
            return true;
        }
        if (buf.size() <= 16) break;
        pos += chunk;
    }
    if (l.IsInt() && l.integer >= 0) {
        len = std::min<uint64_t>(static_cast<uint64_t>(l.integer), size - start);
        return true;
    }
    return false;
}

bool PdfDocument::ReadStream(const PdfObj& stream, std::string& out, size_t maxOut, bool decrypt) {
    out.clear();
    if (!stream.IsStream()) return false;
    uint64_t start = 0, len = 0;
    if (!StreamRawRange(stream, start, len) || len > PdfLimits::kMaxRawStream) return false;
    std::string data;
    cache_.Read(start, static_cast<size_t>(len), data);
    if (decrypt && sec_ && stream.num != encryptNum_) {
        std::string dec;
        if (!sec_->DecryptStream(data, stream.num, stream.gen, stream, *this, dec)) return false;
        data.swap(dec);
    }
    PdfObj filter = GetResolved(stream, "Filter");
    PdfObj parms = GetResolved(stream, "DecodeParms");
    if (parms.IsNull()) parms = GetResolved(stream, "DP");
    std::vector<std::string> names;
    std::vector<PdfObj> parmList;
    if (filter.IsName()) {
        names.push_back(filter.str);
        parmList.push_back(parms.IsArray() ? Resolve(parms.At(0)) : parms);
    } else if (filter.IsArray()) {
        for (size_t i = 0; i < filter.Count() && i < 16; ++i) {
            PdfObj f = Resolve(filter.At(i));
            if (!f.IsName()) return false;
            names.push_back(f.str);
            parmList.push_back(parms.IsArray() ? Resolve(parms.At(i)) : PdfObj());
        }
    }
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        std::string dec;
        if (n == "FlateDecode" || n == "Fl") {
            if (!Inflate(data, dec, maxOut)) return false;
            if (!ApplyPredictor(*this, parmList[i], dec)) return false;
        } else if (n == "LZWDecode" || n == "LZW") {
            int64_t early = IntParam(*this, parmList[i], "EarlyChange", 1);
            if (!LzwDecode(data, dec, maxOut, early ? 1 : 0)) return false;
            if (!ApplyPredictor(*this, parmList[i], dec)) return false;
        } else if (n == "ASCIIHexDecode" || n == "AHx") {
            AsciiHexDecode(data, dec);
        } else if (n == "ASCII85Decode" || n == "A85") {
            Ascii85Decode(data, dec);
        } else if (n == "Crypt") {
            dec.swap(data);  // handled by the security handler
        } else {
            return false;  // image codecs etc. are of no interest here
        }
        data.swap(dec);
    }
    if (data.size() > maxOut) data.resize(maxOut);
    out.swap(data);
    return true;
}

void PdfDocument::SetupEncryption() {
    PdfObj encRef = trailer_.Get("Encrypt");
    if (encRef.IsNull()) return;
    encrypted_ = true;
    if (encRef.IsRef()) encryptNum_ = encRef.num;
    PdfObj enc = Resolve(encRef);
    std::string id0;
    PdfObj id = GetResolved(trailer_, "ID");
    if (id.IsArray()) {
        PdfObj first = Resolve(id.At(0));
        if (first.IsString()) id0 = first.str;
    }
    sec_ = SecurityHandler::Create(*this, enc, id0);
    // Anything read so far was read without decryption.
    objCache_.clear();
    objStms_.clear();
}

bool PdfDocument::Reconstruct() {
    if (reconstructed_) return false;
    reconstructed_ = true;
    objCache_.clear();
    objStms_.clear();

    // Phase 1: scan for "N G obj" and "trailer".
    uint64_t size = cache_.Size();
    const size_t chunk = 1 << 20, overlap = 64;
    std::vector<uint64_t> trailerPos;
    std::vector<std::pair<uint32_t, uint64_t>> found;
    const uint64_t scanEnd = std::min<uint64_t>(size, PdfLimits::kMaxReconstructScan);
    for (uint64_t base = 0; base < scanEnd; base += chunk) {
        std::string buf(static_cast<size_t>(std::min<uint64_t>(chunk + overlap, size - base)), '\0');
        size_t got = 0;
        while (got < buf.size()) {
            size_t n = src_.ReadAt(base + got, &buf[got], buf.size() - got);
            if (n == 0) break;
            got += n;
        }
        buf.resize(got);
        size_t limit = std::min(buf.size(), chunk);  // matches starting in the overlap belong to the next chunk
        for (size_t p = buf.find("obj"); p != std::string::npos && p < limit; p = buf.find("obj", p + 3)) {
            if (p + 3 < buf.size() && !Lexer::IsWhite(static_cast<unsigned char>(buf[p + 3])) &&
                !Lexer::IsDelim(static_cast<unsigned char>(buf[p + 3])))
                continue;
            size_t q = p;
            while (q > 0 && Lexer::IsWhite(static_cast<unsigned char>(buf[q - 1]))) --q;
            size_t genEnd = q;
            while (q > 0 && buf[q - 1] >= '0' && buf[q - 1] <= '9') --q;
            if (q == genEnd || genEnd - q > 5) continue;
            size_t ws = q;
            while (q > 0 && Lexer::IsWhite(static_cast<unsigned char>(buf[q - 1]))) --q;
            if (q == ws) continue;
            size_t numEnd = q;
            while (q > 0 && buf[q - 1] >= '0' && buf[q - 1] <= '9') --q;
            if (q == numEnd || numEnd - q > 8) continue;
            if (q > 0 && !Lexer::IsWhite(static_cast<unsigned char>(buf[q - 1])) &&
                !Lexer::IsDelim(static_cast<unsigned char>(buf[q - 1])))
                continue;
            if (q == 0 && base > 0) continue;  // number may continue in the previous chunk; rare, skip
            uint32_t num = static_cast<uint32_t>(strtoul(buf.substr(q, numEnd - q).c_str(), nullptr, 10));
            found.emplace_back(num, base + q);
        }
        for (size_t p = buf.find("trailer"); p != std::string::npos && p < limit; p = buf.find("trailer", p + 7))
            trailerPos.push_back(base + p + 7);
    }
    for (const auto& f : found) {
        XrefEntry e{1, f.second, 0};
        xref_[f.first] = e;  // later definitions (incremental updates) win
    }
    // Trailers: newest first, so MergeTrailer keeps the newest values; drop a Root that doesn't resolve.
    PdfObj root = Root();
    if (!root.IsDict() && trailer_.IsDict()) {
        auto& d = *trailer_.dict;
        d.erase(std::remove_if(d.begin(), d.end(), [](const std::pair<std::string, PdfObj>& kv) {
                    return kv.first == "Root";
                }),
                d.end());
    }
    for (auto it = trailerPos.rbegin(); it != trailerPos.rend(); ++it) {
        Lexer lx(cache_, *it);
        PdfObj t;
        lx.ParseObject(t);
        if (t.IsDict()) {
            t.type = PdfType::Dict;
            MergeTrailer(t);
        }
    }
    if (!encrypted_ && !trailer_.Get("Encrypt").IsNull()) SetupEncryption();

    // Phase 2 parses every object, so run it now only if no trailer gave a usable Root.
    // Otherwise object stream members are indexed lazily, on the first lookup miss (GetObject).
    if (!Root().IsDict()) ScanObjects(true);
    objCache_.clear();
    return Root().IsDict();
}

void PdfDocument::ScanObjects(bool findCatalog) {
    if (objectsScanned_) return;
    objectsScanned_ = true;
    bool needRoot = findCatalog && !Root().IsDict();
    bool needInfo = findCatalog && !GetResolved(trailer_, "Info").IsDict();
    uint32_t catalog = 0, info = 0;
    uint64_t catalogOff = 0, infoOff = 0;
    std::vector<std::pair<uint32_t, XrefEntry>> entries(xref_.begin(), xref_.end());
    if (entries.size() > PdfLimits::kMaxScanObjects) entries.resize(PdfLimits::kMaxScanObjects);
    for (const auto& kv : entries) {
        if (kv.second.type != 1) continue;
        PdfObj obj;
        if (!ParseIndirectAt(kv.second.offset, kv.first, obj)) continue;
        if (!obj.IsDict()) continue;
        PdfObj type = obj.Get("Type");
        if (obj.IsStream() && type.IsName("ObjStm")) {
            PdfObj s = GetObject(kv.first);
            std::string data;
            PdfObj n = GetResolved(s, "N");
            if (!n.IsInt() || !ReadStream(s, data, PdfLimits::kMaxStreamOut)) continue;
            StringReader sr(data);
            Lexer lx(sr, 0);
            for (int64_t i = 0; i < n.integer && i < 1000000; ++i) {
                Lexer::Token a = lx.Next(), b = lx.Next();
                if (a.type != Lexer::kInt || b.type != Lexer::kInt) break;
                AddEntry(static_cast<uint32_t>(a.integer), XrefEntry{2, kv.first, static_cast<uint32_t>(i)});
            }
        } else if (needRoot && type.IsName("Catalog") && kv.second.offset >= catalogOff) {
            catalog = kv.first;
            catalogOff = kv.second.offset;
        } else if (needInfo && !obj.IsStream() && type.IsNull() && kv.second.offset >= infoOff &&
                   (!obj.Get("Producer").IsNull() || !obj.Get("CreationDate").IsNull() ||
                    !obj.Get("Creator").IsNull() || !obj.Get("ModDate").IsNull())) {
            info = kv.first;
            infoOff = kv.second.offset;
        }
    }
    auto setTrailerRef = [&](const char* key, uint32_t num) {
        PdfObj ref;
        ref.type = PdfType::Ref;
        ref.num = num;
        if (!trailer_.IsDict()) {
            trailer_.type = PdfType::Dict;
            trailer_.dict = std::make_shared<PdfDict>();
        }
        trailer_.dict->emplace_back(key, ref);
    };
    if (needRoot && catalog) setTrailerRef("Root", catalog);
    if (needInfo && info) setTrailerRef("Info", info);
    objCache_.clear();
}

bool PdfDocument::Load() {
    if (!ReadHeader()) return false;  // not a PDF: don't scan the whole file
    uint64_t sx = 0;
    bool ok = FindStartXref(sx) && LoadXrefChain(sx);
    if (ok) {
        SetupEncryption();
        if (!Root().IsDict()) ok = false;
    }
    if (!ok) Reconstruct();
    return Root().IsDict() || GetResolved(trailer_, "Info").IsDict();
}

}  // namespace pdfprops
