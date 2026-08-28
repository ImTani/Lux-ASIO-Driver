#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

struct WasapiBufferSizes {
    long defaultPeriodInFrames;
    long fundamentalPeriodInFrames;
    long minPeriodInFrames;
    long maxPeriodInFrames;
};

class WasapiBackend {
public:
    WasapiBackend();
    ~WasapiBackend();

    bool Init(long sampleRate, const std::wstring& renderId, const std::wstring& captureId);
    void Shutdown();

    bool GetBufferSizes(WasapiBufferSizes& outSizes);
    
    // Initializes the audio streams with the chosen buffer size
    bool InitStreams(long bufferSizeInFrames, HANDLE eventHandle);
    
    bool Start();
    bool Stop();

    ComPtr<IAudioClient3> GetRenderAudioClient() const { return m_renderAudioClient; }
    ComPtr<IAudioClient3> GetCaptureAudioClient() const { return m_captureAudioClient; }
    ComPtr<IAudioRenderClient> GetRenderClient() const { return m_renderClient; }
    ComPtr<IAudioCaptureClient> GetCaptureClient() const { return m_captureClient; }

    WAVEFORMATEX* GetRenderFormat() const { return m_renderFormat; }
    WAVEFORMATEX* GetCaptureFormat() const { return m_captureFormat; }

    // Helpers to write/read
    UINT32 GetRenderBufferPadding();
    UINT32 GetCaptureNextPacketSize();

private:
    bool m_initialized;
    long m_sampleRate;
    
    ComPtr<IMMDeviceEnumerator> m_deviceEnumerator;
    
    ComPtr<IMMDevice> m_renderDevice;
    ComPtr<IAudioClient3> m_renderAudioClient;
    ComPtr<IAudioRenderClient> m_renderClient;
    WAVEFORMATEX* m_renderFormat;

    ComPtr<IMMDevice> m_captureDevice;
    ComPtr<IAudioClient3> m_captureAudioClient;
    ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX* m_captureFormat;
};
