// Lux ASIO loopback latency benchmark (roadmap Phase 1 / Phase 6).
//
// Measures the REAL round-trip latency of any ASIO driver by playing a chirp
// through the driver's outputs and cross-correlating the driver's inputs
// against the emitted template. The measured delay = true output latency +
// acoustic path (speaker->mic, ~1 ms) + true input latency. Because Ableton
// and every other host merely DISPLAY whatever getLatencies() claims, this is
// the only way to compare drivers honestly.
//
// Usage:
//   lux_latency_bench.exe --list
//   lux_latency_bench.exe --driver "FlexASIO"            (registered driver by name)
//   lux_latency_bench.exe --dll path\to\lux_asio.dll     (unregistered Lux build)
//
// Output per run: reported input/output latency (driver's claim) vs measured
// round-trip (median of trials), plus buffer size, sample rate, and signal
// quality diagnostics. Requires speakers audible to the default microphone
// (or any capture device the driver under test uses).

#include <windows.h>
#include <initguid.h>
#include <atomic>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <algorithm>

#include "iasiodrv.h"

typedef HRESULT (STDAPICALLTYPE *DllGetClassObjectFunc)(REFCLSID, REFIID, void**);

// ---------------------------------------------------------------------------
// Globals shared with the ASIO callbacks (ASIO callbacks carry no context ptr)
// ---------------------------------------------------------------------------
static IASIO* g_asio = nullptr;
static std::vector<ASIOBufferInfo> g_bufferInfos;
static std::vector<ASIOChannelInfo> g_channelInfos;
static long g_numInputs = 0, g_numOutputs = 0;
static long g_bufferSize = 0;
static double g_sampleRate = 48000.0;

static std::vector<float> g_chirp;          // emission template
static std::vector<long long> g_emitPositions; // global output sample pos of each emission start
static long long g_streamPos = 0;           // global sample position (counts callbacks)
static long long g_nextEmitPos = 0;
static long g_emitRemaining = 0;            // samples of chirp left to emit
static long g_emitOffset = 0;
static int g_emissionsWanted = 8;
static int g_emissionsDone = 0;

static std::vector<float> g_captured;       // mono mix of all inputs, indexed by g_streamPos
static std::atomic<bool> g_done{false};

// ---------------------------------------------------------------------------
// Sample-type conversion (drivers differ: Lux=float32, FlexASIO/ASIO4ALL=int32...)
// ---------------------------------------------------------------------------
static float ReadSample(const void* buf, long type, long index) {
    switch (type) {
        case ASIOSTFloat32LSB: return ((const float*)buf)[index];
        case ASIOSTFloat64LSB: return (float)((const double*)buf)[index];
        case ASIOSTInt32LSB:   return ((const INT32*)buf)[index] / 2147483648.0f;
        case ASIOSTInt24LSB: {
            const BYTE* p = (const BYTE*)buf + index * 3;
            INT32 v = (INT32)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000) v |= 0xFF000000;
            return v / 8388608.0f;
        }
        case ASIOSTInt16LSB:   return ((const INT16*)buf)[index] / 32768.0f;
        default:               return 0.0f;
    }
}

static void WriteSample(void* buf, long type, long index, float v) {
    v = (v > 1.0f) ? 1.0f : (v < -1.0f) ? -1.0f : v;
    switch (type) {
        case ASIOSTFloat32LSB: ((float*)buf)[index] = v; break;
        case ASIOSTFloat64LSB: ((double*)buf)[index] = v; break;
        case ASIOSTInt32LSB:   ((INT32*)buf)[index] = (INT32)(v * 2147483520.0f); break;
        case ASIOSTInt24LSB: {
            INT32 s = (INT32)(v * 8388607.0f);
            BYTE* p = (BYTE*)buf + index * 3;
            p[0] = s & 0xFF; p[1] = (s >> 8) & 0xFF; p[2] = (s >> 16) & 0xFF;
            break;
        }
        case ASIOSTInt16LSB:   ((INT16*)buf)[index] = (INT16)(v * 32767.0f); break;
        default: break;
    }
}

