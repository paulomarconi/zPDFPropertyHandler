#include "PdfMetadata.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <set>

namespace pdfprops {

namespace {

const size_t kMaxXmp = 4u << 20;

// PDFDocEncoding code points that differ from Latin-1 (0 = undefined).
const uint16_t kPdfDoc18[8] = {0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC};
const uint16_t kPdfDoc80[32] = {0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044,
                                0x2039, 0x203A, 0x2212, 0x2030, 0x201E, 0x201C, 0x201D, 0x2018,
                                0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141, 0x0152, 0x0160,
                                0x0178, 0x017D, 0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0};

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::vector<uint32_t> DecodeUtf16(const std::string& s, size_t start, bool bigEndian) {
    std::vector<uint32_t> cps;
    for (size_t i = start; i + 1 < s.size(); i += 2) {
        unsigned char a = static_cast<unsigned char>(s[i]), b = static_cast<unsigned char>(s[i + 1]);
        uint32_t u = bigEndian ? (a << 8 | b) : (b << 8 | a);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i + 2]), d = static_cast<unsigned char>(s[i + 3]);
            uint32_t lo = bigEndian ? (c << 8 | d) : (d << 8 | c);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cps.push_back(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i += 2;
                continue;
            }
        }
        cps.push_back(u >= 0xD800 && u <= 0xDFFF ? 0xFFFD : u);
    }
    return cps;
}

std::vector<uint32_t> DecodeUtf8(const std::string& s, size_t start) {
    std::vector<uint32_t> cps;
    size_t i = start;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || i + extra >= s.size() + (extra ? 0 : 1)) {
            cps.push_back(0xFFFD);
            ++i;
            continue;
        }
        uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) {
            cps.push_back(0xFFFD);
            ++i;
            continue;
        }
        cps.push_back(cp);
        i += extra + 1;
    }
    return cps;
}

bool IsTrimmable(uint32_t cp) { return cp == 0 || cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0xFEFF; }

std::string EncodeTrimmed(const std::vector<uint32_t>& cps) {
    // Remove language escape sequences (U+001B lang U+001B) and NULs.
    std::vector<uint32_t> v;
    v.reserve(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        if (cps[i] == 0x1B) {
            size_t j = i + 1;
            while (j < cps.size() && cps[j] != 0x1B) ++j;
            i = j;  // skip through the closing escape (or to the end)
            continue;
        }
        if (cps[i] == 0) continue;
        v.push_back(cps[i]);
    }
    size_t b = 0, e = v.size();
    while (b < e && IsTrimmable(v[b])) ++b;
    while (e > b && IsTrimmable(v[e - 1])) --e;
    std::string out;
    for (size_t i = b; i < e; ++i) AppendUtf8(out, v[i]);
    return out;
}

std::string TrimAscii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n' || s[b] == '\0')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == '\0'))
        --e;
    return s.substr(b, e - b);
}

bool ReadDigits(const std::string& s, size_t& i, int n, int& v) {
    if (i + n > s.size()) return false;
    int r = 0;
    for (int k = 0; k < n; ++k) {
        char c = s[i + k];
        if (c < '0' || c > '9') return false;
        r = r * 10 + (c - '0');
    }
    v = r;
    i += n;
    return true;
}

bool RangesOk(const PdfDate& d) {
    return d.year >= 1 && d.year <= 9999 && d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31 &&
           d.hour >= 0 && d.hour <= 23 && d.minute >= 0 && d.minute <= 59 && d.second >= 0 && d.second <= 59 &&
           d.tzMinutes >= -14 * 60 && d.tzMinutes <= 14 * 60;
}

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// ---- XMP ----

std::string DecodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "amp") cp = '&';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            if (ent[1] == 'x' || ent[1] == 'X')
                cp = static_cast<uint32_t>(strtoul(ent.c_str() + 2, nullptr, 16));
            else
                cp = static_cast<uint32_t>(strtoul(ent.c_str() + 1, nullptr, 10));
            if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
        }
        if (cp == 0) {
            out += s[i];
            continue;
        }
        AppendUtf8(out, cp);
        i = semi;
    }
    return out;
}

std::string XmlText(const std::string& s) {
    std::string t;
    bool inTag = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!inTag && s.compare(i, 9, "<![CDATA[") == 0) {
            size_t e = s.find("]]>", i + 9);
            if (e == std::string::npos) e = s.size();
            t.append(s, i + 9, e - i - 9);
            i = e + 2;
            continue;
        }
        char c = s[i];
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) t += c;
    }
    return TrimAscii(DecodeEntities(t));
}

