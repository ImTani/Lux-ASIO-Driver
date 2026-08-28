cmake_minimum_required(VERSION 3.20)
project(LuxAsioDriver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared library for the COM DLL
add_library(lux_asio SHARED
    src/dllmain.cpp
    src/lux_asio.cpp
    src/lux_asio.def
    src/wasapi_backend.cpp
    src/audio_thread.cpp
    src/settings.cpp
    src/control_panel.cpp
    src/panel.rc
    external/asiosdk/common/asiodrvr.cpp
    external/asiosdk/common/combase.cpp
    external/asiosdk/common/dllentry.cpp
    external/asiosdk/common/register.cpp
)

# Required include directories for ASIO SDK
target_include_directories(lux_asio PRIVATE
    external/asiosdk/common
    external/asiosdk/host
    external/asiosdk/host/pc
    src
)

# Preprocessor definitions
target_compile_definitions(lux_asio PRIVATE
    WINDOWS=1
    _WINDOWS=1
    WIN32=1
    _USRDLL=1
)

# Compiler flags for MSVC
if(MSVC)
    # /O2: Optimize for speed, /Oi: intrinsic functions, /GL: whole program optimization, /GS-: disable buffer security check
    target_compile_options(lux_asio PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /GL /GS- /arch:AVX2>
        /Zc:strictStrings-
    )
    # /LTCG for whole program optimization
    set_property(TARGET lux_asio APPEND_STRING PROPERTY LINK_FLAGS_RELEASE " /LTCG")
endif()

# Link necessary Windows libraries for COM and Registry
target_link_libraries(lux_asio PRIVATE
    ole32
    uuid
    advapi32
    user32
    avrt
    comctl32
)
