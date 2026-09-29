// Extracts document metadata (Info dictionary + XMP fallback) from a PDF.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "PdfParser.h"

namespace pdfprops {

struct PdfDate {
    bool valid = false;
    int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    bool hasTz = false;
    int tzMinutes = 0;  // offset from UTC (local = UTC + tzMinutes)

    // Seconds since 1970-01-01 UTC; only meaningful if hasTz (otherwise the
    // fields are treated as UTC and the caller must apply the local zone).
    int64_t ToUnixSeconds() const;
    std::string ToString() const;  // "YYYY-MM-DD HH:MM:SS[+HH:MM]"
};

struct PdfMetadata {
    // All strings are UTF-8; empty = absent.
    std::string title, author, subject, keywords, creator, producer;
    std::string company, category, comments;
    PdfDate created, modified;
    int pageCount = -1;
    std::string version;  // e.g. "1.7"
    bool encrypted = false;
    bool decryptable = true;
    std::string encryption;
    bool reconstructed = false;
    bool hasXmp = false;
};

// Returns false if the input doesn't look like a PDF at all.
bool ExtractMetadata(ByteSource& src, PdfMetadata& md);

// Helpers (exposed for tests).
std::string DecodeTextString(const std::string& raw);
bool ParsePdfDate(const std::string& s, PdfDate& d);
bool ParseXmpDate(const std::string& s, PdfDate& d);
// Splits on ';' (or on ',' if allowComma and there is no ';'), trims, drops empties.
std::vector<std::string> SplitList(const std::string& s, bool allowComma);

}  // namespace pdfprops
