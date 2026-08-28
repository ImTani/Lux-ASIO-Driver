// Enumerates every active render/capture endpoint and prints the
// IAudioClient3 shared-mode engine periods each one supports. This is the
// ground truth for how low the Lux ASIO driver (or any shared-mode client)
// can go on a given machine: min period == the aligned-mode latency floor.

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static void ProbeFlow(IMMDeviceEnumerator* enumerator, EDataFlow flow, const char* label) {
    printf("\n=== %s devices ===\n", label);

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) return;

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        wchar_t name[256] = L"(unknown)";
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
                wcsncpy_s(name, var.pwszVal, _TRUNCATE);
            }
            PropVariantClear(&var);
        }

        ComPtr<IAudioClient3> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&client))) {
            wprintf(L"  %s\n    IAudioClient3 activation FAILED\n", name);
            continue;
        }

        WAVEFORMATEX* fmt = nullptr;
        client->GetMixFormat(&fmt);
        if (!fmt) continue;

        UINT32 defP = 0, funP = 0, minP = 0, maxP = 0;
        HRESULT hr = client->GetSharedModeEnginePeriod(fmt, &defP, &funP, &minP, &maxP);

        wprintf(L"  %s\n", name);
        printf("    mix format: %lu Hz, %u ch\n", fmt->nSamplesPerSec, fmt->nChannels);
        if (SUCCEEDED(hr)) {
            double toMs = 1000.0 / fmt->nSamplesPerSec;
            printf("    periods (frames): min=%u  default=%u  fundamental=%u  max=%u\n",
                   minP, defP, funP, maxP);
            printf("    latency floor:    min=%.2f ms  default=%.2f ms\n",
                   minP * toMs, defP * toMs);
            if (minP < defP)
                printf("    >>> low-latency capable: driver can negotiate below the default period\n");
            else
                printf("    >>> NOT low-latency capable: min == default, %u frames is the floor\n", minP);
        } else {
            printf("    GetSharedModeEnginePeriod FAILED (hr=0x%08lX) — pre-1703 driver path\n", (unsigned long)hr);
        }

        // Exclusive-mode floor: GetDevicePeriod's minimum is the smallest
        // period the driver accepts when bypassing the shared engine.
        REFERENCE_TIME defPeriod100ns = 0, minPeriod100ns = 0;
        if (SUCCEEDED(client->GetDevicePeriod(&defPeriod100ns, &minPeriod100ns))) {
            double minMs = minPeriod100ns / 10000.0;
            long minFrames = (long)((double)minPeriod100ns * fmt->nSamplesPerSec / 10000000.0 + 0.5);
            printf("    EXCLUSIVE floor:  min=%.2f ms (%ld frames)  default=%.2f ms\n",
                   minMs, minFrames, defPeriod100ns / 10000.0);
        }
        CoTaskMemFree(fmt);
    }
}

int main() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        printf("Failed to create device enumerator\n");
        return 1;
    }

    ProbeFlow(enumerator.Get(), eRender, "Render (output)");
    ProbeFlow(enumerator.Get(), eCapture, "Capture (input)");

    CoUninitialize();
    return 0;
}
