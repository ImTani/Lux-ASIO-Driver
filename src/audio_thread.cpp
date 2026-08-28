#include "audio_thread.h"
#define NOMINMAX
#include <avrt.h>
#include <stdio.h>
#include <algorithm>

AudioThread::AudioThread(WasapiBackend* backend)
    : m_backend(backend)
    , m_threadHandle(NULL)
    , m_eventHandle(NULL)
    , m_stopRequested(false)
    , m_bufferSize(0)
    , m_wasapiPeriod(0)
    , m_alignedMode(false)
    , m_callbacks(nullptr)
    , m_bufferInfos(nullptr)
    , m_numInputChannels(0)
    , m_numOutputChannels(0)
    , m_asioBufferIndex(0)
    , m_underrunCount(0)
{
}

AudioThread::~AudioThread()
{
    Stop();
}

bool AudioThread::Start(long bufferSize, ASIOCallbacks* callbacks, ASIOBufferInfo* bufferInfos, long numInputChannels, long numOutputChannels)
{
    m_bufferSize         = bufferSize;
    m_callbacks          = callbacks;
    m_bufferInfos        = bufferInfos;
    m_numInputChannels   = numInputChannels;
    m_numOutputChannels  = numOutputChannels;
    m_asioBufferIndex    = 0;
    m_stopRequested      = false;
    m_underrunCount      = 0;

    m_eventHandle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!m_eventHandle) return false;

    // Negotiate the WASAPI period. InitStreams will set m_alignedMode.
    bool aligned = false;
    if (!m_backend->InitStreams(m_bufferSize, m_eventHandle, aligned)) {
        CloseHandle(m_eventHandle);
        m_eventHandle = NULL;
        return false;
    }

    m_alignedMode    = aligned;
    m_wasapiPeriod   = m_backend->GetNegotiatedPeriod();

    // Allocate ring buffers for fallback mode only.
    // In aligned mode we skip them entirely — zero allocation overhead.
    if (!m_alignedMode) {
        // Size the ring to hold at least 4x the larger of the two periods to absorb jitter.
        size_t ringSize = (size_t)((std::max)(m_wasapiPeriod, m_bufferSize)) * 8;
        if (ringSize < 16384) ringSize = 16384;

        for (int i = 0; i < m_numInputChannels; ++i)
            m_inputRings.push_back(new RingBuffer(ringSize));

        for (int i = 0; i < m_numOutputChannels; ++i) {
            auto rb = new RingBuffer(ringSize);
            // Pre-fill output ring with 2 full WASAPI periods of silence.
            // This gives the DAW time to fill it before WASAPI first drains it.
            rb->PushSilence((size_t)m_wasapiPeriod * 2);
            m_outputRings.push_back(rb);
        }
    }

    m_threadHandle = CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
    if (!m_threadHandle) {
        CloseHandle(m_eventHandle);
        m_eventHandle = NULL;
        return false;
    }

    SetThreadPriority(m_threadHandle, THREAD_PRIORITY_TIME_CRITICAL);

    if (!m_backend->Start()) {
        Stop();
        return false;
    }

    char dbgMsg[256];
    sprintf_s(dbgMsg,
        "[LuxASIO] AudioThread started. ASIO=%ld, WASAPI=%ld, mode=%s\n",
        m_bufferSize, m_wasapiPeriod,
        m_alignedMode ? "ALIGNED" : "FALLBACK-RING");
    OutputDebugStringA(dbgMsg);

    return true;
}

void AudioThread::Stop()
{
    m_stopRequested = true;
    if (m_eventHandle)
        SetEvent(m_eventHandle);

    if (m_threadHandle) {
        WaitForSingleObject(m_threadHandle, INFINITE);
        CloseHandle(m_threadHandle);
        m_threadHandle = NULL;
    }

    if (m_backend)
        m_backend->Stop();

    if (m_eventHandle) {
        CloseHandle(m_eventHandle);
        m_eventHandle = NULL;
    }

    for (auto rb : m_inputRings)  delete rb;
    for (auto rb : m_outputRings) delete rb;
    m_inputRings.clear();
    m_outputRings.clear();
}

DWORD WINAPI AudioThread::ThreadProc(LPVOID lpParam)
{
    AudioThread* self = static_cast<AudioThread*>(lpParam);
    self->Run();
    return 0;
}

void AudioThread::Run()
{
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    auto renderClient  = m_backend->GetRenderClient();
    auto captureClient = m_backend->GetCaptureClient();
    auto renderFormat  = m_backend->GetRenderFormat();
    auto captureFormat = m_backend->GetCaptureFormat();

    if (m_alignedMode)
        RunAligned(renderClient, captureClient, renderFormat, captureFormat);
    else
        RunDecoupled(renderClient, captureClient, renderFormat, captureFormat);

    if (mmcssHandle)
        AvRevertMmThreadCharacteristics(mmcssHandle);
}

