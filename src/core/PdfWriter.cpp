#include "PdfWriter.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "Crypto.h"
#include "PdfMetadata.h"

namespace pdfprops {

namespace {

const size_t kMaxXmp = 4u << 20;

std::string Hex(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) {
        o += d[c >> 4];
        o += d[c & 15];
    }
    return o;
}

std::vector<uint32_t> Utf8CodePoints(const std::string& s) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || i + extra >= s.size() + (extra ? 0 : 1)) {
            cps.push_back(0xFFFD);
            ++i;
            continue;
        }
        uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        cps.push_back(cp);
        i += extra + 1;
    }
    return cps;
}

std::string SerializeName(const std::string& n) {
    std::string o = "/";
    for (unsigned char c : n) {
        if (c < 0x21 || c > 0x7E || c == '#' || Lexer::IsDelim(c)) {
            char b[4];
            snprintf(b, sizeof b, "#%02X", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o;
}

std::string SerializeReal(double v) {
    char b[64];
    snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0" || s.empty()) s = "0";
    return s;
}

// Direct objects only (streams cannot appear inside the Info dictionary).
std::string Serialize(const PdfObj& o, int depth = 0) {
    if (depth > PdfLimits::kMaxDepth) return "null";
    switch (o.type) {
        case PdfType::Bool: return o.boolean ? "true" : "false";
        case PdfType::Int: return std::to_string(o.integer);
        case PdfType::Real: return SerializeReal(o.real);
        case PdfType::String: {
            bool printable = std::all_of(o.str.begin(), o.str.end(), [](char ch) { return ch >= 0x20 && ch < 0x7F; });
            if (!printable) return "<" + Hex(o.str) + ">";
            std::string s = "(";
            for (char ch : o.str) {
                if (ch == '(' || ch == ')' || ch == '\\') s += '\\';
                s += ch;
            }
            return s + ")";
        }
        case PdfType::Name: return SerializeName(o.str);
        case PdfType::Ref: return std::to_string(o.num) + " " + std::to_string(o.gen) + " R";
        case PdfType::Array: {
            std::string s = "[";
            if (o.array)
                for (size_t i = 0; i < o.array->size(); ++i) {
                    if (i) s += ' ';
                    s += Serialize((*o.array)[i], depth + 1);
                }
            return s + "]";
        }
        case PdfType::Dict: {
            std::string s = "<<";
            if (o.dict)
                for (const auto& kv : *o.dict) s += SerializeName(kv.first) + " " + Serialize(kv.second, depth + 1);
            return s + ">>";
        }
        default: return "null";
    }
}

void CivilFromUnix(int64_t t, int& y, int& mo, int& d, int& h, int& mi, int& s) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    h = static_cast<int>(rem / 3600);
    mi = static_cast<int>(rem % 3600 / 60);
    s = static_cast<int>(rem % 60);
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (mo <= 2));
}

std::string PdfDateUtc(int64_t t) {
    int y, mo, d, h, mi, s;
    CivilFromUnix(t, y, mo, d, h, mi, s);
    char b[32];
    snprintf(b, sizeof b, "(D:%04d%02d%02d%02d%02d%02dZ)", y, mo, d, h, mi, s);
    return b;
}

std::string XmpDateUtc(int64_t t) {
    int y, mo, d, h, mi, s;
    CivilFromUnix(t, y, mo, d, h, mi, s);
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, s);
    return b;
}

std::string XmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

