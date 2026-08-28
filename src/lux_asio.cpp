#include "lux_asio.h"
#include "settings.h"
#include "control_panel.h"
#include <string.h>
#include <stdio.h>

CUnknown* LuxAsioDriver::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
    return new LuxAsioDriver(pUnk, phr);
}

HRESULT STDMETHODCALLTYPE AsioDriver::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    if (riid == CLSID_LuxAsioDriver) {
        return GetInterface((IASIO *)this, ppv);
    }
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

LuxAsioDriver::LuxAsioDriver(LPUNKNOWN pUnk, HRESULT *phr)
    : AsioDriver(pUnk, phr)
    , m_active(false)
    , m_buffersCreated(false)
    , m_sampleRate(44100.0)
    , m_callbacks(nullptr)
    , m_backend(new WasapiBackend())
    , m_audioThread(new AudioThread(m_backend))
    , m_bufferInfos(nullptr)
    , m_bufferSize(0)
    , m_numInputs(0)
    , m_numOutputs(0)
    , m_sysRef(NULL)
{
    if (phr) {
        *phr = S_OK;
    }
}

LuxAsioDriver::~LuxAsioDriver()
{
    stop();
    disposeBuffers();
    delete m_audioThread;
    delete m_backend;
}

ASIOBool LuxAsioDriver::init(void* sysRef)
{
    m_sysRef = (HWND)sysRef;
    
    Settings settings;
    settings.Load();

    if (!m_backend->Init(static_cast<long>(m_sampleRate), settings.GetRenderEndpointId(), settings.GetCaptureEndpointId())) {
        return ASIOFalse;
    }
    
    // Determine channel counts from WASAPI formats
    m_numOutputs = 0;
    if (m_backend->GetRenderFormat()) {
        m_numOutputs = m_backend->GetRenderFormat()->nChannels;
    }
    
    m_numInputs = 0;
    if (m_backend->GetCaptureFormat()) {
        m_numInputs = m_backend->GetCaptureFormat()->nChannels;
    }

    m_active = false;
    m_buffersCreated = false;
    return ASIOTrue;
}

void LuxAsioDriver::getDriverName(char *name)
{
    strcpy(name, "Lux ASIO Driver");
}

long LuxAsioDriver::getDriverVersion()
{
    return 1;
}

void LuxAsioDriver::getErrorMessage(char *string)
{
    strcpy(string, "No Error");
}

ASIOError LuxAsioDriver::start()
{
    if (m_callbacks) {
        if (m_audioThread->Start(m_bufferSize, m_callbacks, m_bufferInfos, m_numInputs, m_numOutputs)) {
            m_active = true;
            return ASE_OK;
        }
    }
    return ASE_NotPresent;
}

ASIOError LuxAsioDriver::stop()
{
    m_active = false;
    m_audioThread->Stop();
    return ASE_OK;
}

ASIOError LuxAsioDriver::getChannels(long *numInputChannels, long *numOutputChannels)
{
    if (numInputChannels) *numInputChannels = m_numInputs;
    if (numOutputChannels) *numOutputChannels = m_numOutputs;
    return ASE_OK;
}

ASIOError LuxAsioDriver::getLatencies(long *inputLatency, long *outputLatency)
{
    if (inputLatency) *inputLatency = m_bufferSize;
    if (outputLatency) *outputLatency = m_bufferSize;
    return ASE_OK;
}

ASIOError LuxAsioDriver::getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
    // Decoupled from WASAPI: we offer a highly flexible range for the DAW
    if (minSize) *minSize = 64;
    if (maxSize) *maxSize = 2048;
    if (preferredSize) *preferredSize = 256;
    if (granularity) *granularity = -1; // Power of 2 sizes preferred
    return ASE_OK;
}

ASIOError LuxAsioDriver::canSampleRate(ASIOSampleRate sampleRate)
{
    if (sampleRate == 44100.0 || sampleRate == 48000.0)
        return ASE_OK;
    return ASE_NoClock;
}