static void ZeroBuffer(void* buf, long type, long frames) {
    int bytes = 4;
    if (type == ASIOSTInt24LSB) bytes = 3;
    else if (type == ASIOSTInt16LSB) bytes = 2;
    else if (type == ASIOSTFloat64LSB) bytes = 8;
    memset(buf, 0, (size_t)frames * bytes);
}

// ---------------------------------------------------------------------------
// The measurement callback: capture inputs, emit chirps at known positions
// ---------------------------------------------------------------------------
static void ProcessBlock(long index) {
    if (g_done.load(std::memory_order_relaxed)) return;

    // 1. Record inputs (mono mix) at the current global position
    size_t base = (size_t)g_streamPos;
    if (base + g_bufferSize <= g_captured.size() && g_numInputs > 0) {
        for (long c = 0; c < g_numInputs; ++c) {
            const void* buf = g_bufferInfos[c].buffers[index];
            long type = g_channelInfos[c].type;
            for (long f = 0; f < g_bufferSize; ++f)
                g_captured[base + f] += ReadSample(buf, type, f) / (float)g_numInputs;
        }
    }

    // 2. Emit: zero outputs, then overlay chirp when scheduled
    for (long c = 0; c < g_numOutputs; ++c) {
        void* buf = g_bufferInfos[g_numInputs + c].buffers[index];
        ZeroBuffer(buf, g_channelInfos[g_numInputs + c].type, g_bufferSize);
    }

    for (long f = 0; f < g_bufferSize; ++f) {
        long long pos = g_streamPos + f;
        if (g_emitRemaining == 0 && g_emissionsDone < g_emissionsWanted && pos >= g_nextEmitPos) {
            g_emitPositions.push_back(pos);
            g_emitRemaining = (long)g_chirp.size();
            g_emitOffset = 0;
            g_emissionsDone++;
            g_nextEmitPos = pos + (long long)(g_sampleRate * 0.5); // 500 ms spacing
        }
        if (g_emitRemaining > 0) {
            float s = g_chirp[g_emitOffset];
            for (long c = 0; c < g_numOutputs; ++c) {
                void* buf = g_bufferInfos[g_numInputs + c].buffers[index];
                WriteSample(buf, g_channelInfos[g_numInputs + c].type, f, s);
            }
            g_emitOffset++;
            g_emitRemaining--;
        }
    }

    if (g_asio) g_asio->outputReady();
    g_streamPos += g_bufferSize;

    // Done when all emissions are out and 1.2 s of tail has been captured
    if (g_emissionsDone >= g_emissionsWanted && g_emitRemaining == 0 &&
        g_streamPos > g_nextEmitPos + (long long)(g_sampleRate * 1.2)) {
        g_done.store(true, std::memory_order_relaxed);
    }
    if ((size_t)g_streamPos + g_bufferSize >= g_captured.size()) {
        g_done.store(true, std::memory_order_relaxed); // safety: capture buffer full
    }
}

static void CbBufferSwitch(long index, ASIOBool) { ProcessBlock(index); }
static ASIOTime* CbBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) { ProcessBlock(index); return t; }
static void CbSampleRateDidChange(ASIOSampleRate) {}
static long CbAsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
        case kAsioSelectorSupported:
            return (value == kAsioSupportsTimeInfo || value == kAsioEngineVersion) ? 1 : 0;
        case kAsioEngineVersion:    return 2;
        case kAsioSupportsTimeInfo: return 1;
        default:                    return 0;
    }
}

