#pragma once

#include <windows.h>
#include <cstdarg>

// Debug instrumentation.
//
// Debug builds record hook events, coordinate conversions and timings so that
// problems that do not reproduce on demand can be diagnosed from a log instead
// of from guesses. In release builds every SC_LOG call disappears entirely.
//
// Output goes to two places:
//   1. OutputDebugStringW (debugger, DebugView)
//   2. %LOCALAPPDATA%\SweepCap\sweepcap-debug.log
//
// Never call this from a hook callback. File I/O is forbidden there.
//
// Message text stays Korean: the log is read by the developer during testing.

namespace sc::log {

void Init();
void Shutdown();
void Write(const wchar_t* fmt, ...);

// GDI and USER object counts, handle count and working set, written with the
// given tag.
//
// Called once per capture, so a leak shows up as a number that only ever
// climbs. The realistic failure in this application is not a dangling pointer
// but a GDI or COM object that is never released, and that is invisible in the
// working set until there are thousands of them.
void WriteResourceUsage(const wchar_t* tag);

// In release builds the arguments are left unevaluated but still count as
// "used", so removing the logging does not produce unused-variable warnings.
inline int Sink(...) { return 0; }

}  // namespace sc::log

#if defined(_DEBUG)
#define SC_LOG(...) ::sc::log::Write(__VA_ARGS__)
#define SC_LOG_RESOURCES(tag) ::sc::log::WriteResourceUsage(tag)
#else
#define SC_LOG(...) ((void)sizeof(::sc::log::Sink(__VA_ARGS__)))
#define SC_LOG_RESOURCES(tag) ((void)sizeof(::sc::log::Sink(tag)))
#endif