bool IsXmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Position of the last "</name>" closing tag; XML allows whitespace before the '>' (Foxit writes
// "</rdf:RDF\r\n>").
size_t FindLastCloseTag(const std::string& x, const std::string& name) {
    const std::string open = "</" + name;
    size_t p = x.size();
    while (p > 0 && (p = x.rfind(open, p - 1)) != std::string::npos) {
        size_t q = p + open.size();
        while (q < x.size() && IsXmlSpace(x[q])) ++q;
        if (q < x.size() && x[q] == '>') return p;
        if (p == 0) break;
    }
    return std::string::npos;
}

// Removes every element-form and attribute-form occurrence of an XMP property.
void RemoveXmpProperty(std::string& x, const std::string& qname) {
    const std::string open = "<" + qname, close = "</" + qname;
    size_t p = 0;
    while ((p = x.find(open, p)) != std::string::npos) {
        size_t after = p + open.size();
        if (after >= x.size() || !(IsXmlSpace(x[after]) || x[after] == '>' || x[after] == '/')) {
            p = after;
            continue;
        }
        size_t tagEnd = x.find('>', p);
        if (tagEnd == std::string::npos) return;
        if (x[tagEnd - 1] == '/') {
            x.erase(p, tagEnd + 1 - p);
            continue;
        }
        size_t c = x.find(close, tagEnd);
        if (c == std::string::npos) return;
        size_t cEnd = x.find('>', c);
        if (cEnd == std::string::npos) return;
        x.erase(p, cEnd + 1 - p);
    }
    p = 0;
    while ((p = x.find(qname, p)) != std::string::npos) {
        size_t e = p + qname.size();
        bool inTag = x.rfind('<', p) != std::string::npos &&
                     (x.rfind('>', p) == std::string::npos || x.rfind('>', p) < x.rfind('<', p));
        if (p == 0 || !IsXmlSpace(x[p - 1]) || !inTag) {
            p = e;
            continue;
        }
        size_t q = e;
        while (q < x.size() && IsXmlSpace(x[q])) ++q;
        if (q >= x.size() || x[q] != '=') {
            p = e;
            continue;
        }
        ++q;
        while (q < x.size() && IsXmlSpace(x[q])) ++q;
        if (q >= x.size() || (x[q] != '"' && x[q] != '\'')) {
            p = e;
            continue;
        }
        size_t endQuote = x.find(x[q], q + 1);
        if (endQuote == std::string::npos) return;
        size_t start = p;
        while (start > 0 && IsXmlSpace(x[start - 1])) --start;
        x.erase(start, endQuote + 1 - start);
        p = start;
    }
}

std::string XmpList(const char* container, const std::vector<std::string>& items, bool xdefault) {
    std::string s = std::string("<rdf:") + container + ">";
    for (const auto& v : items) {
        s += xdefault ? "<rdf:li xml:lang=\"x-default\">" : "<rdf:li>";
        s += XmlEscape(v) + "</rdf:li>";
    }
    return s + "</rdf:" + container + ">";
}

struct Checked {
    UpdateStatus status = UpdateStatus::kError;
    PdfObj root;
};

Checked Check(PdfDocument& doc) {
    Checked c;
    if (doc.Encrypted()) {
        c.status = UpdateStatus::kEncrypted;
        return c;
    }
    c.root = doc.Root();
    if (!c.root.IsDict() || !doc.Trailer().Get("Root").IsRef()) {
        c.status = UpdateStatus::kDamaged;
        return c;
    }
    PdfObj acro = doc.GetResolved(c.root, "AcroForm");
    PdfObj sigFlags = acro.IsDict() ? doc.GetResolved(acro, "SigFlags") : PdfObj();
    PdfObj perms = doc.GetResolved(c.root, "Perms");
    if ((sigFlags.IsInt() && (sigFlags.integer & 1)) || (perms.IsDict() && !perms.dict->empty())) {
        c.status = UpdateStatus::kSigned;
        return c;
    }
    // Resolve what the update touches: this can trigger a lazy reconstruction of a damaged xref.
    doc.GetResolved(doc.Trailer(), "Info");
    doc.GetResolved(c.root, "Metadata");
    c.status = doc.CleanXref() ? UpdateStatus::kOk : UpdateStatus::kDamaged;
    return c;
}

}  // namespace

const char* UpdateStatusText(UpdateStatus s) {
    switch (s) {
        case UpdateStatus::kOk: return "ok";
        case UpdateStatus::kNotPdf: return "not a PDF file";
        case UpdateStatus::kEncrypted: return "the file is encrypted";
        case UpdateStatus::kSigned: return "the file is digitally signed";
        case UpdateStatus::kDamaged: return "the file is damaged (its cross-reference table had to be repaired)";
        default: return "internal error";
    }
}

