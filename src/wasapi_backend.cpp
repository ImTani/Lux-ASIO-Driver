#include "wasapi_backend.h"
#include <stdio.h>

WasapiBackend::WasapiBackend()
    : m_initialized(false)
    , m_sampleRate(44100)
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
}

bool WasapiBackend::GetBufferSizes(WasapiBufferSizes& outSizes)
{
    if (!m_renderAudioClient) return false;

    UINT32 defaultPeriod, fundamentalPeriod, minPeriod, maxPeriod;
    
    // We request periods based on the mix format. The driver will try to match this format.
    HRESULT hr = m_renderAudioClient->GetSharedModeEnginePeriod(m_renderFormat, &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod);
    
    if (FAILED(hr)) {
        // Fallback if IAudioClient3 period query fails for some reason
        outSizes.defaultPeriodInFrames = 256;
        outSizes.fundamentalPeriodInFrames = 0;
        outSizes.minPeriodInFrames = 256;
        outSizes.maxPeriodInFrames = 256;
        return true;
    }

    outSizes.defaultPeriodInFrames = defaultPeriod;
    outSizes.fundamentalPeriodInFrames = fundamentalPeriod;
    outSizes.minPeriodInFrames = minPeriod;
    outSizes.maxPeriodInFrames = maxPeriod;
    return true;
}

bool WasapiBackend::InitStreams(long bufferSizeInFrames, HANDLE eventHandle)
{
    if (!m_renderAudioClient) return false;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    // Initialize Render
    HRESULT hr = m_renderAudioClient->InitializeSharedAudioStream(
        flags,
        bufferSizeInFrames,
        m_renderFormat,
        NULL
    );
    if (FAILED(hr)) return false;

    hr = m_renderAudioClient->SetEventHandle(eventHandle);
    if (FAILED(hr)) return false;

    hr = m_renderAudioClient->GetService(IID_PPV_ARGS(&m_renderClient));
    if (FAILED(hr)) return false;

    // Initialize Capture (if available)
    if (m_captureAudioClient && m_captureFormat) {
        // Try to initialize capture with the same period
        hr = m_captureAudioClient->InitializeSharedAudioStream(
            flags,
            bufferSizeInFrames,
            m_captureFormat,
            NULL
        );
        
        if (SUCCEEDED(hr)) {
            // Note: Capture stream needs its own event handle if we wait on it,
            // but in ASIO we typically wait on the render handle and just pull available capture packets.
            // For IAudioClient3, it's often better to not set the event handle on capture and just poll it
            // when render event fires, or set the SAME event handle. We'll set the same event handle.
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
