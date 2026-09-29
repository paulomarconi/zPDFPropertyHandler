// Property handler for .pdf: IInitializeWithStream + IPropertyStore + IPropertyStoreCapabilities.
// Reading works for every file; editing (Title, Subject, Authors, Tags, Comments, Categories,
// Company) is offered when the stream is opened for writing and the file can take an incremental
// update (not encrypted, signed or damaged). Commit appends the update; existing bytes never change.
#include <new>
#include <string>
#include <vector>

#include <windows.h>
#include <shlwapi.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl.h>

#include "../core/PdfMetadata.h"
#include "../core/PdfWriter.h"
#include "Common.h"

using namespace pdfprops;

namespace {

// Property keys (values from the Windows SDK's propkey.h; defined here because
// MinGW's header lacks several of them).
#define SUMMARY_FMTID {0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}
#define DOCSUMMARY_FMTID {0xD5CDD502, 0x2E9C, 0x101B, {0x93, 0x97, 0x08, 0x00, 0x2B, 0x2C, 0xF9, 0xAE}}
const PROPERTYKEY kTitle = {SUMMARY_FMTID, 2};
const PROPERTYKEY kSubject = {SUMMARY_FMTID, 3};
const PROPERTYKEY kAuthor = {SUMMARY_FMTID, 4};
const PROPERTYKEY kKeywords = {SUMMARY_FMTID, 5};
const PROPERTYKEY kComment = {SUMMARY_FMTID, 6};
const PROPERTYKEY kDocDateCreated = {SUMMARY_FMTID, 12};
const PROPERTYKEY kDocDateSaved = {SUMMARY_FMTID, 13};
const PROPERTYKEY kDocPageCount = {SUMMARY_FMTID, 14};
const PROPERTYKEY kApplicationName = {SUMMARY_FMTID, 18};
const PROPERTYKEY kCategory = {DOCSUMMARY_FMTID, 2};
const PROPERTYKEY kCompany = {DOCSUMMARY_FMTID, 15};
const PROPERTYKEY kDocVersion = {DOCSUMMARY_FMTID, 29};
// Our own properties, described by the schema registered in Registration.cpp.
const PROPERTYKEY kPdfProducer = {PDFPROPS_FMTID, 2};
const PROPERTYKEY kPdfVersion = {PDFPROPS_FMTID, 3};

// ByteSource over an IStream (not owned).
class StreamSource : public ByteSource {
public:
    explicit StreamSource(IStream* s) : s_(s) {
        STATSTG st = {};
        if (SUCCEEDED(s_->Stat(&st, STATFLAG_NONAME))) {
            size_ = st.cbSize.QuadPart;
        } else {
            ULARGE_INTEGER end = {};
            LARGE_INTEGER zero = {};
            if (SUCCEEDED(s_->Seek(zero, STREAM_SEEK_END, &end))) size_ = end.QuadPart;
        }
    }
    uint64_t Size() override { return size_; }
    size_t ReadAt(uint64_t offset, void* buf, size_t len) override {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (FAILED(s_->Seek(pos, STREAM_SEEK_SET, nullptr))) return 0;
        ULONG got = 0;
        HRESULT hr = s_->Read(buf, static_cast<ULONG>(len > 0x7fffffff ? 0x7fffffff : len), &got);
        return FAILED(hr) ? 0 : got;
    }

private:
    IStream* s_;
    uint64_t size_ = 0;
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

bool DateToFileTime(const PdfDate& d, FILETIME& ft) {
    if (!d.valid || d.year < 1601) return false;
    if (d.hasTz) {
        int64_t unix = d.ToUnixSeconds();
        int64_t ticks = (unix + 11644473600LL) * 10000000LL;
        if (ticks < 0) return false;
        ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xffffffff);
        ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
        return true;
    }
    SYSTEMTIME local = {};
    local.wYear = static_cast<WORD>(d.year);
    local.wMonth = static_cast<WORD>(d.month);
    local.wDay = static_cast<WORD>(d.day);
    local.wHour = static_cast<WORD>(d.hour);
    local.wMinute = static_cast<WORD>(d.minute);
    local.wSecond = static_cast<WORD>(d.second);
    SYSTEMTIME utc;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) utc = local;
    return SystemTimeToFileTime(&utc, &ft) != FALSE;
}