// =============================================================================
// ALIGNED MODE — zero-overhead direct passthrough
// WASAPI period == ASIO buffer size. Every WASAPI event is exactly one ASIO block.
// No ring buffers, no starvation, no drift. Identical stability to Steinberg's driver.
// =============================================================================
void AudioThread::RunAligned(
    ComPtr<IAudioRenderClient>& renderClient,
    ComPtr<IAudioCaptureClient>& captureClient,
    WAVEFORMATEX* renderFormat,
    WAVEFORMATEX* captureFormat)
{
    const bool hasCapture  = (captureClient && captureFormat && m_numInputChannels > 0);
    const bool hasRender   = (renderClient  && renderFormat  && m_numOutputChannels > 0);

    // Scratch buffers for interleave/deinterleave — allocated once, outside the loop
    std::vector<float> captureDeinterleaved(m_bufferSize);
    std::vector<float> renderInterleaved(m_bufferSize * (renderFormat ? renderFormat->nChannels : 2));

    while (!m_stopRequested) {
        DWORD waitResult = WaitForSingleObject(m_eventHandle, 2000);
        if (m_stopRequested) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        // --- 1. CAPTURE: WASAPI → ASIO input buffers (de-interleave) ---
        if (hasCapture) {
            UINT32 packetLength = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&packetLength);
            while (SUCCEEDED(hr) && packetLength > 0) {
                BYTE* pData = nullptr; DWORD flags = 0;
                UINT32 framesAvail = 0;
                hr = captureClient->GetBuffer(&pData, &framesAvail, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    if (!isSilent) {
                        float* src = reinterpret_cast<float*>(pData);
                        int nCh = captureFormat->nChannels;
                        UINT32 count = (std::min)(framesAvail, (UINT32)m_bufferSize);
                        for (int c = 0; c < m_numInputChannels && c < nCh; ++c) {
                            float* dest = reinterpret_cast<float*>(m_bufferInfos[c].buffers[m_asioBufferIndex]);
                            for (UINT32 f = 0; f < count; ++f)
                                dest[f] = src[f * nCh + c];
                        }
                    } else {
                        for (int c = 0; c < m_numInputChannels; ++c)
                            memset(m_bufferInfos[c].buffers[m_asioBufferIndex], 0, m_bufferSize * sizeof(float));
                    }
                    captureClient->ReleaseBuffer(framesAvail);
                }
                hr = captureClient->GetNextPacketSize(&packetLength);
            }
        } else if (m_numInputChannels > 0) {
            // No capture device — zero the input buffers
            for (int c = 0; c < m_numInputChannels; ++c)
                memset(m_bufferInfos[c].buffers[m_asioBufferIndex], 0, m_bufferSize * sizeof(float));
        }

        // --- 2. DAW PROCESSING BLOCK ---
        if (m_callbacks && m_callbacks->bufferSwitch)
            m_callbacks->bufferSwitch(m_asioBufferIndex, ASIOFalse);

        // --- 3. RENDER: ASIO output buffers → WASAPI (interleave) ---
        if (hasRender) {
            UINT32 padding = 0;
            m_backend->GetRenderAudioClient()->GetCurrentPadding(&padding);

            UINT32 maxWASAPIFrames = 0;
            m_backend->GetRenderAudioClient()->GetBufferSize(&maxWASAPIFrames);

            UINT32 framesToWrite = (maxWASAPIFrames > padding) ? (maxWASAPIFrames - padding) : 0;
            framesToWrite = (std::min)(framesToWrite, (UINT32)m_bufferSize);

            if (framesToWrite > 0) {
                BYTE* pData = nullptr;
                HRESULT hr = renderClient->GetBuffer(framesToWrite, &pData);
                if (SUCCEEDED(hr)) {
                    float* dest = reinterpret_cast<float*>(pData);
                    int nCh = renderFormat->nChannels;
                    for (int c = 0; c < m_numOutputChannels && c < nCh; ++c) {
                        float* src = reinterpret_cast<float*>(
                            m_bufferInfos[m_numInputChannels + c].buffers[m_asioBufferIndex]);
                        for (UINT32 f = 0; f < framesToWrite; ++f)
                            dest[f * nCh + c] = src[f];
                    }
                    renderClient->ReleaseBuffer(framesToWrite, 0);
                }
            }
        }

        m_asioBufferIndex = m_asioBufferIndex == 0 ? 1 : 0;
    }
}

