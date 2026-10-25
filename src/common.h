#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>

namespace afsr {

// User-facing settings, written from the Lua side (MCM) and read by the render thread every frame.
struct Settings
{
    std::atomic<bool> enabled{true};
    std::atomic<bool> sharpening{true};
    std::atomic<float> sharpness{0.25f}; // RCAS stops: 0 = sharpest, 2 = very soft
};

Settings& GetSettings();

// GetProcAddress with a typed result (the void* hop avoids -Wcast-function-type).
template <typename Fn>
inline Fn GetProc(HMODULE module, const char* name)
{
    return module ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
}

template <typename T>
inline void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

// Small RAII wrapper around SRWLOCK (avoids pulling winpthreads into MinGW builds via std::mutex).
class Lock
{
public:
    void lock() { AcquireSRWLockExclusive(&m_lock); }
    void unlock() { ReleaseSRWLockExclusive(&m_lock); }

private:
    SRWLOCK m_lock = SRWLOCK_INIT;
};

class LockGuard
{
public:
    explicit LockGuard(Lock& lock) : m_lock(lock) { m_lock.lock(); }
    ~LockGuard() { m_lock.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& m_lock;
};

} // namespace afsr
