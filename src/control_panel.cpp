#include "control_panel.h"
#include "resource.h"
#include "settings.h"
#include "wasapi_backend.h"
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

static std::vector<DeviceInfo> g_renderDevices;
static std::vector<DeviceInfo> g_captureDevices;
static std::vector<long> g_validPeriods; // Hardware-valid WASAPI periods, queried on open
static bool g_settingsChanged = false;

// Captured definitively in DllMain - this is the DLL's own HINSTANCE,
// which is what DialogBoxParam MUST receive to find our embedded resources.
static HINSTANCE g_hDllInstance = NULL;

void InitControlPanelInstance(HINSTANCE hDll) {
    g_hDllInstance = hDll;
}

bool DidSettingsChange() { return g_settingsChanged; }
void ClearSettingsChangedFlag() { g_settingsChanged = false; }

static void EnumerateDevices(EDataFlow flow, std::vector<DeviceInfo>& outDevices) {
    outDevices.clear();
    
    // Default fallback item
    DeviceInfo defDev;
    defDev.id = L"Default";
    defDev.name = L"Windows Default Device";
    outDevices.push_back(defDev);

    ComPtr<IMMDeviceEnumerator> pEnumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator)))) {
        return;
    }

    ComPtr<IMMDeviceCollection> pCollection;
    if (FAILED(pEnumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCollection))) {
        return;
    }

    UINT count;
    pCollection->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> pEndpoint;
        if (SUCCEEDED(pCollection->Item(i, &pEndpoint))) {
            LPWSTR pwszID = NULL;
            if (SUCCEEDED(pEndpoint->GetId(&pwszID))) {
                DeviceInfo info;
                info.id = pwszID;
                CoTaskMemFree(pwszID);
                
                ComPtr<IPropertyStore> pProps;
                if (SUCCEEDED(pEndpoint->OpenPropertyStore(STGM_READ, &pProps))) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                        info.name = varName.pwszVal;
                        PropVariantClear(&varName);
                    }
                }
                outDevices.push_back(info);
            }
        }
    }
}

