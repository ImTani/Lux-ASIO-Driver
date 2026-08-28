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

private:
    static DWORD WINAPI ThreadProc(LPVOID lpParam);
    void Run();

    WasapiBackend* m_backend;
    HANDLE m_threadHandle;
    HANDLE m_eventHandle;
    std::atomic<bool> m_stopRequested;
    
    long m_bufferSize;
    ASIOCallbacks* m_callbacks;
    ASIOBufferInfo* m_bufferInfos;
    long m_numInputChannels;
    long m_numOutputChannels;
    
    // Decoupling Ring Buffers (size determined by max of ASIO or WASAPI buffers, let's use 8192)
    std::vector<RingBuffer*> m_inputRings;
    std::vector<RingBuffer*> m_outputRings;
    
    // Toggle ASIO double buffer index (0 or 1)
    long m_asioBufferIndex;
};
