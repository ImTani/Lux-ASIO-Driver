#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <string>
#include <vector>

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

    // Queries whether 'requestedFrames' is a valid IAudioClient3 shared period.
    // Returns true and sets outActualFrames = requestedFrames if exact match is valid.
    // Returns true and sets outActualFrames = nearest valid multiple if snapped.
    // Returns false only if IAudioClient3 period query completely fails.
    bool TryNegotiatePeriod(long requestedFrames, long& outActualFrames);

    // Enumerate all valid buffer sizes (multiples of fundamental within [min, max])
    // Used to populate the Control Panel dropdown with only hardware-valid choices.
    std::vector<long> GetValidPeriods();

    // Initializes the audio streams using InitializeSharedAudioStream at the negotiated period.
    // Sets m_alignedMode = true iff wasapiPeriod == asioBufferSize (zero-overhead path).
    bool InitStreams(long asioBufferSizeInFrames, HANDLE eventHandle, bool& outAlignedMode);

    bool Start();
    bool Stop();

    ComPtr<IAudioClient3> GetRenderAudioClient() const { return m_renderAudioClient; }
    ComPtr<IAudioClient3> GetCaptureAudioClient() const { return m_captureAudioClient; }
    ComPtr<IAudioRenderClient> GetRenderClient() const { return m_renderClient; }
    ComPtr<IAudioCaptureClient> GetCaptureClient() const { return m_captureClient; }

    WAVEFORMATEX* GetRenderFormat() const { return m_renderFormat; }
    WAVEFORMATEX* GetCaptureFormat() const { return m_captureFormat; }

    UINT32 GetRenderBufferPadding();
    UINT32 GetCaptureNextPacketSize();

    // The actual WASAPI period we negotiated (may differ from the ASIO buffer size in fallback mode)
    long GetNegotiatedPeriod() const { return m_negotiatedPeriodInFrames; }

private:
    bool m_initialized;
    long m_sampleRate;
    long m_negotiatedPeriodInFrames; // Actual WASAPI period after negotiation

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
