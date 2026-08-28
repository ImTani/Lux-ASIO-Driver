#include "audio_thread.h"
#include <avrt.h>
#include <stdio.h>

AudioThread::AudioThread(WasapiBackend* backend)
    : m_backend(backend)
    , m_threadHandle(NULL)
    , m_eventHandle(NULL)
    , m_stopRequested(false)
    , m_bufferSize(0)
    , m_callbacks(nullptr)
    , m_bufferInfos(nullptr)
    , m_numInputChannels(0)
    , m_numOutputChannels(0)
    , m_asioBufferIndex(0)
{
}

AudioThread::~AudioThread()
{
    Stop();
}

bool AudioThread::Start(long bufferSize, ASIOCallbacks* callbacks, ASIOBufferInfo* bufferInfos, long numInputChannels, long numOutputChannels)
{
    m_bufferSize = bufferSize;
    m_callbacks = callbacks;
    m_bufferInfos = bufferInfos;
    m_numInputChannels = numInputChannels;
    m_numOutputChannels = numOutputChannels;
    m_asioBufferIndex = 0;
    m_stopRequested = false;

    // Allocate ring buffers
    size_t ringSize = 16384; // Large enough to buffer decoupled blocks safely
    for (int i = 0; i < m_numInputChannels; ++i) {
        m_inputRings.push_back(new RingBuffer(ringSize));
    }
    for (int i = 0; i < m_numOutputChannels; ++i) {
        auto rb = new RingBuffer(ringSize);
        // Pre-fill output with silence to prevent immediate WASAPI underflow
        rb->PushSilence(8192);
        m_outputRings.push_back(rb);
    }

    m_eventHandle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!m_eventHandle) return false;

    // Initialize WASAPI with its native preferred period (not m_bufferSize!)
    // We get the actual period by calling GetBufferSizes
    WasapiBufferSizes wSizes;
    long hardwarePeriod = 256;
    if (m_backend->GetBufferSizes(wSizes)) {
        hardwarePeriod = wSizes.minPeriodInFrames;
    }
    
    if (!m_backend->InitStreams(hardwarePeriod, m_eventHandle)) {
        CloseHandle(m_eventHandle);
        m_eventHandle = NULL;
        return false;
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

    return true;
}

