#pragma once

#include <windows.h>
#include <cstdarg>

// 디버그 계측기.
//
// CLAUDE.md "설계 규칙": 재현이 안 되는 문제에 추측으로 수정을 쌓지 않기 위해
// 디버그 빌드에 계측기를 넣는다. 릴리스 빌드에서는 SC_LOG 호출이 통째로 사라진다.
//
// 출력은 두 군데로 간다.
//   1. OutputDebugStringW  (디버거, DebugView)
//   2. %LOCALAPPDATA%\SweepCap\sweepcap-debug.log
//
// 훅 콜백 안에서는 호출하지 않는다. 파일 I/O는 콜백에서 금지다.

namespace sc::log {

void Init();
void Shutdown();
void Write(const wchar_t* fmt, ...);
const wchar_t* FilePath();

// 릴리스에서 인자를 평가하지 않고 "사용됨"으로만 표시해 미사용 경고를 막는다.
inline int Sink(...) { return 0; }

}  // namespace sc::log

#if defined(_DEBUG)
#define SC_LOG(...) ::sc::log::Write(__VA_ARGS__)
#else
#define SC_LOG(...) ((void)sizeof(::sc::log::Sink(__VA_ARGS__)))
#endif
