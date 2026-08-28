#pragma once
#include <windows.h>
#include "asiodrvr.h"

// CLSID for Lux ASIO Driver: {E1A4D078-4925-4C3D-B7E5-3A83FDF0A889}
DEFINE_GUID(CLSID_LuxAsioDriver, 
    0xe1a4d078, 0x4925, 0x4c3d, 0xb7, 0xe5, 0x3a, 0x83, 0xfd, 0xf0, 0xa8, 0x89);

class LuxAsioDriver : public CAsioDriver
{
public:
    LuxAsioDriver(LPUNKNOWN pUnk, HRESULT *phr);
    virtual ~LuxAsioDriver();

    static CUnknown * WINAPI CreateInstance(LPUNKNOWN pUnk, HRESULT *phr);

    virtual ASIOBool init(void *sysRef) override;
    virtual void getDriverName(char *name) override;
    virtual long getDriverVersion() override;
    virtual void getErrorMessage(char *string) override;

    virtual ASIOError start() override;
    virtual ASIOError stop() override;
    virtual ASIOError getChannels(long *numInputChannels, long *numOutputChannels) override;
    virtual ASIOError getLatencies(long *inputLatency, long *outputLatency) override;
    virtual ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) override;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getSampleRate(ASIOSampleRate *sampleRate) override;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) override;
    virtual ASIOError setClockSource(long reference) override;
    virtual ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) override;
    virtual ASIOError getChannelInfo(ASIOChannelInfo *info) override;
    virtual ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks) override;
    virtual ASIOError disposeBuffers() override;
    virtual ASIOError controlPanel() override;
    virtual ASIOError future(long selector, void *opt) override;
    virtual ASIOError outputReady() override;

private:
    bool m_active;
    ASIOSampleRate m_sampleRate;
    long m_bufferSize;
    ASIOCallbacks* m_callbacks;
    ASIOBufferInfo* m_bufferInfos;
    long m_numInputs;
    long m_numOutputs;
};
