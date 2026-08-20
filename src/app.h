#pragma once

#include <windows.h>

namespace sc {

// The whole application lifetime: create the tray-resident window and run the
// message loop. The return value becomes the process exit code.
int Run(HINSTANCE instance);

}  // namespace sc