std::string EncodeTextString(const std::string& utf8) {
    std::vector<uint32_t> cps = Utf8CodePoints(utf8);
    bool ascii = std::all_of(cps.begin(), cps.end(), [](uint32_t c) { return c >= 0x20 && c < 0x7F; });
    if (ascii) {
        std::string s = "(";
        for (uint32_t c : cps) {
            if (c == '(' || c == ')' || c == '\\') s += '\\';
            s += static_cast<char>(c);
        }
        return s + ")";
    }
    std::string be = "\xFE\xFF";
    for (uint32_t c : cps) {
        if (c < 0x20) c = ' ';  // control characters have no place in metadata
        auto put = [&](uint32_t u) {
            be += static_cast<char>(u >> 8);
            be += static_cast<char>(u & 0xFF);
        };
        if (c >= 0x10000) {
            c -= 0x10000;
            put(0xD800 + (c >> 10));
            put(0xDC00 + (c & 0x3FF));
        } else {
            put(c);
        }
    }
    return "<" + Hex(be) + ">";
}

std::string UpdateXmpPacket(const std::string& xmp, const MetadataEdits& e) {
    std::string x = xmp;
    size_t rdfEnd = FindLastCloseTag(x, "rdf:RDF");
    if (rdfEnd == std::string::npos) return std::string();
    struct Prop {
        const MetadataEdits::Field* field;
        const char* names[2];
    };
    const Prop props[] = {
        {&e.title, {"dc:title", nullptr}},       {&e.author, {"dc:creator", nullptr}},
        {&e.subject, {"dc:description", nullptr}}, {&e.keywords, {"pdf:Keywords", "dc:subject"}},
        {&e.company, {"pdfx:Company", nullptr}}, {&e.category, {"pdfx:Category", nullptr}},
        {&e.comments, {"pdfx:Comments", nullptr}},
    };
    for (const Prop& p : props)
        if (p.field->set)
            for (const char* n : p.names)
                if (n) RemoveXmpProperty(x, n);
    for (const char* n : {"xmp:ModifyDate", "xap:ModifyDate", "xmp:MetadataDate", "xap:MetadataDate"})
        RemoveXmpProperty(x, n);

    std::string d =
        "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
        " xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\""
        " xmlns:pdfx=\"http://ns.adobe.com/pdfx/1.3/\">\n";
    auto has = [](const MetadataEdits::Field& f) { return f.set && !f.value.empty(); };
    if (has(e.title)) d += "<dc:title>" + XmpList("Alt", {e.title.value}, true) + "</dc:title>\n";
    if (has(e.author)) d += "<dc:creator>" + XmpList("Seq", SplitList(e.author.value, false), false) + "</dc:creator>\n";
    if (has(e.subject)) d += "<dc:description>" + XmpList("Alt", {e.subject.value}, true) + "</dc:description>\n";
    if (has(e.keywords)) {
        d += "<pdf:Keywords>" + XmlEscape(e.keywords.value) + "</pdf:Keywords>\n";
        d += "<dc:subject>" + XmpList("Bag", SplitList(e.keywords.value, true), false) + "</dc:subject>\n";
    }
    if (has(e.company)) d += "<pdfx:Company>" + XmlEscape(e.company.value) + "</pdfx:Company>\n";
    if (has(e.category)) d += "<pdfx:Category>" + XmlEscape(e.category.value) + "</pdfx:Category>\n";
    if (has(e.comments)) d += "<pdfx:Comments>" + XmlEscape(e.comments.value) + "</pdfx:Comments>\n";
    std::string now = XmpDateUtc(e.nowUnix);
    d += "<xmp:ModifyDate>" + now + "</xmp:ModifyDate>\n<xmp:MetadataDate>" + now + "</xmp:MetadataDate>\n";
    d += "</rdf:Description>\n";
    // Insert right after the <rdf:RDF ...> start tag rather than before </rdf:RDF>: some writers
    // leave malformed elements in the packet (e.g. Foxit's "<xmp:ModifyDate2024-...") and a
    // recovering XML parser can swallow whatever follows them.
    size_t rdfOpen = 0;
    while ((rdfOpen = x.find("<rdf:RDF", rdfOpen)) != std::string::npos) {
        char c = rdfOpen + 8 < x.size() ? x[rdfOpen + 8] : '\0';
        if (IsXmlSpace(c) || c == '>') break;
        rdfOpen += 8;
    }
    if (rdfOpen == std::string::npos) return std::string();
    size_t openEnd = x.find('>', rdfOpen);
    if (openEnd == std::string::npos || openEnd > FindLastCloseTag(x, "rdf:RDF")) return std::string();
    x.insert(openEnd + 1, "\n" + d);
    return x;
}

