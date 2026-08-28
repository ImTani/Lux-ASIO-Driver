#pragma once
#include <windows.h>
#include <atomic>
#include "wasapi_backend.h"
#include "asiodrvr.h" // For ASIOCallbacks and ASIOBufferInfo
#include "ring_buffer.h"
#include <vector>

class AudioThread {
public:
    AudioThread(WasapiBackend* backend);
    ~AudioThread();

    bool Start(long bufferSize, ASIOCallbacks* callbacks, ASIOBufferInfo* bufferInfos, long numInputChannels, long numOutputChannels);
    void Stop();

    // Diagnostics: number of output underruns since last Start()
    long GetUnderrunCount() const { return m_underrunCount.load(std::memory_order_relaxed); }

private:
    static DWORD WINAPI ThreadProc(LPVOID lpParam);
    void Run();

    static DWORD WINAPI AsioThreadProc(LPVOID lpParam);
    void RunAsioThread();

    // Zero-overhead aligned mode: WASAPI period == ASIO buffer, no ring buffer needed
    void RunAligned(
        ComPtr<IAudioRenderClient>& renderClient,
        ComPtr<IAudioCaptureClient>& captureClient,
        WAVEFORMATEX* renderFormat,
        WAVEFORMATEX* captureFormat);

    // Fallback ring-buffer mode: WASAPI period != ASIO buffer
    void RunDecoupled(
        ComPtr<IAudioRenderClient>& renderClient,
        ComPtr<IAudioCaptureClient>& captureClient,
        WAVEFORMATEX* renderFormat,
        WAVEFORMATEX* captureFormat);

    WasapiBackend* m_backend;
    HANDLE m_threadHandle;
    HANDLE m_eventHandle;
    HANDLE m_asioThreadHandle;
    HANDLE m_asioEventHandle;
    std::atomic<bool> m_stopRequested;

    long m_bufferSize;       // ASIO block size (user selected)
    long m_wasapiPeriod;     // Negotiated WASAPI hardware period
    bool m_alignedMode;      // true = direct pass-through, false = ring buffer

    ASIOCallbacks* m_callbacks;
    ASIOBufferInfo* m_bufferInfos;
    long m_numInputChannels;
    long m_numOutputChannels;

    // Decoupling ring buffers (only used in fallback mode)
    std::vector<RingBuffer*> m_inputRings;
    std::vector<RingBuffer*> m_outputRings;

    // Toggle ASIO double buffer index (0 or 1)
    long m_asioBufferIndex;

    // Diagnostics
    std::atomic<long> m_underrunCount;
};
