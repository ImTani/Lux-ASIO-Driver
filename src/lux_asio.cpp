#include "lux_asio.h"
#include <string.h>
#include <stdio.h>

CUnknown * WINAPI LuxAsioDriver::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
    return new LuxAsioDriver(pUnk, phr);
}

LuxAsioDriver::LuxAsioDriver(LPUNKNOWN pUnk, HRESULT *phr)
    : CAsioDriver(L"Lux ASIO Driver", pUnk, phr)
    , m_active(false)
    , m_sampleRate(48000.0)
    , m_bufferSize(512)
    , m_callbacks(nullptr)
    , m_bufferInfos(nullptr)
    , m_numInputs(2)
    , m_numOutputs(2)
{
    if (phr) {
        *phr = S_OK;
    }
}

LuxAsioDriver::~LuxAsioDriver()
{
    disposeBuffers();
}

ASIOBool LuxAsioDriver::init(void* sysRef)
{
    return ASIOTrue;
}

void LuxAsioDriver::getDriverName(char *name)
{
    strcpy_s(name, 32, "Lux ASIO Driver");
}

long LuxAsioDriver::getDriverVersion()
{
    return 1;
}

void LuxAsioDriver::getErrorMessage(char *string)
{
    strcpy_s(string, 128, "No error.");
}

ASIOError LuxAsioDriver::start()
{
    m_active = true;
    return ASE_OK;
}

ASIOError LuxAsioDriver::stop()
{
    m_active = false;
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
    if (minSize) *minSize = 64;
    if (maxSize) *maxSize = 2048;
    if (preferredSize) *preferredSize = 512;
    if (granularity) *granularity = 0;
    return ASE_OK;
}

ASIOError LuxAsioDriver::canSampleRate(ASIOSampleRate sampleRate)
{
    if (sampleRate == 44100.0 || sampleRate == 48000.0 || sampleRate == 96000.0)
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
    if (canSampleRate(sampleRate) == ASE_OK) {
        m_sampleRate = sampleRate;
        return ASE_OK;
    }
    return ASE_NoClock;
}

ASIOError LuxAsioDriver::getClockSources(ASIOClockSource *clocks, long *numSources)
{
    if (clocks && numSources && *numSources > 0) {
        clocks[0].index = 0;
        clocks[0].associatedChannel = -1;
        clocks[0].associatedGroup = -1;
        clocks[0].isCurrentSource = ASIOTrue;
        strcpy_s(clocks[0].name, 32, "Internal Clock");
        *numSources = 1;
        return ASE_OK;
    }
    return ASE_InvalidParameter;
}

ASIOError LuxAsioDriver::setClockSource(long reference)
{
    if (reference == 0) return ASE_OK;
    return ASE_NotPresent;
}

ASIOError LuxAsioDriver::getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
    if (sPos) {
        sPos->hi = 0;
        sPos->lo = 0;
    }
    if (tStamp) {
        tStamp->hi = 0;
        tStamp->lo = 0;
    }
    return ASE_OK;
}

ASIOError LuxAsioDriver::getChannelInfo(ASIOChannelInfo *info)
{
    if (!info) return ASE_InvalidParameter;

    if (info->isInput) {
        if (info->channel >= m_numInputs) return ASE_InvalidParameter;
        info->type = ASIOSTFloat32LSB;
        info->channelGroup = 0;
        info->isActive = ASIOTrue;
        sprintf_s(info->name, 32, "In %d", info->channel + 1);
    } else {
        if (info->channel >= m_numOutputs) return ASE_InvalidParameter;
        info->type = ASIOSTFloat32LSB;
        info->channelGroup = 0;
        info->isActive = ASIOTrue;
        sprintf_s(info->name, 32, "Out %d", info->channel + 1);
    }
    return ASE_OK;
}

ASIOError LuxAsioDriver::createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks)
{
    m_bufferSize = bufferSize;
    m_callbacks = callbacks;
    m_bufferInfos = bufferInfos;

    for (long i = 0; i < numChannels; ++i) {
        bufferInfos[i].buffers[0] = new float[bufferSize]();
        bufferInfos[i].buffers[1] = new float[bufferSize]();
    }
    return ASE_OK;
}

ASIOError LuxAsioDriver::disposeBuffers()
{
    if (m_bufferInfos) {
        long totalChannels = m_numInputs + m_numOutputs;
        for (long i = 0; i < totalChannels; ++i) {
            delete[] static_cast<float*>(m_bufferInfos[i].buffers[0]);
            delete[] static_cast<float*>(m_bufferInfos[i].buffers[1]);
            m_bufferInfos[i].buffers[0] = nullptr;
            m_bufferInfos[i].buffers[1] = nullptr;
        }
        m_bufferInfos = nullptr;
    }
    return ASE_OK;
}

ASIOError LuxAsioDriver::controlPanel()
{
    return ASE_NotPresent;
}

ASIOError LuxAsioDriver::future(long selector, void *opt)
{
    return ASE_SUCCESS;
}

ASIOError LuxAsioDriver::outputReady()
{
    return ASE_OK;
}
