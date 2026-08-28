# Lux ASIO Driver

A modern, low-latency, user-mode Windows ASIO driver built on top of the WASAPI `IAudioClient3` shared-mode engine.

## Features

- **`IAudioClient3` Low-Latency Shared Mode:** Leverages the Windows low-latency audio engine period negotiation while allowing simultaneous multi-client audio output (e.g. DAW + YouTube / Spotify).
- **Buffer Decoupling Engine:** Lock-free atomic ring buffers bridge the DAW's ASIO block size (64 to 2048 samples) and the hardware endpoint's native WASAPI period, solving the fixed-buffer flaw.
- **MMCSS Pro Audio Thread Scheduling:** Real-time audio processing thread registered with Windows MMCSS (`AvSetMmThreadCharacteristicsW`) for glitch-free performance.
- **Native Hardware Setup Control Panel:** Lightweight Win32 dialog allowing real-time device selection and buffer size switching via `kAsioResetRequest`.
- **Pure User-Mode COM Component:** 100% user-mode implementation without kernel-mode driver dependencies.

## Architecture

- **ASIO Shim (`src/lux_asio.cpp`):** Implementation of Steinberg's `IASIO` COM interface.
- **WASAPI Backend (`src/wasapi_backend.cpp`):** Handles `IAudioClient3` device discovery, format negotiation, and shared streams.
- **Audio Thread & Ring Buffer (`src/audio_thread.cpp`, `src/ring_buffer.h`):** High-priority worker loop with SPSC lock-free block adaptation.
- **Control Panel (`src/control_panel.cpp`, `src/panel.rc`):** Native Windows dialog for device enumeration and buffer reconfiguration.

## Building

### Prerequisites
- Visual Studio 2022 / Build Tools with C++ and ATL support
- CMake (>= 3.15)
- Windows 10/11 SDK

### Build Steps
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Registration
Register the driver with Windows ASIO registry:
```powershell
# Double-click or merge register.reg, or run:
reg import register.reg
```
Then select **Lux ASIO Driver** in your DAW (Ableton Live, FL Studio, Reaper, Cubase, etc.).
