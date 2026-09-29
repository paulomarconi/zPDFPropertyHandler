#pragma once

#include <cstddef>
#include <string>

namespace pdfprops {

// Decompresses zlib-wrapped (or raw) DEFLATE data, as used by /FlateDecode.
// Output is capped at maxOut bytes. Returns true if any usable output was
// produced; PDFs with truncated or slightly damaged streams are common, so a
// partial result is returned rather than discarded.
bool Inflate(const std::string& in, std::string& out, size_t maxOut);

}  // namespace pdfprops