int64_t UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t ticks = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return ticks / 10000000LL - 11644473600LL;
}

// Converts the value Explorer sends to UTF-8; lists are joined with "; ".
HRESULT PropVariantToUtf8(REFPROPVARIANT pv, std::string& out) {
    out.clear();
    auto append = [&](const wchar_t* w) {
        std::string s = Narrow(w);
        if (s.empty()) return;
        if (!out.empty()) out += "; ";
        out += s;
    };
    switch (pv.vt) {
        case VT_EMPTY:
        case VT_NULL: return S_OK;
        case VT_LPWSTR: append(pv.pwszVal); return S_OK;
        case VT_BSTR: append(pv.bstrVal); return S_OK;
        case VT_VECTOR | VT_LPWSTR:
            for (ULONG i = 0; i < pv.calpwstr.cElems; ++i) append(pv.calpwstr.pElems[i]);
            return S_OK;
        case VT_VECTOR | VT_BSTR:
            for (ULONG i = 0; i < pv.cabstr.cElems; ++i) append(pv.cabstr.pElems[i]);
            return S_OK;
        default: {
            PWSTR s = nullptr;
            HRESULT hr = PropVariantToStringAlloc(pv, &s);
            if (SUCCEEDED(hr)) append(s);
            CoTaskMemFree(s);
            return hr;
        }
    }
}