// =============================================================================
// FALLBACK RING-BUFFER MODE — used when WASAPI period != ASIO buffer size
// Still uses ring buffers but with a 2× WASAPI-period pre-fill and underrun logging.
// =============================================================================
void AudioThread::RunDecoupled(
    ComPtr<IAudioRenderClient>& renderClient,
    ComPtr<IAudioCaptureClient>& captureClient,
    WAVEFORMATEX* renderFormat,
    WAVEFORMATEX* captureFormat)
{
    std::vector<float> captureDeinterleaved(16384);
    std::vector<float> renderInterleaved(16384);

    while (!m_stopRequested) {
        DWORD waitResult = WaitForSingleObject(m_eventHandle, 2000);
        if (m_stopRequested) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        // How many frames can we write to WASAPI right now?
        UINT32 padding = m_backend->GetRenderBufferPadding();
        UINT32 maxWASAPIFrames = 0;
        if (m_backend->GetRenderAudioClient())
            m_backend->GetRenderAudioClient()->GetBufferSize(&maxWASAPIFrames);
        UINT32 availableFrames = (maxWASAPIFrames > padding) ? (maxWASAPIFrames - padding) : 0;

        // --- 1. CAPTURE → Input Rings ---
        if (captureClient && captureFormat && m_numInputChannels > 0) {
            UINT32 packetLength = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&packetLength);
            while (SUCCEEDED(hr) && packetLength > 0) {
                BYTE* pData = nullptr; DWORD flags = 0; UINT32 framesAvail = 0;
                hr = captureClient->GetBuffer(&pData, &framesAvail, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    if (captureDeinterleaved.size() < framesAvail)
                        captureDeinterleaved.resize(framesAvail * 2);

                    float* src = reinterpret_cast<float*>(pData);
                    int nCh = captureFormat->nChannels;
                    for (int c = 0; c < m_numInputChannels && c < nCh; ++c) {
                        for (UINT32 f = 0; f < framesAvail; ++f)
                            captureDeinterleaved[f] = src[f * nCh + c];
                        m_inputRings[c]->Push(captureDeinterleaved.data(), framesAvail);
                    }
                    captureClient->ReleaseBuffer(framesAvail);
                }
                hr = captureClient->GetNextPacketSize(&packetLength);
            }
        } else if (m_numInputChannels > 0 && availableFrames > 0) {
            for (int c = 0; c < m_numInputChannels; ++c)
                m_inputRings[c]->PushSilence(availableFrames);
        }

        // --- 2. ASIO Processing: Paced Rendering ---
        bool canProcess = true;
        // The output ring should hold enough data to survive WASAPI reads, but not fill to 100%.
        // By breaking early, we prevent burst-rendering that starves the WASAPI thread.
        size_t targetDepth = (size_t)((std::max)(m_bufferSize, m_wasapiPeriod)) * 2;

        while (canProcess) {
            // Check if we already have enough data in the output ring to safely survive
            if (m_numOutputChannels > 0 && !m_outputRings.empty()) {
                if (m_outputRings[0]->GetAvailableRead() >= targetDepth) {
                    canProcess = false; 
                    break;
                }
            }

            if (m_numInputChannels > 0 && !m_inputRings.empty()) {
                if (m_inputRings[0]->GetAvailableRead() < (size_t)m_bufferSize) {
                    canProcess = false; break;
                }
            }
            if (m_numOutputChannels > 0 && !m_outputRings.empty()) {
                if (m_outputRings[0]->GetAvailableWrite() < (size_t)m_bufferSize) {
                    canProcess = false; break;
                }
            }

            for (int c = 0; c < m_numInputChannels; ++c) {
                float* dest = reinterpret_cast<float*>(m_bufferInfos[c].buffers[m_asioBufferIndex]);
                m_inputRings[c]->Pop(dest, m_bufferSize);
            }

            if (m_callbacks && m_callbacks->bufferSwitch)
                m_callbacks->bufferSwitch(m_asioBufferIndex, ASIOFalse);

            for (int c = 0; c < m_numOutputChannels; ++c) {
                float* src = reinterpret_cast<float*>(
                    m_bufferInfos[m_numInputChannels + c].buffers[m_asioBufferIndex]);
                m_outputRings[c]->Push(src, m_bufferSize);
            }

            m_asioBufferIndex = m_asioBufferIndex == 0 ? 1 : 0;
        }

        // --- 3. Output Rings → WASAPI (with underrun protection) ---
        if (availableFrames > 0 && renderClient && renderFormat) {
            BYTE* pData = nullptr;
            HRESULT hr = renderClient->GetBuffer(availableFrames, &pData);
            if (SUCCEEDED(hr)) {
                int nCh = renderFormat->nChannels;
                bool underrun = (!m_outputRings.empty() &&
                    m_outputRings[0]->GetAvailableRead() < (size_t)availableFrames);

                if (underrun) {
                    // Write silence instead of garbage; count and log
                    memset(pData, 0, availableFrames * nCh * sizeof(float));
                    long prev = m_underrunCount.fetch_add(1, std::memory_order_relaxed);
                    if ((prev & 0xF) == 0) { // Log every 16 underruns to avoid spam
                        char dbg[128];
                        sprintf_s(dbg, "[LuxASIO] OUTPUT UNDERRUN #%ld (ASIO=%ld, WASAPI=%ld)\n",
                                  prev + 1, m_bufferSize, m_wasapiPeriod);
                        OutputDebugStringA(dbg);
                    }
                } else {
                    if (renderInterleaved.size() < availableFrames)
                        renderInterleaved.resize(availableFrames * 2);

                    float* dest = reinterpret_cast<float*>(pData);
                    for (int c = 0; c < m_numOutputChannels && c < nCh; ++c) {
                        m_outputRings[c]->Pop(renderInterleaved.data(), availableFrames);
                        for (UINT32 f = 0; f < availableFrames; ++f)
                            dest[f * nCh + c] = renderInterleaved[f];
                    }
                }
                renderClient->ReleaseBuffer(availableFrames, 0);
            }
        }
    }
}
