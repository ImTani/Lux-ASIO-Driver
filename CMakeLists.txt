cmake_minimum_required(VERSION 3.15)
project(LuxASIO LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ASIO_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/asiosdk)

include_directories(
    src
    ${ASIO_SDK_DIR}/common
    ${ASIO_SDK_DIR}/host
    ${ASIO_SDK_DIR}/host/pc
)

add_library(lux_asio SHARED
    src/lux_asio.cpp
    src/dllmain.cpp
    src/lux_asio.def
    external/asiosdk/common/asiodrvr.cpp
    external/asiosdk/common/combase.cpp
    external/asiosdk/common/dllentry.cpp
    external/asiosdk/common/register.cpp
)

target_link_libraries(lux_asio PRIVATE
    ole32
    oleaut32
    uuid
    advapi32
    user32
)
