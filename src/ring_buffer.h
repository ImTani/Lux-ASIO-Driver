#pragma once
#include <atomic>
#include <vector>
#include <algorithm>

class RingBuffer {
public:
    RingBuffer(size_t capacity) {
        // Round up to next power of 2 for fast modulo
        m_capacity = 1;
        while (m_capacity < capacity) m_capacity <<= 1;
        m_mask = m_capacity - 1;
        
        m_buffer.resize(m_capacity, 0.0f);
        m_readIndex.store(0, std::memory_order_relaxed);
        m_writeIndex.store(0, std::memory_order_relaxed);
    }

    size_t GetAvailableRead() const {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        return writeIdx - readIdx;
    }

    size_t GetAvailableWrite() const {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        return m_capacity - (writeIdx - readIdx);
    }

    size_t Push(const float* data, size_t count) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        size_t available = m_capacity - (writeIdx - readIdx);
        
        size_t toWrite = (std::min)(count, available);
        if (toWrite == 0) return 0;

        size_t idx1 = writeIdx & m_mask;
        size_t chunk1 = (std::min)(toWrite, m_capacity - idx1);
        std::copy(data, data + chunk1, m_buffer.begin() + idx1);

        if (chunk1 < toWrite) {
            std::copy(data + chunk1, data + toWrite, m_buffer.begin());
        }

        m_writeIndex.store(writeIdx + toWrite, std::memory_order_release);
        return toWrite;
    }
    
    // Push silence
    size_t PushSilence(size_t count) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        size_t available = m_capacity - (writeIdx - readIdx);
        
        size_t toWrite = (std::min)(count, available);
        if (toWrite == 0) return 0;

        size_t idx1 = writeIdx & m_mask;
        size_t chunk1 = (std::min)(toWrite, m_capacity - idx1);
        std::fill(m_buffer.begin() + idx1, m_buffer.begin() + idx1 + chunk1, 0.0f);

        if (chunk1 < toWrite) {
            std::fill(m_buffer.begin(), m_buffer.begin() + (toWrite - chunk1), 0.0f);
        }

        m_writeIndex.store(writeIdx + toWrite, std::memory_order_release);
        return toWrite;
    }

    size_t Pop(float* data, size_t count) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        size_t available = writeIdx - readIdx;

        size_t toRead = (std::min)(count, available);
        if (toRead == 0) return 0;

        size_t idx1 = readIdx & m_mask;
        size_t chunk1 = (std::min)(toRead, m_capacity - idx1);
        std::copy(m_buffer.begin() + idx1, m_buffer.begin() + idx1 + chunk1, data);

        if (chunk1 < toRead) {
            std::copy(m_buffer.begin(), m_buffer.begin() + (toRead - chunk1), data + chunk1);
        }

        m_readIndex.store(readIdx + toRead, std::memory_order_release);
        return toRead;
    }

    void Clear() {
        m_readIndex.store(0, std::memory_order_relaxed);
        m_writeIndex.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float> m_buffer;
    size_t m_capacity;
    size_t m_mask;
    
    // Indices grow indefinitely. We use unsigned arithmetic for safe wrap-around.
    alignas(64) std::atomic<size_t> m_readIndex;
    alignas(64) std::atomic<size_t> m_writeIndex;
};
