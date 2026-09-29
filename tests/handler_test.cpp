// Loads zPDFPropertyHandler.dll directly (no registration needed), runs the
// handler on each file through IInitializeWithStream and prints the property
// store the way Explorer would format it.
//   handler_test path\to\zPDFPropertyHandler.dll file.pdf...
//   handler_test path\to\zPDFPropertyHandler.dll --write file.pdf...   (MODIFIES the files: use copies)
//   handler_test --shell [--write] file.pdf...   (uses the REGISTERED handler, like Explorer)
// --write opens each file read-write, sets Title / Authors (vector) / clears Subject through
// IPropertyStore::SetValue, commits, then prints the result.
// --shell goes through SHGetPropertyStoreFromParsingName, so it tests the installed handler, the
// registration and the property schema (zPDF.Producer / zPDF.PdfVersion) together.
#include <cstdio>
#include <cwchar>
#include <initializer_list>

#include <windows.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shobjidl.h>

static const CLSID CLSID_Handler = {0x58e789dd, 0x8a3b, 0x417e, {0xb4, 0x07, 0x25, 0xe1, 0x7a, 0x80, 0x97, 0x17}};

typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, void**);

#define SUMMARY_FMTID {0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}
static const PROPERTYKEY kTitle = {SUMMARY_FMTID, 2};
static const PROPERTYKEY kSubject = {SUMMARY_FMTID, 3};
static const PROPERTYKEY kAuthor = {SUMMARY_FMTID, 4};
static const PROPERTYKEY kPageCount = {SUMMARY_FMTID, 14};

// Prints "  write: writable=yes commit=0x..." or "  write: writable=no"; returns false on unexpected results.
static bool WriteTest(IClassFactory* factory, const wchar_t* path) {
    IStream* stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path, STGM_READWRITE | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
    if (FAILED(hr)) {
        wprintf(L"  write: open failed 0x%08lx\n", hr);
        return false;
    }
    bool ok = true;
    IInitializeWithStream* init = nullptr;
    IPropertyStore* store = nullptr;
    IPropertyStoreCapabilities* caps = nullptr;
    hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&init));
    if (SUCCEEDED(hr)) hr = init->Initialize(stream, STGM_READWRITE);
    if (SUCCEEDED(hr)) hr = init->QueryInterface(IID_PPV_ARGS(&store));
    if (SUCCEEDED(hr)) hr = init->QueryInterface(IID_PPV_ARGS(&caps));
    if (FAILED(hr)) {
        wprintf(L"  write: handler failed 0x%08lx\n", hr);
        ok = false;
    } else if (caps->IsPropertyWritable(kTitle) != S_OK) {
        wprintf(L"  write: writable=no\n");
        PROPVARIANT pv;
        InitPropVariantFromString(L"x", &pv);
        if (store->SetValue(kTitle, pv) != STG_E_ACCESSDENIED) {
            wprintf(L"  write: SetValue accepted on a read-only file\n");
            ok = false;
        }
        PropVariantClear(&pv);
    } else {
        if (caps->IsPropertyWritable(kPageCount) == S_OK) {
            wprintf(L"  write: page count reported writable\n");
            ok = false;
        }
        PROPVARIANT title, authors, empty, pages;
        InitPropVariantFromString(L"COM \u2713 edit", &title);
        PCWSTR names[] = {L"Ann", L"Bob"};
        InitPropVariantFromStringVector(names, 2, &authors);
        PropVariantInit(&empty);
        InitPropVariantFromInt32(99, &pages);
        HRESULT h1 = store->SetValue(kTitle, title), h2 = store->SetValue(kAuthor, authors),
                h3 = store->SetValue(kSubject, empty), h4 = store->SetValue(kPageCount, pages);
        if (FAILED(h1) || FAILED(h2) || FAILED(h3) || h4 != STG_E_ACCESSDENIED) {
            wprintf(L"  write: SetValue results 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n", h1, h2, h3, h4);
            ok = false;
        }
        hr = store->Commit();
        wprintf(L"  write: writable=yes commit=0x%08lx\n", hr);
        if (FAILED(hr)) ok = false;
        PropVariantClear(&title);
        PropVariantClear(&authors);
        PropVariantClear(&pages);
    }
    if (caps) caps->Release();
    if (store) store->Release();
    if (init) init->Release();
    stream->Release();
    return ok;
}

static void PrintByName(IPropertyStore* store, PCWSTR canonical) {
    PROPERTYKEY key;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    PWSTR display = nullptr;
    if (FAILED(PSGetPropertyKeyFromName(canonical, &key))) {
        wprintf(L"  %-24ls (not registered)\n", canonical);
        return;
    }
    if (SUCCEEDED(store->GetValue(key, &pv)) && pv.vt != VT_EMPTY) PSFormatForDisplayAlloc(key, pv, PDFF_DEFAULT, &display);
    char buf[2048];
    WideCharToMultiByte(CP_UTF8, 0, display ? display : L"", -1, buf, sizeof buf, nullptr, nullptr);
    wprintf(L"  %-24ls ", canonical);
    fflush(stdout);
    printf("%s\n", buf);
    fflush(stdout);
    CoTaskMemFree(display);
    PropVariantClear(&pv);
}