bool IsXmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string AttrValue(const std::string& tag, const std::string& name) {
    size_t p = 0;
    while ((p = tag.find(name, p)) != std::string::npos) {
        size_t e = p + name.size();
        if (p > 0 && IsXmlSpace(tag[p - 1])) {
            while (e < tag.size() && IsXmlSpace(tag[e])) ++e;
            if (e < tag.size() && tag[e] == '=') {
                ++e;
                while (e < tag.size() && IsXmlSpace(tag[e])) ++e;
                if (e < tag.size() && (tag[e] == '"' || tag[e] == '\'')) {
                    size_t close = tag.find(tag[e], e + 1);
                    if (close == std::string::npos) return std::string();
                    return TrimAscii(DecodeEntities(tag.substr(e + 1, close - e - 1)));
                }
            }
        }
        p = e;
    }
    return std::string();
}

// Values of an XMP property given in element or attribute form. For rdf:Alt
// the x-default entry comes first.
std::vector<std::string> XmpValues(const std::string& x, const std::string& qname) {
    std::vector<std::string> items;
    std::string open = "<" + qname;
    size_t pos = 0;
    while ((pos = x.find(open, pos)) != std::string::npos) {
        size_t after = pos + open.size();
        if (after >= x.size()) break;
        char c = x[after];
        if (!IsXmlSpace(c) && c != '>' && c != '/') {
            pos = after;
            continue;
        }
        size_t tagEnd = x.find('>', pos);
        if (tagEnd == std::string::npos) break;
        if (x[tagEnd - 1] == '/') {
            pos = tagEnd;
            continue;
        }
        size_t close = x.find("</" + qname, tagEnd);
        if (close == std::string::npos) break;
        std::string inner = x.substr(tagEnd + 1, close - tagEnd - 1);
        if (inner.find("<rdf:li") != std::string::npos) {
            size_t lp = 0;
            while ((lp = inner.find("<rdf:li", lp)) != std::string::npos) {
                size_t le = inner.find('>', lp);
                if (le == std::string::npos) break;
                std::string tag = inner.substr(lp, le - lp);
                if (inner[le - 1] == '/') {
                    lp = le;
                    continue;
                }
                size_t lc = inner.find("</rdf:li", le);  // "</rdf:li>" or "</rdf:li\r\n>"
                if (lc == std::string::npos) lc = inner.size();
                std::string v = XmlText(inner.substr(le + 1, lc - le - 1));
                if (!v.empty()) {
                    if (AttrValue(tag, "xml:lang") == "x-default")
                        items.insert(items.begin(), v);
                    else
                        items.push_back(v);
                }
                lp = lc;
            }
        } else {
            std::string v = XmlText(inner);
            if (!v.empty()) items.push_back(v);
        }
        if (!items.empty()) return items;
        pos = close;
    }
    // Attribute form: <rdf:Description ... qname="value">
    size_t p = 0;
    while ((p = x.find(qname, p)) != std::string::npos) {
        if (p > 0 && IsXmlSpace(x[p - 1])) {
            size_t tagStart = x.rfind('<', p);
            size_t tagEnd = x.find('>', p);
            if (tagStart != std::string::npos && tagEnd != std::string::npos) {
                std::string v = AttrValue(x.substr(tagStart, tagEnd - tagStart), qname);
                if (!v.empty()) {
                    items.push_back(v);
                    return items;
                }
            }
        }
        p += qname.size();
    }
    return items;
}

std::string XmpFirst(const std::string& x, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        auto v = XmpValues(x, n);
        if (!v.empty()) return v[0];
    }
    return std::string();
}

std::string XmpJoined(const std::string& x, const char* name) {
    std::string out;
    for (const auto& v : XmpValues(x, name)) {
        if (!out.empty()) out += "; ";
        out += v;
    }
    return out;
}

std::string XmpToUtf8(const std::string& raw) {
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE && static_cast<unsigned char>(raw[1]) == 0xFF) {
        std::string o;
        for (uint32_t cp : DecodeUtf16(raw, 2, true)) AppendUtf8(o, cp);
        return o;
    }
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) {
        std::string o;
        for (uint32_t cp : DecodeUtf16(raw, 2, false)) AppendUtf8(o, cp);
        return o;
    }
    return raw;
}

// ---- document helpers ----

