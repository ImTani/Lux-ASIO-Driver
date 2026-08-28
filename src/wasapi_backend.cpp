#include "wasapi_backend.h"
#include <stdio.h>

WasapiBackend::WasapiBackend()
    : m_initialized(false)
    , m_sampleRate(44100)
    , m_negotiatedPeriodInFrames(0)
    , m_renderFormat(nullptr)
    , m_captureFormat(nullptr)
{
}

WasapiBackend::~WasapiBackend()
{
    Shutdown();
}

bool WasapiBackend::Init(long sampleRate, const std::wstring& renderId, const std::wstring& captureId)
{
    if (m_initialized) return true;
    m_sampleRate = sampleRate;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&m_deviceEnumerator));
    if (FAILED(hr)) return false;

    // Get render device
    if (renderId.empty() || renderId == L"Default") {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_renderDevice);
    } else {
        hr = m_deviceEnumerator->GetDevice(renderId.c_str(), &m_renderDevice);
        if (FAILED(hr)) m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_renderDevice);
    }

    if (m_renderDevice) {
        hr = m_renderDevice->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&m_renderAudioClient);
        if (SUCCEEDED(hr)) {
            m_renderAudioClient->GetMixFormat(&m_renderFormat);
        }
    }
    if (!m_renderAudioClient) return false;

    // Get capture device
    if (captureId.empty() || captureId == L"Default") {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &m_captureDevice);
    } else {
        hr = m_deviceEnumerator->GetDevice(captureId.c_str(), &m_captureDevice);
    }

    if (m_captureDevice) {
        hr = m_captureDevice->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&m_captureAudioClient);
        if (SUCCEEDED(hr)) {
            m_captureAudioClient->GetMixFormat(&m_captureFormat);
        }
    }

    m_initialized = true;
    return true;
}

void WasapiBackend::Shutdown()
{
    if (m_renderFormat) {
        CoTaskMemFree(m_renderFormat);
        m_renderFormat = nullptr;
    }
    if (m_captureFormat) {
        CoTaskMemFree(m_captureFormat);
        m_captureFormat = nullptr;
    }

    m_renderClient.Reset();
    m_captureClient.Reset();
    m_renderAudioClient.Reset();
    m_captureAudioClient.Reset();
    m_renderDevice.Reset();
    m_captureDevice.Reset();
    m_deviceEnumerator.Reset();

    m_initialized = false;
    m_negotiatedPeriodInFrames = 0;
}

bool WasapiBackend::GetBufferSizes(WasapiBufferSizes& outSizes)
{
    if (!m_renderAudioClient) return false;

    UINT32 defaultPeriod, fundamentalPeriod, minPeriod, maxPeriod;
    HRESULT hr = m_renderAudioClient->GetSharedModeEnginePeriod(
        m_renderFormat, &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod);

    if (FAILED(hr)) {
        // Fallback for pre-Win10-1703 or drivers that don't support IAudioClient3 period query
        outSizes.defaultPeriodInFrames   = 480;
        outSizes.fundamentalPeriodInFrames = 480;
        outSizes.minPeriodInFrames       = 480;
        outSizes.maxPeriodInFrames       = 480;
        return true;
    }

    outSizes.defaultPeriodInFrames     = static_cast<long>(defaultPeriod);
    outSizes.fundamentalPeriodInFrames = static_cast<long>(fundamentalPeriod);
    outSizes.minPeriodInFrames         = static_cast<long>(minPeriod);
    outSizes.maxPeriodInFrames         = static_cast<long>(maxPeriod);
    return true;
}

bool WasapiBackend::TryNegotiatePeriod(long requestedFrames, long& outActualFrames)
{
    WasapiBufferSizes sizes;
    if (!GetBufferSizes(sizes)) {
        outActualFrames = requestedFrames;
        return false;
    }

    long fundamental = sizes.fundamentalPeriodInFrames;
    long minP = sizes.minPeriodInFrames;
    long maxP = sizes.maxPeriodInFrames;

    if (fundamental <= 0) {
        // Driver doesn't expose fundamental — just clamp and use as-is
        outActualFrames = (requestedFrames < minP) ? minP :
                          (requestedFrames > maxP) ? maxP : requestedFrames;
        return true;
    }

    // Snap to nearest valid multiple of fundamental within [min, max]
    // Valid period = N * fundamental, N >= ceil(minP / fundamental)
    long nMin = (minP + fundamental - 1) / fundamental; // ceil
    long nMax = maxP / fundamental;                      // floor

    // Find the N whose N*fundamental is closest to requestedFrames
    long nRequested = (requestedFrames + fundamental / 2) / fundamental; // round
    long n = (nRequested < nMin) ? nMin :
             (nRequested > nMax) ? nMax : nRequested;

    outActualFrames = n * fundamental;

    char dbgMsg[256];
    sprintf_s(dbgMsg, "[LuxASIO] TryNegotiatePeriod: requested=%ld, fundamental=%ld, snap->%ld (N=%ld)\n",
              requestedFrames, fundamental, outActualFrames, n);
    OutputDebugStringA(dbgMsg);

    return true;
}

