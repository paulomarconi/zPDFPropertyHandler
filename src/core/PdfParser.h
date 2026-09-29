// Tolerant, read-only PDF object parser: just enough of PDF to find the
// document catalog, the Info dictionary and the XMP metadata stream.
// Portable C++17, no platform headers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdfprops {

// Random-access byte source (an IStream in the DLL, a FILE* in the CLI).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t Size() = 0;
    // Reads up to len bytes at offset; returns the number of bytes read.
    virtual size_t ReadAt(uint64_t offset, void* buf, size_t len) = 0;
};

class MemorySource : public ByteSource {
public:
    explicit MemorySource(std::string data) : data_(std::move(data)) {}
    uint64_t Size() override { return data_.size(); }
    size_t ReadAt(uint64_t offset, void* buf, size_t len) override;

private:
    std::string data_;
};

// Byte-level access used by the lexer.
class ByteReader {
public:
    virtual ~ByteReader() = default;
    virtual uint64_t Size() const = 0;
    virtual int At(uint64_t pos) = 0;  // -1 past the end
    // Appends up to n bytes starting at pos to out; returns bytes appended.
    virtual size_t Read(uint64_t pos, size_t n, std::string& out);
};

// Small LRU cache of fixed-size blocks over a ByteSource.
class BlockCache : public ByteReader {
public:
    explicit BlockCache(ByteSource& src, size_t blockSize = 64 * 1024, size_t maxBlocks = 4);
    uint64_t Size() const override { return size_; }
    int At(uint64_t pos) override;
    size_t Read(uint64_t pos, size_t n, std::string& out) override;

private:
    const std::string* Block(uint64_t index);

    ByteSource& src_;
    uint64_t size_;
    size_t blockSize_;
    size_t maxBlocks_;
    std::list<std::pair<uint64_t, std::string>> blocks_;  // front = most recently used
    uint64_t lastIndex_ = UINT64_MAX;
    const std::string* last_ = nullptr;
};

class StringReader : public ByteReader {
public:
    explicit StringReader(const std::string& s) : s_(s) {}
    uint64_t Size() const override { return s_.size(); }
    int At(uint64_t pos) override { return pos < s_.size() ? static_cast<unsigned char>(s_[pos]) : -1; }

private:
    const std::string& s_;
};

enum class PdfType { Null, Bool, Int, Real, String, Name, Array, Dict, Ref, Stream };

struct PdfObj;
using PdfArray = std::vector<PdfObj>;
using PdfDict = std::vector<std::pair<std::string, PdfObj>>;

struct PdfObj {
    PdfType type = PdfType::Null;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0;
    std::string str;  // String bytes or Name (without '/', #xx decoded)
    std::shared_ptr<PdfArray> array;
    std::shared_ptr<PdfDict> dict;  // Dict and Stream
    uint32_t num = 0, gen = 0;      // Ref target; for Stream: owning object
    uint64_t streamPos = 0;         // Stream: offset of the first data byte

    bool IsNull() const { return type == PdfType::Null; }
    bool IsInt() const { return type == PdfType::Int; }
    bool IsNumber() const { return type == PdfType::Int || type == PdfType::Real; }
    bool IsString() const { return type == PdfType::String; }
    bool IsName() const { return type == PdfType::Name; }
    bool IsName(const char* n) const { return type == PdfType::Name && str == n; }
    bool IsArray() const { return type == PdfType::Array && array; }
    bool IsDict() const { return (type == PdfType::Dict || type == PdfType::Stream) && dict; }
    bool IsStream() const { return type == PdfType::Stream && dict; }
    bool IsRef() const { return type == PdfType::Ref; }
    double Number() const { return type == PdfType::Real ? real : static_cast<double>(integer); }

    // Dictionary lookup (no reference resolution). Returns a Null object if absent.
    const PdfObj& Get(const char* key) const;
    size_t Count() const { return IsArray() ? array->size() : 0; }
    const PdfObj& At(size_t i) const;

    static PdfObj MakeInt(int64_t v) { PdfObj o; o.type = PdfType::Int; o.integer = v; return o; }
};

// Limits guarding against malicious or broken files.
struct PdfLimits {
    static constexpr int kMaxDepth = 64;
    static constexpr size_t kMaxString = 1 << 20;
    static constexpr size_t kMaxArray = 1 << 20;
    static constexpr size_t kMaxDict = 1 << 16;
    static constexpr size_t kMaxStreamOut = 64u << 20;  // xref / object streams
    static constexpr size_t kMaxRawStream = 256u << 20;
    static constexpr uint64_t kMaxReconstructScan = 256u << 20;  // bytes scanned when rebuilding a broken xref
    static constexpr size_t kMaxScanObjects = 200000;            // objects parsed when looking for a Catalog
};

