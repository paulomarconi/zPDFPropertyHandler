// Metadata editing via incremental update: the original bytes are never changed; a new Info
// dictionary, an updated XMP stream (if the file has one) and a new cross-reference section
// are appended.
#pragma once

#include <cstdint>
#include <string>

#include "PdfParser.h"

namespace pdfprops {

struct MetadataEdits {
    struct Field {
        bool set = false;   // false = leave unchanged
        std::string value;  // UTF-8; lists joined with "; "; empty = remove
    };
    Field title, author, subject, keywords, comments, category, company;
    int64_t nowUnix = 0;  // time stamp for ModDate / xmp:ModifyDate / xmp:MetadataDate (UTC)

    bool Any() const {
        return title.set || author.set || subject.set || keywords.set || comments.set || category.set || company.set;
    }
};

enum class UpdateStatus { kOk, kNotPdf, kEncrypted, kSigned, kDamaged, kError };

const char* UpdateStatusText(UpdateStatus s);

// Whether the file can safely receive a metadata update.
UpdateStatus CanUpdateMetadata(ByteSource& src);

// Builds the bytes to append to the file. The caller writes them at the current end of the file.
UpdateStatus BuildMetadataUpdate(ByteSource& src, const MetadataEdits& edits, std::string& append);

// Exposed for tests.
std::string EncodeTextString(const std::string& utf8);
std::string UpdateXmpPacket(const std::string& xmp, const MetadataEdits& edits);

}  // namespace pdfprops