static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG: {
            // Load current settings
            Settings settings;
            settings.Load();

            // Populate Buffer Sizes dynamically from hardware-valid WASAPI periods.
            // This replaces the hardcoded list and guarantees aligned (zero-overhead) mode
            // for every size shown — no ring-buffer starvation possible.
            HWND hBufferCombo = GetDlgItem(hwnd, IDC_BUFFER_SIZE);
            int selectedBufferIdx = 0;
            if (!g_validPeriods.empty()) {
                long savedSize = settings.GetBufferSize();
                // Find the closest valid period to the saved preference
                long bestDiff = LONG_MAX;
                for (int i = 0; i < (int)g_validPeriods.size(); i++) {
                    long diff = abs(g_validPeriods[i] - savedSize);
                    if (diff < bestDiff) { bestDiff = diff; selectedBufferIdx = i; }
                }
                for (int i = 0; i < (int)g_validPeriods.size(); i++) {
                    wchar_t bufText[64];
                    // Mark the hardware-native default period (480 / fundamental) with a star
                    bool isDefault = (g_validPeriods[i] == 480);
                    wsprintfW(bufText, L"%d Samples%s", g_validPeriods[i], isDefault ? L" \u2605" : L"");
                    SendMessageW(hBufferCombo, CB_ADDSTRING, 0, (LPARAM)bufText);
                }
            } else {
                // Fallback if hardware query failed: offer 30 to 1920 range
                int fallbackSizes[] = { 30, 60, 120, 240, 480, 960, 1440, 1920 };
                long savedSize = settings.GetBufferSize();
                for (int i = 0; i < 8; i++) {
                    wchar_t bufText[64];
                    bool isDefault = (fallbackSizes[i] == 480);
                    wsprintfW(bufText, L"%d Samples%s", fallbackSizes[i], isDefault ? L" \u2605" : L"");
                    SendMessageW(hBufferCombo, CB_ADDSTRING, 0, (LPARAM)bufText);
                    if (savedSize == fallbackSizes[i]) selectedBufferIdx = i;
                }
            }
            SendMessageW(hBufferCombo, CB_SETCURSEL, selectedBufferIdx, 0);

            // Populate Render Devices
            EnumerateDevices(eRender, g_renderDevices);
            HWND hRenderCombo = GetDlgItem(hwnd, IDC_RENDER_DEVICE);
            int selectedRenderIdx = 0;
            for (size_t i = 0; i < g_renderDevices.size(); i++) {
                SendMessageW(hRenderCombo, CB_ADDSTRING, 0, (LPARAM)g_renderDevices[i].name.c_str());
                if (settings.GetRenderEndpointId() == g_renderDevices[i].id) {
                    selectedRenderIdx = (int)i;
                }
            }
            SendMessageW(hRenderCombo, CB_SETCURSEL, selectedRenderIdx, 0);

            // Populate Capture Devices
            EnumerateDevices(eCapture, g_captureDevices);
            HWND hCaptureCombo = GetDlgItem(hwnd, IDC_CAPTURE_DEVICE);
            int selectedCaptureIdx = 0;
            for (size_t i = 0; i < g_captureDevices.size(); i++) {
                SendMessageW(hCaptureCombo, CB_ADDSTRING, 0, (LPARAM)g_captureDevices[i].name.c_str());
                if (settings.GetCaptureEndpointId() == g_captureDevices[i].id) {
                    selectedCaptureIdx = (int)i;
                }
            }
            SendMessageW(hCaptureCombo, CB_SETCURSEL, selectedCaptureIdx, 0);

            return TRUE;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_APPLY) {
                Settings settings;
                
                // Get buffer size from selected index
                HWND hBufferCombo = GetDlgItem(hwnd, IDC_BUFFER_SIZE);
                int selBuf = SendMessageW(hBufferCombo, CB_GETCURSEL, 0, 0);
                if (selBuf >= 0) {
                    long chosen = 0;
                    if (!g_validPeriods.empty() && selBuf < (int)g_validPeriods.size()) {
                        chosen = g_validPeriods[selBuf];
                    } else {
                        int fallbackSizes[] = { 30, 60, 120, 240, 480, 960, 1440, 1920 };
                        if (selBuf < 8) chosen = fallbackSizes[selBuf];
                    }
                    if (chosen > 0) settings.SetBufferSize(chosen);
                }

                // Get render
                HWND hRenderCombo = GetDlgItem(hwnd, IDC_RENDER_DEVICE);
                int selRen = SendMessageW(hRenderCombo, CB_GETCURSEL, 0, 0);
                if (selRen >= 0 && selRen < (int)g_renderDevices.size()) {
                    settings.SetRenderEndpointId(g_renderDevices[selRen].id);
                }

                // Get capture
                HWND hCaptureCombo = GetDlgItem(hwnd, IDC_CAPTURE_DEVICE);
                int selCap = SendMessageW(hCaptureCombo, CB_GETCURSEL, 0, 0);
                if (selCap >= 0 && selCap < (int)g_captureDevices.size()) {
                    settings.SetCaptureEndpointId(g_captureDevices[selCap].id);
                }

                settings.Save();
                g_settingsChanged = true;
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            else if (LOWORD(wParam) == IDC_CANCEL || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

void ShowControlPanel(HWND parentWindow) {
    // Ensure COM is ready for device enumeration
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    // Fix #2: Initialize Common Controls v6 before creating dialog.
    // Without this, ComboBoxes and other controls may silently fail.
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);
    
    g_settingsChanged = false;

    // Query hardware-valid WASAPI periods so the dropdown shows only aligned-mode sizes.
    // We create a temporary backend, init to default device, query, then discard.
    g_validPeriods.clear();
    {
        WasapiBackend tempBackend;
        if (tempBackend.Init(48000, L"Default", L"Default")) {
            g_validPeriods = tempBackend.GetValidPeriods();
            tempBackend.Shutdown();
        }
    }

    // Fix #1: MUST use the DLL's own HINSTANCE (not the host's) so that
    // Windows finds our dialog resource embedded in lux_asio.dll.
    // Fix #3: Check return value and log any Win32 error code.
    INT_PTR result = DialogBoxParamW(
        g_hDllInstance,
        MAKEINTRESOURCEW(IDD_CONTROL_PANEL),
        parentWindow,
        DialogProc,
        0
    );

    if (result == -1) {
        DWORD err = GetLastError();
        char dbgMsg[256];
        wsprintfA(dbgMsg, "[LuxASIO] DialogBoxParam failed! GetLastError()=%lu (0x%lX)\n", err, err);
        OutputDebugStringA(dbgMsg);
    }
}