// ---------------------------------------------------------------------------
// Driver loading
// ---------------------------------------------------------------------------
static IASIO* LoadRegisteredDriver(const std::wstring& name, std::wstring& outName) {
    HKEY hAsio;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hAsio) != ERROR_SUCCESS)
        return nullptr;

    IASIO* result = nullptr;
    for (DWORD i = 0;; i++) {
        wchar_t keyName[256];
        DWORD keyLen = 256;
        if (RegEnumKeyExW(hAsio, i, keyName, &keyLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (_wcsicmp(keyName, name.c_str()) != 0) continue;

        HKEY hDrv;
        if (RegOpenKeyExW(hAsio, keyName, 0, KEY_READ, &hDrv) == ERROR_SUCCESS) {
            wchar_t clsidStr[64] = L"";
            DWORD sz = sizeof(clsidStr);
            RegGetValueW(hDrv, NULL, L"CLSID", RRF_RT_REG_SZ, NULL, clsidStr, &sz);
            RegCloseKey(hDrv);

            CLSID clsid;
            if (SUCCEEDED(CLSIDFromString(clsidStr, &clsid))) {
                void* p = nullptr;
                // ASIO convention: IID == CLSID
                if (SUCCEEDED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid, &p))) {
                    result = (IASIO*)p;
                    outName = keyName;
                }
            }
        }
        break;
    }
    RegCloseKey(hAsio);
    return result;
}

// {B9721DFB-6832-4752-B6CD-369F9DF4E383} — Lux driver CLSID for --dll mode
DEFINE_GUID(CLSID_LuxAsioDriver,
0xb9721dfb, 0x6832, 0x4752, 0xb6, 0xcd, 0x36, 0x9f, 0x9d, 0xf4, 0xe3, 0x83);

static IASIO* LoadDllDriver(const char* path, HMODULE& outModule) {
    outModule = LoadLibraryA(path);
    if (!outModule) return nullptr;
    auto gco = (DllGetClassObjectFunc)GetProcAddress(outModule, "DllGetClassObject");
    if (!gco) return nullptr;
    IClassFactory* factory = nullptr;
    if (FAILED(gco(CLSID_LuxAsioDriver, IID_IClassFactory, (void**)&factory))) return nullptr;
    IASIO* asio = nullptr;
    factory->CreateInstance(nullptr, CLSID_LuxAsioDriver, (void**)&asio);
    factory->Release();
    return asio;
}