class Lexer {
public:
    enum TokType { kEof, kInt, kReal, kString, kName, kKeyword, kArrayBegin, kArrayEnd, kDictBegin, kDictEnd, kError };
    struct Token {
        TokType type = kEof;
        int64_t integer = 0;
        double real = 0;
        std::string text;  // string bytes, name, or keyword
        uint64_t pos = 0;
    };

    Lexer(ByteReader& r, uint64_t pos) : r_(r), pos_(pos) {}
    Token Next();
    uint64_t Pos() const { return pos_; }
    void Seek(uint64_t pos) { pos_ = pos; }
    void SkipWhitespace();

    // Parses one object. Handles "N G R" references and "stream" after a dictionary.
    // Returns false on syntax error (obj is then Null).
    bool ParseObject(PdfObj& obj, int depth = 0);

    static bool IsWhite(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
    static bool IsDelim(int c) {
        return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
               c == '/' || c == '%';
    }

private:
    bool ParseFromToken(const Token& t, PdfObj& obj, int depth);
    void ReadLiteralString(std::string& out);
    void ReadHexString(std::string& out);
    void ReadName(std::string& out);

    ByteReader& r_;
    uint64_t pos_;
};

struct XrefEntry {
    uint8_t type = 0;      // 1 = in file at offset, 2 = in object stream
    uint64_t offset = 0;   // type 1: byte offset; type 2: object stream number
    uint32_t gen = 0;      // type 1: generation; type 2: index within the stream
};

class SecurityHandler;

class PdfDocument {
public:
    explicit PdfDocument(ByteSource& src);
    ~PdfDocument();

    // Reads header, cross-reference data and trailer; sets up decryption.
    // Returns false only if nothing usable was found.
    bool Load();

    const PdfObj& Trailer() const { return trailer_; }
    const std::string& HeaderVersion() const { return headerVersion_; }
    bool Encrypted() const { return encrypted_; }
    bool CanDecrypt() const;         // standard handler with the empty user password worked
    std::string EncryptionInfo() const;
    bool Reconstructed() const { return reconstructed_; }

    // For incremental updates: the newest xref section was found exactly at startxref, nothing was
    // reconstructed and there is no junk before the header (so offsets need no correction).
    bool CleanXref() const;
    uint64_t LastXrefOffset() const { return lastXrefOffset_; }
    bool LastXrefIsStream() const { return lastXrefIsStream_; }
    uint32_t MaxObjectNumber() const;

    PdfObj GetObject(uint32_t num);
    // Follows references (bounded); non-references are returned unchanged.
    PdfObj Resolve(const PdfObj& o);
    PdfObj GetResolved(const PdfObj& dict, const char* key) { return Resolve(dict.Get(key)); }

    // Reads and decodes a stream's data. Returns false if the filters could not be applied.
    bool ReadStream(const PdfObj& stream, std::string& out, size_t maxOut, bool decrypt = true);

    PdfObj Root();

private:
    bool ReadHeader();
    bool FindStartXref(uint64_t& off);
    bool LoadXrefChain(uint64_t off);
    bool LoadXrefAt(uint64_t off, PdfObj& trailer, bool& isStream);
    bool LoadXrefTable(Lexer& lx, PdfObj& trailer);
    bool LoadXrefStream(const PdfObj& stm);
    void AddEntry(uint32_t num, const XrefEntry& e);
    void MergeTrailer(const PdfObj& t);
    bool Reconstruct();
    void ScanObjects(bool findCatalog);
    bool ParseIndirectAt(uint64_t off, uint32_t expectNum, PdfObj& obj, uint32_t* numOut = nullptr,
                         uint32_t* genOut = nullptr);
    PdfObj GetFromObjStm(uint32_t stmNum, uint32_t index, uint32_t num);
    bool StreamRawRange(const PdfObj& stream, uint64_t& start, uint64_t& len);
    void SetupEncryption();
    void DecryptStrings(PdfObj& o, uint32_t num, uint32_t gen, int depth);

    ByteSource& src_;
    BlockCache cache_;
    uint64_t base_ = 0;  // offset of "%PDF-" (junk before header shifts all offsets)
    std::string headerVersion_;
    PdfObj trailer_;
    std::unordered_map<uint32_t, XrefEntry> xref_;
    std::set<uint32_t> resolving_;
    struct ObjStm {
        std::string data;
        std::vector<std::pair<uint32_t, uint64_t>> offsets;  // (num, offset into data)
        uint64_t first = 0;
    };
    std::map<uint32_t, std::shared_ptr<ObjStm>> objStms_;
    std::unordered_map<uint32_t, PdfObj> objCache_;
    bool reconstructed_ = false;
    bool objectsScanned_ = false;
    bool lastXrefExact_ = false;
    bool xrefRepaired_ = false;
    bool lastXrefIsStream_ = false;
    uint64_t lastXrefOffset_ = 0;
    bool encrypted_ = false;
    uint32_t encryptNum_ = 0;
    std::unique_ptr<SecurityHandler> sec_;
};

}  // namespace pdfprops
