#pragma once

#include <windows.h>

// {58E789DD-8A3B-417E-B407-25E17A809717}
static const CLSID CLSID_PdfPropertyHandler = {0x58e789dd, 0x8a3b, 0x417e,
                                               {0xb4, 0x07, 0x25, 0xe1, 0x7a, 0x80, 0x97, 0x17}};
#define PDFPROPS_CLSID_STRING L"{58E789DD-8A3B-417E-B407-25E17A809717}"
#define PDFPROPS_NAME L"zPDF Property Handler"

// Format ID of our own properties (zPDF.Producer = 2, zPDF.PdfVersion = 3), see Registration.cpp.
// {8864E2EC-52D5-4DF0-B248-9D97A80814DB}
#define PDFPROPS_FMTID {0x8864E2EC, 0x52D5, 0x4DF0, {0xB2, 0x48, 0x9D, 0x97, 0xA8, 0x08, 0x14, 0xDB}}
#define PDFPROPS_FMTID_STRING "{8864E2EC-52D5-4DF0-B248-9D97A80814DB}"

extern HINSTANCE g_hInstance;
extern LONG g_moduleRefs;

inline void ModuleAddRef() { InterlockedIncrement(&g_moduleRefs); }
inline void ModuleRelease() { InterlockedDecrement(&g_moduleRefs); }

HRESULT PdfPropertyHandler_CreateInstance(REFIID riid, void** ppv);
