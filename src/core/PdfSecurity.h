// Standard security handler (PDF 1.1 - 2.0) for the empty user password.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "PdfParser.h"

namespace pdfprops {

class SecurityHandler {
public:
    enum Method { kNone, kRc4, kAesV2, kAesV3 };

    // Returns nullptr if the Encrypt dictionary is not a standard-handler dictionary.
    static std::unique_ptr<SecurityHandler> Create(PdfDocument& doc, const PdfObj& encrypt, const std::string& id0);

    bool Authenticated() const { return ok_; }
    std::string Info() const;

    // Returns false if the data could not be decrypted (wrong password); out is then empty.
    bool DecryptString(const std::string& in, uint32_t num, uint32_t gen, std::string& out) const;
    bool DecryptStream(const std::string& in, uint32_t num, uint32_t gen, const PdfObj& dict, PdfDocument& doc,
                       std::string& out) const;

private:
    Method MethodFor(PdfDocument& doc, const PdfObj& cf, const std::string& name) const;
    bool Decrypt(Method m, const std::string& in, uint32_t num, uint32_t gen, std::string& out) const;
    bool AuthenticateLegacy(const std::string& o, const std::string& u, uint32_t p, const std::string& id0);
    bool AuthenticateAes256(const std::string& u, const std::string& ue);

    int v_ = 0, r_ = 0;
    size_t keyLen_ = 5;
    bool encryptMetadata_ = true;
    Method stmMethod_ = kRc4, strMethod_ = kRc4;
    bool ok_ = false;
    std::string key_;
    PdfObj cf_;  // /CF dictionary (V4+)
};

}  // namespace pdfprops