ASIOError LuxAsioDriver::getSampleRate(ASIOSampleRate *sampleRate)
{
    if (sampleRate) *sampleRate = m_sampleRate;
    return ASE_OK;
}

ASIOError LuxAsioDriver::setSampleRate(ASIOSampleRate sampleRate)
{
    if (sampleRate == 44100.0 || sampleRate == 48000.0) {
        m_sampleRate = sampleRate;
        return ASE_OK;
    }
    return ASE_NoClock;
}

ASIOError LuxAsioDriver::getClockSources(ASIOClockSource *clocks, long *numSources)
{
    if (!clocks || !numSources) return ASE_InvalidParameter;

    // Just report one internal clock source
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy(clocks[0].name, "Internal");

    *numSources = 1;
    return ASE_OK;
}

ASIOError LuxAsioDriver::setClockSource(long reference)
{
    if (reference == 0) return ASE_OK;
    return ASE_InvalidParameter;
}

ASIOError LuxAsioDriver::getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
    // Return dummy positions
    sPos->hi = 0;
    sPos->lo = 0;
    tStamp->hi = 0;
    tStamp->lo = 0;
    return ASE_OK;
}

ASIOError LuxAsioDriver::getChannelInfo(ASIOChannelInfo *info)
{
    if (!info) return ASE_InvalidParameter;

    info->channelGroup = 0;
    info->type = ASIOSTFloat32LSB; // We natively copy 32-bit floats
    
    if (info->isInput) {
        if (info->channel < m_numInputs) {
            info->isActive = ASIOTrue;
            sprintf(info->name, "Lux In %d", info->channel + 1);
            return ASE_OK;
        }
    } else {
        if (info->channel < m_numOutputs) {
            info->isActive = ASIOTrue;
            sprintf(info->name, "Lux Out %d", info->channel + 1);
            return ASE_OK;
        }
    }
    
    return ASE_InvalidParameter;
}

ASIOError LuxAsioDriver::createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks)
{
    if (m_buffersCreated) return ASE_InvalidMode;

    m_callbacks = callbacks;
    m_bufferInfos = bufferInfos;
    m_bufferSize = bufferSize;
    
    for (int i = 0; i < numChannels; i++) {
        m_bufferInfos[i].buffers[0] = new float[bufferSize];
        m_bufferInfos[i].buffers[1] = new float[bufferSize];
        
        memset(m_bufferInfos[i].buffers[0], 0, bufferSize * sizeof(float));
        memset(m_bufferInfos[i].buffers[1], 0, bufferSize * sizeof(float));
    }
    
    m_buffersCreated = true;
    return ASE_OK;
}

ASIOError LuxAsioDriver::disposeBuffers()
{
    if (!m_buffersCreated) return ASE_OK;
    
    long totalChannels = m_numInputs + m_numOutputs;
    if (m_bufferInfos) {
        for (int i = 0; i < totalChannels; i++) {
            delete[] (float*)m_bufferInfos[i].buffers[0];
            delete[] (float*)m_bufferInfos[i].buffers[1];
            m_bufferInfos[i].buffers[0] = nullptr;
            m_bufferInfos[i].buffers[1] = nullptr;
        }
    }
    
    m_buffersCreated = false;
    m_callbacks = nullptr;
    m_bufferInfos = nullptr;
    return ASE_OK;
}

ASIOError LuxAsioDriver::controlPanel()
{
    ShowControlPanel(m_sysRef);
    if (DidSettingsChange()) {
        ClearSettingsChangedFlag();
        if (m_callbacks && m_callbacks->asioMessage) {
            m_callbacks->asioMessage(kAsioResetRequest, 0, 0, 0);
        }
    }
    return ASE_OK;
}

ASIOError LuxAsioDriver::future(long selector, void *opt)
{
    return ASE_NotPresent;
}

ASIOError LuxAsioDriver::outputReady()
{
    return ASE_OK; // Supported
}
