// DLL entry points and class factory.
#include <new>

#include <windows.h>
#include <unknwn.h>

#include "Common.h"

HINSTANCE g_hInstance = nullptr;
LONG g_moduleRefs = 0;

namespace {

class ClassFactory : public IClassFactory {
public:
    ClassFactory() { ModuleAddRef(); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return PdfPropertyHandler_CreateInstance(riid, ppv);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock)
            ModuleAddRef();
        else
            ModuleRelease();
        return S_OK;
    }

private:
    virtual ~ClassFactory() { ModuleRelease(); }
    LONG refs_ = 1;
};

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInstance = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_PdfPropertyHandler) return CLASS_E_CLASSNOTAVAILABLE;
    ClassFactory* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_moduleRefs == 0 ? S_OK : S_FALSE; }