UpdateStatus CanUpdateMetadata(ByteSource& src) {
    PdfDocument doc(src);
    if (!doc.Load()) return UpdateStatus::kNotPdf;
    return Check(doc).status;
}

UpdateStatus BuildMetadataUpdate(ByteSource& src, const MetadataEdits& e, std::string& out) {
    out.clear();
    PdfDocument doc(src);
    if (!doc.Load()) return UpdateStatus::kNotPdf;
    Checked c = Check(doc);
    if (c.status != UpdateStatus::kOk) return c.status;

    const uint64_t fileSize = src.Size();
    char last = 0;
    if (fileSize == 0 || src.ReadAt(fileSize - 1, &last, 1) != 1) return UpdateStatus::kError;
    if (last != '\n' && last != '\r') out += '\n';
    auto offset = [&]() { return fileSize + out.size(); };

    const PdfObj& trailer = doc.Trailer();
    PdfObj sizeObj = doc.GetResolved(trailer, "Size");
    uint32_t next = doc.MaxObjectNumber() + 1;
    if (sizeObj.IsInt() && sizeObj.integer > next && sizeObj.integer < (1ll << 31))
        next = static_cast<uint32_t>(sizeObj.integer);

    struct Written {
        uint32_t num, gen;
        uint64_t offset;
    };
    std::vector<Written> written;

    // ---- Info dictionary: copy, then apply the edits ----
    PdfObj infoRef = trailer.Get("Info");
    PdfObj info = doc.Resolve(infoRef);
    uint32_t infoNum, infoGen = 0;
    if (infoRef.IsRef()) {
        infoNum = infoRef.num;
        infoGen = infoRef.gen;
    } else {
        infoNum = next++;
    }
    std::vector<std::pair<std::string, std::string>> entries;  // key -> serialized value
    auto setEntry = [&](const std::string& key, const std::string& value) {
        for (auto& kv : entries)
            if (kv.first == key) {
                kv.second = value;
                return;
            }
        entries.emplace_back(key, value);
    };
    auto removeEntry = [&](const std::string& key) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::pair<std::string, std::string>& kv) { return kv.first == key; }),
                      entries.end());
    };
    if (info.IsDict())
        for (const auto& kv : *info.dict) setEntry(kv.first, Serialize(kv.second));
    const std::pair<const char*, const MetadataEdits::Field*> fields[] = {
        {"Title", &e.title},       {"Author", &e.author},     {"Subject", &e.subject}, {"Keywords", &e.keywords},
        {"Comments", &e.comments}, {"Category", &e.category}, {"Company", &e.company},
    };
    for (const auto& f : fields) {
        if (!f.second->set) continue;
        if (f.second->value.empty())
            removeEntry(f.first);
        else
            setEntry(f.first, EncodeTextString(f.second->value));
    }
    setEntry("ModDate", PdfDateUtc(e.nowUnix));
    std::string infoBody = "<<";
    for (const auto& kv : entries) infoBody += SerializeName(kv.first) + " " + kv.second;
    infoBody += ">>";
    written.push_back({infoNum, infoGen, offset()});
    out += std::to_string(infoNum) + " " + std::to_string(infoGen) + " obj\n" + infoBody + "\nendobj\n";

    // ---- XMP metadata stream, if present and readable ----
    PdfObj metaRef = c.root.Get("Metadata");
    PdfObj meta = doc.Resolve(metaRef);
    if (metaRef.IsRef() && meta.IsStream()) {
        std::string raw;
        bool utf16 = false;
        if (doc.ReadStream(meta, raw, kMaxXmp)) {
            utf16 = raw.size() >= 2 && ((raw[0] == '\xFE' && raw[1] == '\xFF') || (raw[0] == '\xFF' && raw[1] == '\xFE'));
            std::string updated = utf16 ? std::string() : UpdateXmpPacket(raw, e);
            if (!updated.empty()) {
                written.push_back({metaRef.num, metaRef.gen, offset()});
                out += std::to_string(metaRef.num) + " " + std::to_string(metaRef.gen) + " obj\n";
                out += "<</Type /Metadata /Subtype /XML /Length " + std::to_string(updated.size()) + ">>\nstream\n";
                out += updated + "\nendstream\nendobj\n";
            }
        }
    }

    // ---- File identifier: keep the first part, change the second ----
    std::string id0;
    PdfObj id = doc.GetResolved(trailer, "ID");
    if (id.IsArray()) {
        PdfObj first = doc.Resolve(id.At(0));
        if (first.IsString()) id0 = first.str;
    }
    std::string id1 = crypto::Md5(id0 + std::to_string(fileSize) + std::to_string(e.nowUnix) + infoBody);
    if (id0.empty()) id0 = id1;
    const std::string idArray = "[<" + Hex(id0) + "><" + Hex(id1) + ">]";
    const PdfObj& rootRef = trailer.Get("Root");
    const std::string common = "/Root " + Serialize(rootRef) + " /Info " + std::to_string(infoNum) + " " +
                               std::to_string(infoGen) + " R /ID " + idArray + " /Prev " +
                               std::to_string(doc.LastXrefOffset());

    // ---- Cross-reference section, of the same kind as the newest existing one ----
    uint64_t xrefOffset;
    if (doc.LastXrefIsStream()) {
        uint32_t xrefNum = next++;
        xrefOffset = offset();
        written.push_back({xrefNum, 0, xrefOffset});
        std::sort(written.begin(), written.end(), [](const Written& a, const Written& b) { return a.num < b.num; });
        int w = 1;
        while (w < 8 && (xrefOffset >> (8 * w)) != 0) ++w;
        std::string data, index;
        for (size_t i = 0; i < written.size();) {
            size_t j = i;
            while (j + 1 < written.size() && written[j + 1].num == written[j].num + 1) ++j;
            index += (index.empty() ? "" : " ") + std::to_string(written[i].num) + " " + std::to_string(j - i + 1);
            for (size_t k = i; k <= j; ++k) {
                data += '\x01';
                for (int b = w - 1; b >= 0; --b) data += static_cast<char>((written[k].offset >> (8 * b)) & 0xFF);
                data += static_cast<char>((written[k].gen >> 8) & 0xFF);
                data += static_cast<char>(written[k].gen & 0xFF);
            }
            i = j + 1;
        }
        out += std::to_string(xrefNum) + " 0 obj\n<</Type /XRef /Size " + std::to_string(next) + " /W [1 " +
               std::to_string(w) + " 2] /Index [" + index + "] " + common + " /Length " + std::to_string(data.size()) +
               ">>\nstream\n" + data + "\nendstream\nendobj\n";
    } else {
        xrefOffset = offset();
        std::sort(written.begin(), written.end(), [](const Written& a, const Written& b) { return a.num < b.num; });
        out += "xref\n";
        for (size_t i = 0; i < written.size();) {
            size_t j = i;
            while (j + 1 < written.size() && written[j + 1].num == written[j].num + 1) ++j;
            out += std::to_string(written[i].num) + " " + std::to_string(j - i + 1) + "\n";
            for (size_t k = i; k <= j; ++k) {
                char line[32];
                snprintf(line, sizeof line, "%010llu %05u n\r\n", static_cast<unsigned long long>(written[k].offset),
                         written[k].gen & 0xFFFF);
                out += line;
            }
            i = j + 1;
        }
        out += "trailer\n<</Size " + std::to_string(next) + " " + common + ">>\n";
    }
    out += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
    return UpdateStatus::kOk;
}

}  // namespace pdfprops