int CountPages(PdfDocument& doc, const PdfObj& node, std::set<uint32_t>& seen, int depth, int& budget) {
    if (depth > PdfLimits::kMaxDepth || --budget < 0) return 0;
    PdfObj kids = doc.GetResolved(node, "Kids");
    if (!kids.IsArray()) return node.IsDict() ? 1 : 0;
    int total = 0;
    for (size_t i = 0; i < kids.Count(); ++i) {
        const PdfObj& k = kids.At(i);
        if (k.IsRef() && !seen.insert(k.num).second) continue;
        PdfObj kid = doc.Resolve(k);
        if (!kid.IsDict()) continue;
        if (kid.Get("Type").IsName("Page") || kid.Get("Kids").IsNull())
            ++total;
        else
            total += CountPages(doc, kid, seen, depth + 1, budget);
        if (budget < 0) break;
    }
    return total;
}

int VersionValue(const std::string& v) {
    int major = 0, minor = 0;
    if (sscanf(v.c_str(), "%d.%d", &major, &minor) < 1) return -1;
    return major * 100 + minor;
}

std::string InfoString(PdfDocument& doc, const PdfObj& info, const char* key) {
    PdfObj v = doc.GetResolved(info, key);
    if (v.IsString()) return DecodeTextString(v.str);
    return std::string();
}

}  // namespace

std::string DecodeTextString(const std::string& raw) {
    std::vector<uint32_t> cps;
    auto u8 = [&](size_t i) { return i < raw.size() ? static_cast<unsigned char>(raw[i]) : 0u; };
    if (u8(0) == 0xFE && u8(1) == 0xFF) {
        cps = DecodeUtf16(raw, 2, true);
    } else if (u8(0) == 0xFF && u8(1) == 0xFE) {
        cps = DecodeUtf16(raw, 2, false);
    } else if (u8(0) == 0xEF && u8(1) == 0xBB && u8(2) == 0xBF) {
        cps = DecodeUtf8(raw, 3);
    } else {
        cps.reserve(raw.size());
        for (unsigned char c : raw) {
            uint32_t cp = c;
            if (c >= 0x18 && c <= 0x1F) cp = kPdfDoc18[c - 0x18];
            else if (c >= 0x80 && c <= 0x9F) cp = kPdfDoc80[c - 0x80];
            else if (c == 0xA0) cp = 0x20AC;
            else if (c == 0x7F || c == 0xAD) cp = 0;
            if (cp == 0 && c != 0) cp = 0xFFFD;
            cps.push_back(cp);
        }
    }
    return EncodeTrimmed(cps);
}

bool ParsePdfDate(const std::string& in, PdfDate& d) {
    d = PdfDate();
    std::string s = TrimAscii(in);
    size_t i = 0;
    if (s.compare(0, 2, "D:") == 0) i = 2;
    if (!ReadDigits(s, i, 4, d.year)) return false;
    int* parts[] = {&d.month, &d.day, &d.hour, &d.minute, &d.second};
    for (int* p : parts)
        if (!ReadDigits(s, i, 2, *p)) break;
    if (d.second == 60) d.second = 59;
    if (i < s.size()) {
        char c = s[i];
        if (c == 'Z' || c == 'z') {
            d.hasTz = true;
            d.tzMinutes = 0;
        } else if (c == '+' || c == '-') {
            ++i;
            int hh = 0, mm = 0;
            if (ReadDigits(s, i, 2, hh)) {
                if (i < s.size() && (s[i] == '\'' || s[i] == ':')) ++i;
                ReadDigits(s, i, 2, mm);
                d.hasTz = true;
                d.tzMinutes = (hh * 60 + mm) * (c == '-' ? -1 : 1);
            }
        }
    }
    d.valid = RangesOk(d);
    return d.valid;
}