static int ShellTest(int argc, wchar_t** argv) {
    bool write = argc > 2 && wcscmp(argv[2], L"--write") == 0;
    int rc = 0;
    for (int i = write ? 3 : 2; i < argc; ++i) {
        wchar_t full[MAX_PATH * 4];
        GetFullPathNameW(argv[i], ARRAYSIZE(full), full, nullptr);
        wprintf(L"%ls\n", full);
        if (write) {
            IPropertyStore* store = nullptr;
            HRESULT hr = SHGetPropertyStoreFromParsingName(full, nullptr, GPS_READWRITE, IID_PPV_ARGS(&store));
            if (FAILED(hr)) {
                wprintf(L"  GPS_READWRITE failed 0x%08lx\n", hr);
                rc = 1;
            } else {
                PROPVARIANT title, authors;
                InitPropVariantFromString(L"Shell \u2713 edit", &title);
                PCWSTR names[] = {L"Ann", L"Bob"};
                InitPropVariantFromStringVector(names, 2, &authors);
                HRESULT h1 = store->SetValue(kTitle, title), h2 = store->SetValue(kAuthor, authors);
                hr = FAILED(h1) ? h1 : FAILED(h2) ? h2 : store->Commit();
                wprintf(L"  shell write: 0x%08lx\n", hr);
                if (FAILED(hr)) rc = 1;
                PropVariantClear(&title);
                PropVariantClear(&authors);
                store->Release();
            }
        }
        IPropertyStore* store = nullptr;
        HRESULT hr = SHGetPropertyStoreFromParsingName(full, nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store));
        if (FAILED(hr)) {
            wprintf(L"  GPS_DEFAULT failed 0x%08lx\n", hr);
            rc = 1;
            continue;
        }
        for (PCWSTR n : {L"System.Title", L"System.Author", L"System.Document.PageCount", L"zPDF.Producer",
                         L"zPDF.PdfVersion", L"System.Document.DateSaved"})
            PrintByName(store, n);
        store->Release();
    }
    return rc;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: handler_test dll file.pdf...\n");
        return 2;
    }
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (wcscmp(argv[1], L"--shell") == 0) {
        int r = ShellTest(argc, argv);
        CoUninitialize();
        return r;
    }
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
        fwprintf(stderr, L"cannot load %ls (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(reinterpret_cast<void*>(GetProcAddress(dll, "DllGetClassObject")));
    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject ? getClassObject(CLSID_Handler, IID_PPV_ARGS(&factory)) : E_FAIL;
    if (FAILED(hr)) {
        fwprintf(stderr, L"DllGetClassObject failed 0x%08lx\n", hr);
        return 1;
    }
    int rc = 0;
    bool write = argc > 3 && wcscmp(argv[2], L"--write") == 0;
    int first = write ? 3 : 2;
    for (int i = first; i < argc; ++i) {
        wprintf(L"%ls\n", argv[i]);
        fflush(stdout);
        if (write && !WriteTest(factory, argv[i])) rc = 1;
        fflush(stdout);
        IStream* stream = nullptr;
        hr = SHCreateStreamOnFileEx(argv[i], STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream);
        if (FAILED(hr)) {
            wprintf(L"  open failed 0x%08lx\n", hr);
            rc = 1;
            continue;
        }
        IInitializeWithStream* init = nullptr;
        IPropertyStore* store = nullptr;
        hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&init));
        if (SUCCEEDED(hr)) hr = init->Initialize(stream, STGM_READ);
        if (SUCCEEDED(hr)) hr = init->QueryInterface(IID_PPV_ARGS(&store));
        if (FAILED(hr)) {
            wprintf(L"  handler failed 0x%08lx\n", hr);
            rc = 1;
        } else {
            // Second Initialize must be refused.
            if (init->Initialize(stream, STGM_READ) != HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED)) {
                wprintf(L"  second Initialize not refused\n");
                rc = 1;
            }
            PROPVARIANT dummy;
            PropVariantInit(&dummy);
            PROPERTYKEY k0 = {};
            DWORD count = 0;
            store->GetCount(&count);
            if (count > 0) store->GetAt(0, &k0);
            if (store->SetValue(k0, dummy) != STG_E_ACCESSDENIED || store->Commit() != STG_E_ACCESSDENIED) {
                wprintf(L"  store is not read-only\n");
                rc = 1;
            }
            for (DWORD k = 0; k < count; ++k) {
                PROPERTYKEY key;
                if (FAILED(store->GetAt(k, &key))) continue;
                PROPVARIANT pv;
                PropVariantInit(&pv);
                store->GetValue(key, &pv);
                PWSTR name = nullptr, display = nullptr;
                PSGetNameFromPropertyKey(key, &name);
                IPropertyDescription* desc = nullptr;
                if (SUCCEEDED(PSGetPropertyDescription(key, IID_PPV_ARGS(&desc)))) {
                    desc->FormatForDisplay(pv, PDFF_DEFAULT, &display);
                    desc->Release();
                }
                char buf[4096];
                WideCharToMultiByte(CP_UTF8, 0, display ? display : L"?", -1, buf, sizeof buf, nullptr, nullptr);
                char nbuf[256];
                WideCharToMultiByte(CP_UTF8, 0, name ? name : L"?", -1, nbuf, sizeof nbuf, nullptr, nullptr);
                printf("  %-28s (vt=%-4u) %s\n", nbuf, pv.vt, buf);
                fflush(stdout);
                CoTaskMemFree(name);
                CoTaskMemFree(display);
                PropVariantClear(&pv);
            }
            store->Release();
        }
        if (init) init->Release();
        stream->Release();
    }
    factory->Release();
    auto canUnload = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "DllCanUnloadNow")));
    if (canUnload && canUnload() != S_OK) {
        wprintf(L"DllCanUnloadNow: objects leaked\n");
        rc = 1;
    }
    FreeLibrary(dll);
    CoUninitialize();
    return rc;
}