std::vector<long> WasapiBackend::GetValidPeriods()
{
    std::vector<long> periods;

    WasapiBufferSizes sizes;
    if (!GetBufferSizes(sizes)) return periods;

    long fundamental = sizes.fundamentalPeriodInFrames;
    long minP = sizes.minPeriodInFrames;
    long maxP = sizes.maxPeriodInFrames;

    long step = (fundamental > 0) ? fundamental : ((minP > 0) ? minP : sizes.defaultPeriodInFrames);
    if (step <= 0) step = 256; // Absolute fallback
    
    long start = (minP > 0) ? minP : step;

    // We want to offer large buffer sizes (for heavy projects) even if the hardware's
    // max period is restricted (e.g. minP == maxP == 480). The AudioThread handles 
    // these larger sizes via the decoupled ring-buffer mode.
    long cap = (maxP > 4096) ? maxP : 4096;
    
    for (long p = start; p <= cap; p += step) {
        periods.push_back(p);
    }

    return periods;
}

bool WasapiBackend::InitStreams(long asioBufferSizeInFrames, HANDLE eventHandle, bool& outAlignedMode)
{
    if (!m_renderAudioClient) return false;

    // Negotiate the WASAPI period to the user's ASIO buffer size.
    // If exact match is achievable we get aligned mode; otherwise snap to nearest valid multiple.
    long wasapiPeriod = 0;
    TryNegotiatePeriod(asioBufferSizeInFrames, wasapiPeriod);
    if (wasapiPeriod <= 0) wasapiPeriod = asioBufferSizeInFrames;

    m_negotiatedPeriodInFrames = wasapiPeriod;
    outAlignedMode = (wasapiPeriod == asioBufferSizeInFrames);

    char dbgMsg[256];
    sprintf_s(dbgMsg, "[LuxASIO] InitStreams: ASIO=%ld, WASAPI=%ld, mode=%s\n",
              asioBufferSizeInFrames, wasapiPeriod,
              outAlignedMode ? "ALIGNED (zero-overhead)" : "FALLBACK (ring buffer)");
    OutputDebugStringA(dbgMsg);

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    // --- Render ---
    HRESULT hr = m_renderAudioClient->InitializeSharedAudioStream(
        flags,
        static_cast<UINT32>(wasapiPeriod),
        m_renderFormat,
        NULL
    );

    if (FAILED(hr)) {
        // IAudioClient3::InitializeSharedAudioStream may fail on some virtualized/legacy drivers.
        // Fall back to the classic IAudioClient::Initialize with a 100ns-unit period.
        // Convert frames → 100ns: (frames / sampleRate) * 10,000,000
        REFERENCE_TIME period100ns = (REFERENCE_TIME)wasapiPeriod * 10000000LL / m_sampleRate;
        hr = m_renderAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            period100ns,
            0,
            m_renderFormat,
            NULL
        );
        // If we fell back, we're no longer aligned in the IAudioClient3 sense
        outAlignedMode = false;
        OutputDebugStringA("[LuxASIO] InitStreams: InitializeSharedAudioStream failed, using classic Initialize fallback\n");
        if (FAILED(hr)) return false;
    }

    hr = m_renderAudioClient->SetEventHandle(eventHandle);
    if (FAILED(hr)) return false;

    hr = m_renderAudioClient->GetService(IID_PPV_ARGS(&m_renderClient));
    if (FAILED(hr)) return false;

    // --- Capture (optional) ---
    if (m_captureAudioClient && m_captureFormat) {
        // Try to match capture period to render period for symmetric timing
        HRESULT hrCap = m_captureAudioClient->InitializeSharedAudioStream(
            flags,
            static_cast<UINT32>(wasapiPeriod),
            m_captureFormat,
            NULL
        );

        if (FAILED(hrCap)) {
            REFERENCE_TIME period100ns = (REFERENCE_TIME)wasapiPeriod * 10000000LL / m_sampleRate;
            hrCap = m_captureAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                period100ns,
                0,
                m_captureFormat,
                NULL
            );
        }

        if (SUCCEEDED(hrCap)) {
            // In ASIO we drive from render event; set same handle so capture is also pumped.
            m_captureAudioClient->SetEventHandle(eventHandle);
            m_captureAudioClient->GetService(IID_PPV_ARGS(&m_captureClient));
        }
    }

    return true;
}

bool WasapiBackend::Start()
{
    bool success = false;
    if (m_renderAudioClient) {
        if (SUCCEEDED(m_renderAudioClient->Start())) {
            success = true;
        }
    }
    if (m_captureAudioClient) {
        m_captureAudioClient->Start();
    }
    return success;
}

bool WasapiBackend::Stop()
{
    if (m_renderAudioClient) m_renderAudioClient->Stop();
    if (m_captureAudioClient) m_captureAudioClient->Stop();
    return true;
}

UINT32 WasapiBackend::GetRenderBufferPadding()
{
    UINT32 padding = 0;
    if (m_renderAudioClient) {
        m_renderAudioClient->GetCurrentPadding(&padding);
    }
    return padding;
}

UINT32 WasapiBackend::GetCaptureNextPacketSize()
{
    UINT32 packetSize = 0;
    if (m_captureClient) {
        m_captureClient->GetNextPacketSize(&packetSize);
    }
    return packetSize;
}
