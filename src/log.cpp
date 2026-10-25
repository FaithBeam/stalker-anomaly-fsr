#include "log.h"
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace afsr {

namespace {
Lock g_logLock;
std::string g_pending;
std::string g_polled;
constexpr size_t kMaxPending = 16 * 1024;
} // namespace

void Log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    char debugLine[1100];
    snprintf(debugLine, sizeof(debugLine), "[anomaly_fsr] %s\n", line);
    OutputDebugStringA(debugLine);

    LockGuard guard(g_logLock);
    if (g_pending.size() < kMaxPending)
    {
        g_pending += line;
        g_pending += '\n';
    }
}

const char* PollLog()
{
    LockGuard guard(g_logLock);
    g_polled.swap(g_pending);
    g_pending.clear();
    return g_polled.c_str();
}

} // namespace afsr
