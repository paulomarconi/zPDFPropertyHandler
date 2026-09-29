#include "PdfSecurity.h"

#include <algorithm>

#include "Crypto.h"

namespace pdfprops {

namespace {

const unsigned char kPad[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                                0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                                0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

std::string PadString() { return std::string(reinterpret_cast<const char*>(kPad), 32); }

// Algorithm 2.B (ISO 32000-2) for the user password (no udata).
std::string Hash2B(const std::string& pw, const std::string& salt) {
    std::string k = crypto::Sha256(pw + salt);
    for (int round = 0;; ++round) {
        std::string unit = pw + k;
        std::string k1;
        k1.reserve(unit.size() * 64);
        for (int i = 0; i < 64; ++i) k1 += unit;
        std::string e = crypto::AesCbcEncrypt(k.substr(0, 16), k.substr(16, 16), k1);
        unsigned sum = 0;
        for (int i = 0; i < 16; ++i) sum += static_cast<unsigned char>(e[i]);
        switch (sum % 3) {
            case 0: k = crypto::Sha256(e); break;
            case 1: k = crypto::Sha384(e); break;
            default: k = crypto::Sha512(e); break;
        }
        if (round >= 63 && static_cast<unsigned char>(e.back()) <= round - 31) break;
        if (round > 1000) break;  // cannot happen for valid input; defensive bound
    }
    return k.substr(0, 32);
}

}  // namespace

std::unique_ptr<SecurityHandler> SecurityHandler::Create(PdfDocument& doc, const PdfObj& enc,
                                                         const std::string& id0) {
    if (!enc.IsDict()) return nullptr;
    PdfObj filter = doc.GetResolved(enc, "Filter");
    if (!filter.IsName("Standard")) return nullptr;
    std::unique_ptr<SecurityHandler> h(new SecurityHandler());
    PdfObj v = doc.GetResolved(enc, "V"), r = doc.GetResolved(enc, "R");
    h->v_ = v.IsInt() ? static_cast<int>(v.integer) : 0;
    h->r_ = r.IsInt() ? static_cast<int>(r.integer) : 2;
    PdfObj len = doc.GetResolved(enc, "Length");
    int bits = len.IsInt() ? static_cast<int>(len.integer) : (h->v_ >= 4 ? 128 : 40);
    if (bits < 40 || bits > 128 || bits % 8) bits = h->v_ >= 4 ? 128 : 40;
    h->keyLen_ = h->r_ == 2 ? 5 : static_cast<size_t>(bits / 8);
    PdfObj em = doc.GetResolved(enc, "EncryptMetadata");
    if (em.type == PdfType::Bool) h->encryptMetadata_ = em.boolean;

    if (h->v_ >= 4) {
        h->cf_ = doc.GetResolved(enc, "CF");
        PdfObj stmf = doc.GetResolved(enc, "StmF"), strf = doc.GetResolved(enc, "StrF");
        h->stmMethod_ = h->MethodFor(doc, h->cf_, stmf.IsName() ? stmf.str : "Identity");
        h->strMethod_ = h->MethodFor(doc, h->cf_, strf.IsName() ? strf.str : "Identity");
    } else {
        h->stmMethod_ = h->strMethod_ = kRc4;
    }

    PdfObj o = doc.GetResolved(enc, "O"), u = doc.GetResolved(enc, "U");
    if (h->r_ >= 5) {
        h->keyLen_ = 32;
        PdfObj ue = doc.GetResolved(enc, "UE");
        if (u.IsString() && ue.IsString()) h->ok_ = h->AuthenticateAes256(u.str, ue.str);
    } else {
        PdfObj p = doc.GetResolved(enc, "P");
        uint32_t pv = p.IsInt() ? static_cast<uint32_t>(p.integer) : 0xFFFFFFFCu;
        if (o.IsString() && u.IsString()) h->ok_ = h->AuthenticateLegacy(o.str, u.str, pv, id0);
    }
    return h;
}

SecurityHandler::Method SecurityHandler::MethodFor(PdfDocument& doc, const PdfObj& cf,
                                                   const std::string& name) const {
    if (name == "Identity") return kNone;
    if (!cf.IsDict()) return kNone;
    PdfObj f = doc.GetResolved(cf, name.c_str());
    if (!f.IsDict()) return kNone;
    PdfObj cfm = doc.GetResolved(f, "CFM");
    if (cfm.IsName("V2")) return kRc4;
    if (cfm.IsName("AESV2")) return kAesV2;
    if (cfm.IsName("AESV3")) return kAesV3;
    return kNone;
}

bool SecurityHandler::AuthenticateLegacy(const std::string& o, const std::string& u, uint32_t p,
                                         const std::string& id0) {
    std::string pad = PadString();
    std::string in = pad + o.substr(0, 32);
    for (int i = 0; i < 4; ++i) in += static_cast<char>((p >> (8 * i)) & 0xff);
    in += id0;
    if (r_ >= 4 && !encryptMetadata_) in += std::string(4, '\xff');
    std::string key = crypto::Md5(in);
    size_t n = std::min<size_t>(keyLen_, 16);
    if (r_ >= 3)
        for (int i = 0; i < 50; ++i) key = crypto::Md5(key.substr(0, n));
    key_ = key.substr(0, n);

    if (r_ == 2) {
        std::string check = crypto::Rc4(key_, pad);
        return u.size() >= 32 && check == u.substr(0, 32);
    }
    std::string x = crypto::Rc4(key_, crypto::Md5(pad + id0));
    for (int i = 1; i <= 19; ++i) {
        std::string k = key_;
        for (char& c : k) c = static_cast<char>(c ^ i);
        x = crypto::Rc4(k, x);
    }
    return u.size() >= 16 && x == u.substr(0, 16);
}

bool SecurityHandler::AuthenticateAes256(const std::string& u, const std::string& ue) {
    if (u.size() < 48 || ue.size() < 32) return false;
    std::string vsalt = u.substr(32, 8), ksalt = u.substr(40, 8);
    std::string hash, ikey;
    if (r_ == 5) {
        hash = crypto::Sha256(vsalt);
        ikey = crypto::Sha256(ksalt);
    } else {
        hash = Hash2B("", vsalt);
        ikey = Hash2B("", ksalt);
    }
    if (hash != u.substr(0, 32)) return false;
    key_ = crypto::AesCbcDecrypt(ikey, std::string(16, '\0'), ue.substr(0, 32));
    return key_.size() == 32;
}

std::string SecurityHandler::Info() const {
    auto name = [](Method m) {
        switch (m) {
            case kRc4: return "RC4";
            case kAesV2: return "AES-128";
            case kAesV3: return "AES-256";
            default: return "Identity";
        }
    };
    Method m = stmMethod_ != kNone ? stmMethod_ : strMethod_;
    std::string s = name(m);
    if (m == kRc4) s += " " + std::to_string(keyLen_ * 8) + "-bit";
    s += " (V" + std::to_string(v_) + " R" + std::to_string(r_) + ")";
    if (!encryptMetadata_) s += ", metadata not encrypted";
    s += ok_ ? ", no user password" : ", user password required";
    return s;
}

bool SecurityHandler::Decrypt(Method m, const std::string& in, uint32_t num, uint32_t gen, std::string& out) const {
    out.clear();
    if (m == kNone) {
        out = in;
        return true;
    }
    if (!ok_) return false;
    std::string key;
    if (m == kAesV3) {
        key = key_;
    } else {
        std::string k = key_;
        k += static_cast<char>(num & 0xff);
        k += static_cast<char>((num >> 8) & 0xff);
        k += static_cast<char>((num >> 16) & 0xff);
        k += static_cast<char>(gen & 0xff);
        k += static_cast<char>((gen >> 8) & 0xff);
        if (m == kAesV2) k += "sAlT";
        key = crypto::Md5(k).substr(0, std::min<size_t>(key_.size() + 5, 16));
    }
    if (m == kRc4) {
        out = crypto::Rc4(key, in);
        return true;
    }
    if (in.size() < 32) return in.size() == 16;  // just an IV: empty string
    std::string body = in.substr(16);
    body.resize(body.size() & ~static_cast<size_t>(15));
    out = crypto::AesCbcDecrypt(key, in.substr(0, 16), body);
    if (!out.empty()) {
        unsigned padLen = static_cast<unsigned char>(out.back());
        if (padLen >= 1 && padLen <= 16 && padLen <= out.size()) out.resize(out.size() - padLen);
    }
    return true;
}

bool SecurityHandler::DecryptString(const std::string& in, uint32_t num, uint32_t gen, std::string& out) const {
    return Decrypt(strMethod_, in, num, gen, out);
}

bool SecurityHandler::DecryptStream(const std::string& in, uint32_t num, uint32_t gen, const PdfObj& dict,
                                    PdfDocument& doc, std::string& out) const {
    Method m = stmMethod_;
    PdfObj type = doc.GetResolved(dict, "Type");
    if (type.IsName("XRef")) m = kNone;
    if (type.IsName("Metadata") && !encryptMetadata_) m = kNone;
    // An explicit /Crypt filter selects the crypt filter by name (default Identity).
    PdfObj filter = doc.GetResolved(dict, "Filter");
    PdfObj first = filter.IsArray() ? doc.Resolve(filter.At(0)) : filter;
    if (first.IsName("Crypt")) {
        PdfObj parms = doc.GetResolved(dict, "DecodeParms");
        if (parms.IsArray()) parms = doc.Resolve(parms.At(0));
        PdfObj name = parms.IsDict() ? doc.GetResolved(parms, "Name") : PdfObj();
        m = MethodFor(doc, cf_, name.IsName() ? name.str : "Identity");
    }
    return Decrypt(m, in, num, gen, out);
}

}  // namespace pdfprops