void AudioThread::Stop()
{
    m_stopRequested = true;
    if (m_eventHandle) {
        SetEvent(m_eventHandle); 
    }

    if (m_threadHandle) {
        WaitForSingleObject(m_threadHandle, INFINITE);
        CloseHandle(m_threadHandle);
        m_threadHandle = NULL;
    }

    if (m_backend) {
        m_backend->Stop();
    }

    if (m_eventHandle) {
        CloseHandle(m_eventHandle);
        m_eventHandle = NULL;
    }

    for (auto rb : m_inputRings) delete rb;
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

    auto renderClient = m_backend->GetRenderClient();
    auto captureClient = m_backend->GetCaptureClient();
    auto renderFormat = m_backend->GetRenderFormat();
    auto captureFormat = m_backend->GetCaptureFormat();
    
    // For de-interleaving capture
    std::vector<float> captureDeinterleaved(16384); 
    // For interleaving render
    std::vector<float> renderInterleaved(16384);

    while (!m_stopRequested) {
        DWORD waitResult = WaitForSingleObject(m_eventHandle, 2000);
        if (m_stopRequested) break;
        if (waitResult != WAIT_OBJECT_0) {
            continue;
        }

        UINT32 padding = m_backend->GetRenderBufferPadding();
        WasapiBufferSizes wSizes;
        m_backend->GetBufferSizes(wSizes); // To get max buffer size
        UINT32 renderBufferLimit = wSizes.defaultPeriodInFrames; 
        
        // Actually for shared mode, we can write up to (Buffer Size - Padding)
        // But our InitStreams requested hardwarePeriod. The max buffer size is determined by WASAPI.
        // Let's just ask GetCurrentPadding.
        UINT32 availableFrames = 0;
        // Wait, GetBufferSizes gives period. We initialized with hardwarePeriod.
        // Buffer capacity is usually 1x or 2x the period.
        // Let's just ask how many frames we can write.
        // Unfortunately we didn't save the allocated buffer size. But IAudioClient::GetBufferSize can tell us.
        UINT32 maxWASAPIFrames = 0;
        if (m_backend->GetRenderAudioClient()) {
            m_backend->GetRenderAudioClient()->GetBufferSize(&maxWASAPIFrames);
        }
        
        if (maxWASAPIFrames > padding) {
            availableFrames = maxWASAPIFrames - padding;
        }

        // 1. CAPTURE (Push to Input Rings)
        if (captureClient && captureFormat && m_numInputChannels > 0) {
            UINT32 packetLength = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&packetLength);
            
            while (SUCCEEDED(hr) && packetLength > 0) {
                BYTE* pData = nullptr;
                DWORD flags = 0;
                UINT32 numFramesAvailable = 0;
                UINT64 devPosition = 0;
                UINT64 qpcPosition = 0;

                hr = captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, &devPosition, &qpcPosition);
                if (SUCCEEDED(hr)) {
                    if (captureFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || 
                       (captureFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && 
                        reinterpret_cast<WAVEFORMATEXTENSIBLE*>(captureFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) 
                    {
                        float* src = reinterpret_cast<float*>(pData);
                        int numChannels = captureFormat->nChannels;
                        
                        // Ensure we have enough space in temp buffer
                        if (captureDeinterleaved.size() < numFramesAvailable) {
                            captureDeinterleaved.resize(numFramesAvailable * 2);
                        }

                        for (int c = 0; c < m_numInputChannels && c < numChannels; ++c) {
                            for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                                captureDeinterleaved[f] = src[f * numChannels + c];
                            }
                            m_inputRings[c]->Push(captureDeinterleaved.data(), numFramesAvailable);
                        }
                    }
                    captureClient->ReleaseBuffer(numFramesAvailable);
                }
                hr = captureClient->GetNextPacketSize(&packetLength);
            }
        } else {
            // Fake input if no capture device
            if (m_numInputChannels > 0 && availableFrames > 0) {
                for (int c = 0; c < m_numInputChannels; ++c) {
                    m_inputRings[c]->PushSilence(availableFrames);
                }
            }
        }

        // 2. ASIO PROCESSING (Block Adaptation)
        // If we don't have inputs, we drive it by outputs instead.
        // If we have inputs, we wait until input ring has enough.
        // Actually, safer to drive by output space to prevent output ring from draining!
        // We know we just consumed some WASAPI frames. The output ring needs to be refilled.
        bool canProcess = true;
        while (canProcess) {
            // Do we have enough input frames? (If we have inputs)
            if (m_numInputChannels > 0 && m_inputRings.size() > 0) {
                if (m_inputRings[0]->GetAvailableRead() < (size_t)m_bufferSize) {
                    canProcess = false;
                }
            }
            
            // Do we have enough output space? (If we have outputs)
            if (m_numOutputChannels > 0 && m_outputRings.size() > 0) {
                if (m_outputRings[0]->GetAvailableWrite() < (size_t)m_bufferSize) {
                    canProcess = false; // We can't push more right now
                }
            }
            
            if (!canProcess) break;

            // Pop inputs to ASIO buffers
            for (int c = 0; c < m_numInputChannels; ++c) {
                float* dest = reinterpret_cast<float*>(m_bufferInfos[c].buffers[m_asioBufferIndex]);
                m_inputRings[c]->Pop(dest, m_bufferSize);
            }

            // Call DAW
            if (m_callbacks && m_callbacks->bufferSwitch) {
                m_callbacks->bufferSwitch(m_asioBufferIndex, ASIOFalse);
            }

            // Push ASIO outputs to output rings
            for (int c = 0; c < m_numOutputChannels; ++c) {
                float* src = reinterpret_cast<float*>(m_bufferInfos[m_numInputChannels + c].buffers[m_asioBufferIndex]);
                m_outputRings[c]->Push(src, m_bufferSize);
            }

            m_asioBufferIndex = m_asioBufferIndex == 0 ? 1 : 0;
        }

        // 3. RENDER (Pop from Output Rings to WASAPI)
        if (availableFrames > 0 && renderClient && renderFormat) {
            BYTE* pData = nullptr;
            HRESULT hr = renderClient->GetBuffer(availableFrames, &pData);
            if (SUCCEEDED(hr)) {
                if (renderFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || 
                   (renderFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && 
                    reinterpret_cast<WAVEFORMATEXTENSIBLE*>(renderFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) 
                {
                    float* dest = reinterpret_cast<float*>(pData);
                    int numChannels = renderFormat->nChannels;
                    
                    if (renderInterleaved.size() < availableFrames) {
                        renderInterleaved.resize(availableFrames * 2);
                    }

                    // Check if we have enough data to render (underrun check)
                    bool underrun = false;
                    if (m_numOutputChannels > 0 && m_outputRings[0]->GetAvailableRead() < availableFrames) {
                        underrun = true; 
                    }

                    if (underrun) {
                        memset(dest, 0, availableFrames * numChannels * sizeof(float));
                    } else {
                        for (int c = 0; c < m_numOutputChannels && c < numChannels; ++c) {
                            m_outputRings[c]->Pop(renderInterleaved.data(), availableFrames);
                            for (UINT32 f = 0; f < availableFrames; ++f) {
                                dest[f * numChannels + c] = renderInterleaved[f];
                            }
                        }
                    }
                } else {
                    memset(pData, 0, availableFrames * renderFormat->nBlockAlign);
                }
                
                renderClient->ReleaseBuffer(availableFrames, 0);
            }
        }
    }

    if (mmcssHandle) {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
}
