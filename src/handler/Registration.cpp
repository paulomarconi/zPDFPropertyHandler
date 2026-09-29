// DllRegisterServer / DllUnregisterServer.
//
// Values we replace (the .pdf property handler and the Explorer property lists)
// are backed up under our CLSID key as "1:<old value>" or "0:" (was absent) and
// restored on unregister, but only if they still hold our value.
// Our own properties (zPDF.Producer, zPDF.PdfVersion) are described by a property schema that is
// written next to the DLL and registered with PSRegisterPropertySchema.
#include <string>

#include <windows.h>
#include <propsys.h>
#include <shlobj.h>

#include "Common.h"

namespace {

const wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\" PDFPROPS_CLSID_STRING;
const wchar_t kInprocKey[] = L"Software\\Classes\\CLSID\\" PDFPROPS_CLSID_STRING L"\\InprocServer32";
const wchar_t kHandlerKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\.pdf";
const wchar_t kAssocKey[] = L"Software\\Classes\\SystemFileAssociations\\.pdf";

struct PropList {
    const wchar_t* name;
    const wchar_t* value;
};

const PropList kPropLists[] = {
    {L"FullDetails",
     L"prop:System.PropGroup.Description;System.Title;System.Subject;System.Keywords;System.Category;"
     L"System.Comment;System.PropGroup.Origin;System.Author;System.Company;System.ApplicationName;"
     L"System.Document.DateCreated;System.Document.DateSaved;System.PropGroup.Content;System.Document.PageCount;"
     L"zPDF.PdfVersion;zPDF.Producer;System.PropGroup.FileSystem;System.ItemNameDisplay;System.ItemTypeText;"
     L"System.ItemFolderPathDisplay;System.Size;System.DateCreated;System.DateModified;System.DateAccessed;"
     L"System.FileAttributes;System.OfflineAvailability;System.OfflineStatus;System.SharedWith;"
     L"System.FileOwner;System.ComputerName"},
    {L"PreviewDetails",
     L"prop:System.Title;System.Author;System.Subject;System.Keywords;System.Comment;System.Document.PageCount;"
     L"System.Document.DateCreated;System.Document.DateSaved;System.ApplicationName;zPDF.Producer;"
     L"System.DateModified;System.Size"},
    {L"InfoTip",
     L"prop:System.ItemTypeText;System.Title;System.Author;System.Document.PageCount;System.Size;"
     L"System.DateModified"},
};

bool GetString(const wchar_t* subkey, const wchar_t* value, std::wstring& out) {
    DWORD bytes = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return false;
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value, flags, nullptr, &buf[0], &bytes) != ERROR_SUCCESS)
        return false;
    buf.resize(wcslen(buf.c_str()));
    out = buf;
    return true;
}

LSTATUS SetString(const wchar_t* subkey, const wchar_t* value, const std::wstring& data) {
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, subkey, value, REG_SZ, data.c_str(),
                           static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
}

LSTATUS BackupAndSet(const wchar_t* subkey, const wchar_t* value, const std::wstring& backupName,
                     const std::wstring& ours) {
    std::wstring cur, existingBackup;
    bool has = GetString(subkey, value, cur);
    if (!(has && cur == ours) && !GetString(kClsidKey, backupName.c_str(), existingBackup)) {
        LSTATUS st = SetString(kClsidKey, backupName.c_str(), has ? L"1:" + cur : std::wstring(L"0:"));
        if (st != ERROR_SUCCESS) return st;
    }
    return SetString(subkey, value, ours);
}

// Deletes the key if it has neither values nor subkeys.
void DeleteKeyIfEmpty(const wchar_t* subkey) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    DWORD subkeys = 1, values = 1;
    LSTATUS st = RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr,
                                  nullptr, nullptr);
    RegCloseKey(h);
    if (st == ERROR_SUCCESS && subkeys == 0 && values == 0) RegDeleteKeyW(HKEY_LOCAL_MACHINE, subkey);
}

// Returns true if the value was ours and has been restored or removed.
bool Restore(const wchar_t* subkey, const wchar_t* value, const std::wstring& backupName, const std::wstring& ours) {
    std::wstring cur, backup;
    bool haveBackup = GetString(kClsidKey, backupName.c_str(), backup);
    if (!GetString(subkey, value, cur) || cur != ours) return false;
    if (haveBackup && backup.compare(0, 2, L"1:") == 0)
        SetString(subkey, value, backup.substr(2));
    else
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, subkey, value);
    return true;
}