static void ListDrivers() {
    HKEY hAsio;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hAsio) != ERROR_SUCCESS) return;
    printf("Registered ASIO drivers:\n");
    for (DWORD i = 0;; i++) {
        wchar_t keyName[256]; DWORD keyLen = 256;
        if (RegEnumKeyExW(hAsio, i, keyName, &keyLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wprintf(L"  %s\n", keyName);
    }
    RegCloseKey(hAsio);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (argc >= 2 && strcmp(argv[1], "--list") == 0) { ListDrivers(); return 0; }

    IASIO* asio = nullptr;
    HMODULE dllModule = NULL;
    std::wstring driverLabel = L"?";

    if (argc >= 3 && strcmp(argv[1], "--driver") == 0) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
        std::wstring wname(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wname.data(), wlen);
        wname.resize(wcslen(wname.c_str()));
        asio = LoadRegisteredDriver(wname, driverLabel);
        if (!asio) { printf("FAILED to load registered driver '%s'\n", argv[2]); return 1; }
    } else if (argc >= 3 && strcmp(argv[1], "--dll") == 0) {
        asio = LoadDllDriver(argv[2], dllModule);
        driverLabel = L"(dll)";
        if (!asio) { printf("FAILED to load DLL driver '%s'\n", argv[2]); return 1; }
    } else {
        printf("Usage: %s --list | --driver <name> | --dll <path>\n", argv[0]);
        return 1;
    }

    wprintf(L"=== %s ===\n", driverLabel.c_str());

    if (asio->init(GetDesktopWindow()) != ASIOTrue) {
        char err[128] = {0};
        asio->getErrorMessage(err);
        printf("init() failed: %s\n", err);
        asio->Release();
        return 1;
    }

    char name[64] = {0};
    asio->getDriverName(name);
    printf("driver: %s\n", name);

    // Prefer 48 kHz; fall back to whatever the driver runs at
    if (asio->canSampleRate(48000.0) == ASE_OK) asio->setSampleRate(48000.0);
    asio->getSampleRate(&g_sampleRate);
    if (g_sampleRate <= 0) g_sampleRate = 48000.0;

    asio->getChannels(&g_numInputs, &g_numOutputs);
    if (g_numOutputs < 1 || g_numInputs < 1) {
        printf("need at least 1 input and 1 output (have %ld in / %ld out)\n", g_numInputs, g_numOutputs);
        asio->Release();
        return 1;
    }
    // Cap to keep the load small and uniform
    g_numInputs = (std::min)(g_numInputs, 2L);
    g_numOutputs = (std::min)(g_numOutputs, 2L);

    long minS = 0, maxS = 0, prefS = 0, gran = 0;
    asio->getBufferSize(&minS, &maxS, &prefS, &gran);
    g_bufferSize = prefS > 0 ? prefS : 256;

    printf("rate: %.0f Hz | buffer: %ld samples | channels: %ld in / %ld out\n",
           g_sampleRate, g_bufferSize, g_numInputs, g_numOutputs);

    // Build the chirp template: 250 ms Hann-windowed 400 Hz -> 8 kHz sweep
    // (long sweep = high correlation gain, survives quiet speakers and mic DSP)
    long chirpLen = (long)(g_sampleRate * 0.25);
    g_chirp.resize(chirpLen);
    double f0 = 400.0, f1 = 8000.0, T = chirpLen / g_sampleRate;
    for (long i = 0; i < chirpLen; ++i) {
        double t = i / g_sampleRate;
        double phase = 2.0 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2.0 * T));
        double window = 0.5 * (1.0 - cos(2.0 * 3.14159265358979 * i / (chirpLen - 1)));
        g_chirp[i] = (float)(0.7 * window * sin(phase));
    }

    // Capture buffer: 8 emissions x 0.5 s + lead-in/tail
    g_captured.assign((size_t)(g_sampleRate * 8.0), 0.0f);
    g_streamPos = 0;
    g_emitPositions.clear();
    g_emissionsDone = 0;
    g_emitRemaining = 0;
    g_nextEmitPos = (long long)(g_sampleRate * 0.6); // let the stream settle first
    g_done = false;

    // Create buffers on all (capped) channels
    long total = g_numInputs + g_numOutputs;
    g_bufferInfos.assign(total, {});
    g_channelInfos.assign(total, {});
    long idx = 0;
    for (long c = 0; c < g_numInputs;  c++) { g_bufferInfos[idx].isInput = ASIOTrue;  g_bufferInfos[idx].channelNum = c; idx++; }
    for (long c = 0; c < g_numOutputs; c++) { g_bufferInfos[idx].isInput = ASIOFalse; g_bufferInfos[idx].channelNum = c; idx++; }

    ASIOCallbacks cb{};
    cb.bufferSwitch = CbBufferSwitch;
    cb.bufferSwitchTimeInfo = CbBufferSwitchTimeInfo;
    cb.sampleRateDidChange = CbSampleRateDidChange;
    cb.asioMessage = CbAsioMessage;

    g_asio = asio;
    if (asio->createBuffers(g_bufferInfos.data(), total, g_bufferSize, &cb) != ASE_OK) {
        printf("createBuffers failed\n");
        asio->Release();
        return 1;
    }

    for (long i = 0; i < total; i++) {
        g_channelInfos[i].channel = g_bufferInfos[i].channelNum;
        g_channelInfos[i].isInput = g_bufferInfos[i].isInput;
        asio->getChannelInfo(&g_channelInfos[i]);
    }

    long repIn = 0, repOut = 0;
    asio->getLatencies(&repIn, &repOut);
    printf("REPORTED: in=%ld out=%ld frames (%.2f / %.2f ms)\n",
           repIn, repOut, repIn * 1000.0 / g_sampleRate, repOut * 1000.0 / g_sampleRate);

    if (asio->start() != ASE_OK) {
        printf("start() failed\n");
        asio->disposeBuffers();
        asio->Release();
        return 1;
    }

    // Wait for the measurement to finish (max 12 s)
    for (int i = 0; i < 240 && !g_done.load(); i++) Sleep(50);
    asio->stop();
    asio->disposeBuffers();
    asio->Release();
    if (dllModule) FreeLibrary(dllModule);

    // --- Analysis: cross-correlate each emission against the capture -------
    double rms = 0;
    for (float v : g_captured) rms += (double)v * v;
    rms = sqrt(rms / g_captured.size());
    printf("capture RMS: %.5f | emissions: %d\n", rms, (int)g_emitPositions.size());

    double tNorm = 0;
    for (float v : g_chirp) tNorm += (double)v * v;

    struct Trial { double delayMs; double score; };
    std::vector<Trial> trials;
    for (long long emitPos : g_emitPositions) {
        long long searchStart = emitPos;
        long long searchEnd = emitPos + (long long)(g_sampleRate * 0.45); // up to 450 ms RTT
        if ((size_t)(searchEnd + g_chirp.size()) > g_captured.size()) continue;

        double bestScore = 0;
        long long bestLag = -1;
        for (long long lag = searchStart; lag < searchEnd; ++lag) {
            double dot = 0, energy = 1e-12;
            const float* cap = g_captured.data() + lag;
            for (size_t i = 0; i < g_chirp.size(); ++i) {
                dot += (double)cap[i] * g_chirp[i];
                energy += (double)cap[i] * cap[i];
            }
            double score = dot / sqrt(energy * tNorm); // normalized correlation
            if (score > bestScore) { bestScore = score; bestLag = lag; }
        }

        printf("  trial @%.1fs: best corr %.3f at +%.2f ms\n",
               emitPos / g_sampleRate, bestScore,
               bestLag >= 0 ? (bestLag - emitPos) * 1000.0 / g_sampleRate : -1.0);

        if (bestLag >= 0)
            trials.push_back({ (bestLag - emitPos) * 1000.0 / g_sampleRate, bestScore });
    }

    // Robust acceptance: a real chirp produces the SAME delay on every trial;
    // noise produces random lags. Cluster around the median and require
    // majority agreement within +/-4 ms instead of a hard score threshold.
    std::vector<double> delays;
    if (!trials.empty()) {
        std::vector<double> all;
        for (auto& t : trials) all.push_back(t.delayMs);
        std::sort(all.begin(), all.end());
        double med = all[all.size() / 2];
        for (auto& t : trials)
            if (fabs(t.delayMs - med) <= 4.0) delays.push_back(t.delayMs);
        if (delays.size() < (trials.size() + 1) / 2) delays.clear(); // no majority = noise
    }

    if (delays.empty()) {
        printf("MEASURED: no consistent chirp detected — check speaker volume and microphone.\n");
        return 2;
    }

    std::sort(delays.begin(), delays.end());
    double median = delays[delays.size() / 2];
    double lo = delays.front(), hi = delays.back();
    printf("MEASURED round-trip: %.2f ms median (%zu/%d trials in cluster, spread %.2f..%.2f ms)\n",
           median, delays.size(), (int)g_emitPositions.size(), lo, hi);
    printf("REPORTED round-trip: %.2f ms (in+out) | delta (measured - reported - acoustic): %.2f ms\n",
           (repIn + repOut) * 1000.0 / g_sampleRate,
           median - (repIn + repOut) * 1000.0 / g_sampleRate);

    CoUninitialize();
    return 0;
}
