#include <initguid.h>
#include "lux_asio.h"
#include <windows.h>
#include <stdio.h>
#include <olectl.h>

extern LONG RegisterAsioDriver(CLSID clsid, char *szdllname, char *szregname, char *szasiodesc, char *szthreadmodel);
extern LONG UnregisterAsioDriver(CLSID clsid, char *szdllname, char *szregname);

CFactoryTemplate g_Templates[] = {
    {
        L"Lux ASIO Driver",
        &CLSID_LuxAsioDriver,
        LuxAsioDriver::CreateInstance,
        nullptr
    }
};

int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

static void GetDllPath(char* path, size_t maxLen)
{
    HMODULE hModule = NULL;
    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetDllPath,
        &hModule);
    GetModuleFileNameA(hModule, path, (DWORD)maxLen);
}

STDAPI DllRegisterServer()
{
    char dllPath[MAX_PATH];
    GetDllPath(dllPath, MAX_PATH);

    char driverName[] = "Lux ASIO Driver";
    char driverDesc[] = "Lux ASIO Driver";
    char threadModel[] = "Apartment";

    LONG rc = RegisterAsioDriver(CLSID_LuxAsioDriver, "lux_asio.dll", driverName, driverDesc, threadModel);
    return (rc == 0) ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer()
{
    char dllPath[MAX_PATH];
    GetDllPath(dllPath, MAX_PATH);

    char driverName[] = "Lux ASIO Driver";
    LONG rc = UnregisterAsioDriver(CLSID_LuxAsioDriver, dllPath, driverName);
    return (rc == 0) ? S_OK : SELFREG_E_CLASS;
}