const char kSchema[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\r\n"
    "  <propertyDescriptionList publisher=\"zPDFPropertyHandler\" product=\"zPDFPropertyHandler\">\r\n"
    "    <propertyDescription name=\"zPDF.Producer\" formatID=\"" PDFPROPS_FMTID_STRING "\" propID=\"2\">\r\n"
    "      <description>Application that produced the PDF file (PDF Producer)</description>\r\n"
    "      <searchInfo inInvertedIndex=\"true\" isColumn=\"true\" columnIndexType=\"OnDisk\" maxSize=\"512\"/>\r\n"
    "      <labelInfo label=\"PDF producer\"/>\r\n"
    "      <typeInfo type=\"String\" isInnate=\"true\" isViewable=\"true\" multipleValues=\"false\"/>\r\n"
    "      <displayInfo displayType=\"String\" defaultColumnWidth=\"30\"/>\r\n"
    "    </propertyDescription>\r\n"
    "    <propertyDescription name=\"zPDF.PdfVersion\" formatID=\"" PDFPROPS_FMTID_STRING "\" propID=\"3\">\r\n"
    "      <description>PDF specification version of the file, e.g. 1.7 or 2.0</description>\r\n"
    "      <searchInfo inInvertedIndex=\"false\" isColumn=\"true\" columnIndexType=\"OnDisk\" maxSize=\"16\"/>\r\n"
    "      <labelInfo label=\"PDF version\"/>\r\n"
    "      <typeInfo type=\"String\" isInnate=\"true\" isViewable=\"true\" multipleValues=\"false\"/>\r\n"
    "      <displayInfo displayType=\"String\" defaultColumnWidth=\"8\"/>\r\n"
    "    </propertyDescription>\r\n"
    "  </propertyDescriptionList>\r\n"
    "</schema>\r\n";

// The schema file lives next to the DLL (each architecture registers its own copy).
std::wstring SchemaPath() {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(g_hInstance, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return std::wstring();
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return p.substr(0, slash + 1) + L"zPDFPropertyHandler.propdesc";
}

HRESULT RegisterSchema() {
    std::wstring path = SchemaPath();
    if (path.empty()) return E_FAIL;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written = 0;
    BOOL ok = WriteFile(h, kSchema, sizeof(kSchema) - 1, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != sizeof(kSchema) - 1) return E_FAIL;
    return PSRegisterPropertySchema(path.c_str());
}

void UnregisterSchema() {
    std::wstring path = SchemaPath();
    if (path.empty()) return;
    PSUnregisterPropertySchema(path.c_str());
    DeleteFileW(path.c_str());
}

HRESULT ToHr(LSTATUS st) { return st == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(st); }

}  // namespace

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(g_hInstance, path, ARRAYSIZE(path));
    if (n == 0 || n >= ARRAYSIZE(path)) return HRESULT_FROM_WIN32(GetLastError());

    LSTATUS st = SetString(kClsidKey, nullptr, PDFPROPS_NAME);
    if (st != ERROR_SUCCESS) return ToHr(st);
    if ((st = SetString(kInprocKey, nullptr, path)) != ERROR_SUCCESS) return ToHr(st);
    if ((st = SetString(kInprocKey, L"ThreadingModel", L"Both")) != ERROR_SUCCESS) return ToHr(st);

    HRESULT hr = RegisterSchema();
    if (FAILED(hr)) return hr;

    st = BackupAndSet(kHandlerKey, nullptr, L"Backup.PropertyHandler", PDFPROPS_CLSID_STRING);
    if (st != ERROR_SUCCESS) return ToHr(st);

#ifdef _WIN64
    // Explorer property lists live in the shared (non-redirected) Classes view: set them
    // from the 64-bit build only, so a 32-bit registration can't change them.
    for (const PropList& p : kPropLists) {
        st = BackupAndSet(kAssocKey, p.name, std::wstring(L"Backup.") + p.name, p.value);
        if (st != ERROR_SUCCESS) return ToHr(st);
    }
#endif

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    // The PropertyHandlers\.pdf key usually only exists because of a handler: drop it once empty.
    if (Restore(kHandlerKey, nullptr, L"Backup.PropertyHandler", PDFPROPS_CLSID_STRING)) DeleteKeyIfEmpty(kHandlerKey);
#ifdef _WIN64
    for (const PropList& p : kPropLists) Restore(kAssocKey, p.name, std::wstring(L"Backup.") + p.name, p.value);
#endif
    UnregisterSchema();
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : ToHr(st);
}
