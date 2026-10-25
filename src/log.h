#pragma once

namespace afsr {

// Messages go to OutputDebugString and to a pending buffer that the Lua side drains into the game log.
void Log(const char* fmt, ...);

// Returns all messages logged since the previous call (newline separated), or "" if none.
// The pointer stays valid until the next call.
const char* PollLog();

} // namespace afsr
