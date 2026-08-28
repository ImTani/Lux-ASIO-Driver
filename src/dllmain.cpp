#include <initguid.h>
#include "lux_asio.h"
#include "control_panel.h"
#include <windows.h>
#include <stdio.h>
#include <olectl.h>

// DllMain - runs before DllEntryPoint. We capture the DLL's HINSTANCE here
// because this is the authoritative handle for our embedded dialog resources.
// DllEntryPoint (from the ASIO SDK) is called separately by the CRT.
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        // Fix #1: Capture our DLL's HINSTANCE definitively here.
        // The ASIO SDK's DllEntryPoint also receives this but we cannot
        // rely on extern 'hinstance' being set before controlPanel() is called.
        InitControlPanelInstance(hinstDLL);
    }
    return TRUE;
}

// Extern functions provided by the ASIO SDK (register.cpp)
extern LONG RegisterAsioDriver(CLSID clsid, char *szdllname, char *szregname, char *szasiodesc, char *szthreadmodel);
extern LONG UnregisterAsioDriver(CLSID clsid, char *szdllname, char *szregname);

// Define the global factory templates array required by dllentry.cpp (from ASIO SDK)
CFactoryTemplate g_Templates[] = {
    {
        L"Lux ASIO Driver",           // Name
        &CLSID_LuxAsioDriver,         // CLSID
        LuxAsioDriver::CreateInstance,// Creation function
        nullptr                       // Initialization function (optional)
    }
};

int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

// Helper to get current DLL name
static void GetDllPath(char* path, size_t maxLen)
{
    HMODULE hModule = NULL;
    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetDllPath,
        &hModule);
    GetModuleFileNameA(hModule, path, (DWORD)maxLen);
}

// -------------------------------------------------------------------------
// COM Registration / Unregistration Entry Points
// -------------------------------------------------------------------------

STDAPI DllRegisterServer()
{
    char dllPath[MAX_PATH];
    GetDllPath(dllPath, MAX_PATH);

    char driverName[] = "Lux ASIO Driver";
    char driverDesc[] = "Lux ASIO Driver";
    char threadModel[] = "Apartment";

    LONG rc = RegisterAsioDriver(CLSID_LuxAsioDriver, "lux_asio.dll", driverName, driverDesc, threadModel);
    
    if (rc == 0)
        return S_OK;
    else
        return SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer()
{
    char dllPath[MAX_PATH];
    GetDllPath(dllPath, MAX_PATH);

    char driverName[] = "Lux ASIO Driver";

    LONG rc = UnregisterAsioDriver(CLSID_LuxAsioDriver, dllPath, driverName);
    
    if (rc == 0)
        return S_OK;
    else
        return SELFREG_E_CLASS;
}