class PdfPropertyHandler : public IInitializeWithStream, public IPropertyStore, public IPropertyStoreCapabilities {
public:
    PdfPropertyHandler() { ModuleAddRef(); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream)
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IPropertyStore)
            *ppv = static_cast<IPropertyStore*>(this);
        else if (riid == IID_IPropertyStoreCapabilities)
            *ppv = static_cast<IPropertyStoreCapabilities*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override {
        if (!stream) return E_INVALIDARG;
        if (cache_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        IPropertyStoreCache* cache = nullptr;
        HRESULT hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&cache));
        if (FAILED(hr)) return hr;
        try {
            StreamSource src(stream);
            PdfMetadata md;
            if (ExtractMetadata(src, md)) Fill(cache, md);
            if ((mode & (STGM_WRITE | STGM_READWRITE)) && CanUpdateMetadata(src) == UpdateStatus::kOk) {
                stream_ = stream;
                stream_->AddRef();
            }
        } catch (...) {
            // Parsing failures (including bad_alloc) leave whatever was collected; never throw across COM.
        }
        cache_ = cache;
        return S_OK;
    }

    // IPropertyStore
    IFACEMETHODIMP GetCount(DWORD* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return cache_ ? cache_->GetCount(count) : E_UNEXPECTED;
    }
    IFACEMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override {
        if (!key) return E_POINTER;
        return cache_ ? cache_->GetAt(index, key) : E_UNEXPECTED;
    }
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (!pv) return E_POINTER;
        PropVariantInit(pv);
        return cache_ ? cache_->GetValue(key, pv) : E_UNEXPECTED;
    }
    IFACEMETHODIMP SetValue(REFPROPERTYKEY key, REFPROPVARIANT pv) override {
        if (!cache_) return E_UNEXPECTED;
        MetadataEdits::Field* field = EditableField(key);
        if (!field || !stream_) return STG_E_ACCESSDENIED;
        try {
            std::string value;
            HRESULT hr = PropVariantToUtf8(pv, value);
            if (FAILED(hr)) return hr;
            field->set = true;
            field->value = value;
            return cache_->SetValueAndState(key, &pv, PSC_DIRTY);
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    IFACEMETHODIMP Commit() override {
        if (!cache_) return E_UNEXPECTED;
        if (!stream_) return STG_E_ACCESSDENIED;
        if (!edits_.Any()) return S_OK;
        try {
            edits_.nowUnix = UnixNow();
            std::string append;
            UpdateStatus st;
            {
                StreamSource src(stream_);
                st = BuildMetadataUpdate(src, edits_, append);
            }
            if (st != UpdateStatus::kOk) return STG_E_ACCESSDENIED;
            LARGE_INTEGER zero = {};
            ULARGE_INTEGER end = {};
            HRESULT hr = stream_->Seek(zero, STREAM_SEEK_END, &end);
            if (FAILED(hr)) return hr;
            size_t done = 0;
            while (SUCCEEDED(hr) && done < append.size()) {
                ULONG written = 0;
                hr = stream_->Write(append.data() + done, static_cast<ULONG>(append.size() - done), &written);
                if (SUCCEEDED(hr) && written == 0) hr = STG_E_WRITEFAULT;
                done += written;
            }
            if (SUCCEEDED(hr)) hr = stream_->Commit(STGC_DEFAULT);
            if (FAILED(hr)) {
                stream_->SetSize(end);  // don't leave a partial update behind
                return hr;
            }
            edits_ = MetadataEdits();
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    // IPropertyStoreCapabilities
    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY key) override {
        return stream_ && EditableField(key) ? S_OK : S_FALSE;
    }

private:
    virtual ~PdfPropertyHandler() {
        if (cache_) cache_->Release();
        if (stream_) stream_->Release();
        ModuleRelease();
    }

    MetadataEdits::Field* EditableField(REFPROPERTYKEY key) {
        if (key == kTitle) return &edits_.title;
        if (key == kSubject) return &edits_.subject;
        if (key == kAuthor) return &edits_.author;
        if (key == kKeywords) return &edits_.keywords;
        if (key == kComment) return &edits_.comments;
        if (key == kCategory) return &edits_.category;
        if (key == kCompany) return &edits_.company;
        return nullptr;
    }

    static void SetValue(IPropertyStoreCache* c, REFPROPERTYKEY key, PROPVARIANT& pv, HRESULT hr) {
        if (SUCCEEDED(hr)) {
            c->SetValueAndState(key, &pv, PSC_NORMAL);
            PropVariantClear(&pv);
        }
    }

    static void SetString(IPropertyStoreCache* c, REFPROPERTYKEY key, const std::string& v) {
        if (v.empty()) return;
        std::wstring w = Widen(v);
        PROPVARIANT pv;
        SetValue(c, key, pv, InitPropVariantFromString(w.c_str(), &pv));
    }

    static void SetVector(IPropertyStoreCache* c, REFPROPERTYKEY key, const std::string& v, bool allowComma) {
        std::vector<std::string> parts = SplitList(v, allowComma);
        if (parts.empty()) return;
        std::vector<std::wstring> w;
        std::vector<PCWSTR> ptrs;
        for (const auto& p : parts) w.push_back(Widen(p));
        for (const auto& s : w) ptrs.push_back(s.c_str());
        PROPVARIANT pv;
        SetValue(c, key, pv, InitPropVariantFromStringVector(ptrs.data(), static_cast<ULONG>(ptrs.size()), &pv));
    }

    static void SetDate(IPropertyStoreCache* c, REFPROPERTYKEY key, const PdfDate& d) {
        FILETIME ft;
        if (!DateToFileTime(d, ft)) return;
        PROPVARIANT pv;
        SetValue(c, key, pv, InitPropVariantFromFileTime(&ft, &pv));
    }

    static void Fill(IPropertyStoreCache* c, const PdfMetadata& md) {
        SetString(c, kTitle, md.title);
        SetVector(c, kAuthor, md.author, false);
        SetString(c, kSubject, md.subject);
        SetVector(c, kKeywords, md.keywords, true);
        SetString(c, kApplicationName, md.creator);
        SetString(c, kCompany, md.company);
        SetVector(c, kCategory, md.category, false);
        SetString(c, kComment, md.comments);
        SetDate(c, kDocDateCreated, md.created);
        SetDate(c, kDocDateSaved, md.modified);
        if (md.pageCount >= 0) {
            PROPVARIANT pv;
            SetValue(c, kDocPageCount, pv, InitPropVariantFromInt32(md.pageCount, &pv));
        }
        SetString(c, kDocVersion, md.version.empty() ? std::string() : "PDF " + md.version);
        SetString(c, kPdfProducer, md.producer);
        SetString(c, kPdfVersion, md.version);
    }

    LONG refs_ = 1;
    IPropertyStoreCache* cache_ = nullptr;
    IStream* stream_ = nullptr;  // held only when opened for writing and the file can be updated
    MetadataEdits edits_;
};

}  // namespace

HRESULT PdfPropertyHandler_CreateInstance(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    PdfPropertyHandler* h = new (std::nothrow) PdfPropertyHandler();
    if (!h) return E_OUTOFMEMORY;
    HRESULT hr = h->QueryInterface(riid, ppv);
    h->Release();
    return hr;
}