bool ParseXmpDate(const std::string& in, PdfDate& d) {
    d = PdfDate();
    std::string s = TrimAscii(in);
    size_t i = 0;
    if (!ReadDigits(s, i, 4, d.year)) return false;
    auto sep = [&](char c) {
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    };
    if (sep('-') && ReadDigits(s, i, 2, d.month) && sep('-') && ReadDigits(s, i, 2, d.day) &&
        (sep('T') || sep(' ')) && ReadDigits(s, i, 2, d.hour) && sep(':') && ReadDigits(s, i, 2, d.minute)) {
        if (sep(':') && ReadDigits(s, i, 2, d.second)) {
            if (sep('.'))
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (d.second == 60) d.second = 59;
        if (i < s.size()) {
            char c = s[i];
            if (c == 'Z' || c == 'z') {
                d.hasTz = true;
            } else if (c == '+' || c == '-') {
                ++i;
                int hh = 0, mm = 0;
                if (ReadDigits(s, i, 2, hh)) {
                    sep(':');
                    ReadDigits(s, i, 2, mm);
                    d.hasTz = true;
                    d.tzMinutes = (hh * 60 + mm) * (c == '-' ? -1 : 1);
                }
            }
        }
    }
    d.valid = RangesOk(d);
    return d.valid;
}

int64_t PdfDate::ToUnixSeconds() const {
    return DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
           minute * 60 + second - static_cast<int64_t>(tzMinutes) * 60;
}

std::string PdfDate::ToString() const {
    if (!valid) return std::string();
    char buf[64];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    std::string s = buf;
    if (hasTz) {
        int m = tzMinutes < 0 ? -tzMinutes : tzMinutes;
        snprintf(buf, sizeof buf, "%c%02d:%02d", tzMinutes < 0 ? '-' : '+', m / 60, m % 60);
        s += buf;
    }
    return s;
}

std::vector<std::string> SplitList(const std::string& s, bool allowComma) {
    char sep = ';';
    if (allowComma && s.find(';') == std::string::npos) sep = ',';
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        std::string part = TrimAscii(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (!part.empty()) out.push_back(part);
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

bool ExtractMetadata(ByteSource& src, PdfMetadata& md) {
    md = PdfMetadata();
    PdfDocument doc(src);
    bool loaded = doc.Load();
    md.version = doc.HeaderVersion();
    md.encrypted = doc.Encrypted();
    md.decryptable = doc.CanDecrypt();
    md.encryption = doc.EncryptionInfo();
    md.reconstructed = doc.Reconstructed();
    if (!loaded && md.version.empty()) return false;

    PdfObj root = doc.Root();
    PdfObj rv = doc.GetResolved(root, "Version");
    if (rv.IsName() && VersionValue(rv.str) > VersionValue(md.version)) md.version = rv.str;

    PdfObj info = doc.GetResolved(doc.Trailer(), "Info");
    if (info.IsDict()) {
        md.title = InfoString(doc, info, "Title");
        md.author = InfoString(doc, info, "Author");
        md.subject = InfoString(doc, info, "Subject");
        md.keywords = InfoString(doc, info, "Keywords");
        md.creator = InfoString(doc, info, "Creator");
        md.producer = InfoString(doc, info, "Producer");
        md.company = InfoString(doc, info, "Company");
        md.category = InfoString(doc, info, "Category");
        md.comments = InfoString(doc, info, "Comments");
        ParsePdfDate(InfoString(doc, info, "CreationDate"), md.created);
        ParsePdfDate(InfoString(doc, info, "ModDate"), md.modified);
    }

    PdfObj pages = doc.GetResolved(root, "Pages");
    if (pages.IsDict()) {
        PdfObj count = doc.GetResolved(pages, "Count");
        if (count.IsInt() && count.integer >= 0 && count.integer < 100000000) {
            md.pageCount = static_cast<int>(count.integer);
        } else {
            std::set<uint32_t> seen;
            int budget = 200000;
            md.pageCount = CountPages(doc, pages, seen, 0, budget);
        }
    }

    PdfObj meta = doc.GetResolved(root, "Metadata");
    std::string raw;
    if (meta.IsStream() && doc.ReadStream(meta, raw, kMaxXmp)) {
        std::string x = XmpToUtf8(raw);
        if (x.find("x:xmpmeta") != std::string::npos || x.find("rdf:RDF") != std::string::npos) {
            md.hasXmp = true;
            auto fill = [](std::string& field, const std::string& v) {
                if (field.empty()) field = v;
            };
            fill(md.title, XmpFirst(x, {"dc:title"}));
            fill(md.author, XmpJoined(x, "dc:creator"));
            fill(md.subject, XmpFirst(x, {"dc:description"}));
            fill(md.keywords, XmpFirst(x, {"pdf:Keywords"}));
            fill(md.keywords, XmpJoined(x, "dc:subject"));
            fill(md.creator, XmpFirst(x, {"xmp:CreatorTool", "xap:CreatorTool"}));
            fill(md.producer, XmpFirst(x, {"pdf:Producer"}));
            // Custom document properties (Office's PDF export writes them to the pdfx namespace).
            fill(md.company, XmpFirst(x, {"pdfx:Company"}));
            fill(md.category, XmpFirst(x, {"pdfx:Category"}));
            fill(md.comments, XmpFirst(x, {"pdfx:Comments"}));
            if (!md.created.valid) ParseXmpDate(XmpFirst(x, {"xmp:CreateDate", "xap:CreateDate"}), md.created);
            if (!md.modified.valid) ParseXmpDate(XmpFirst(x, {"xmp:ModifyDate", "xap:ModifyDate"}), md.modified);
        }
    }
    return true;
}

}  // namespace pdfprops
