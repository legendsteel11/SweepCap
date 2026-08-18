#pragma once

#include <windows.h>

namespace sc {

// 앱 수명 전체. 트레이 상주 창을 만들고 메시지 루프를 돈다.
// 반환값이 그대로 프로세스 종료 코드가 된다.
int Run(HINSTANCE instance);

}  // namespace sc
