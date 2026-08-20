#ifndef SWEEPCAP_APP_NAME_H
#define SWEEPCAP_APP_NAME_H

// The application name lives here and in the resource script, nowhere else.
// It is a code name and may change.
//
// This header is included by both C++ and the resource compiler, so it must
// contain macro definitions only.

#define SWEEPCAP_NAME        "SweepCap"
#define SWEEPCAP_NAME_W     L"SweepCap"

#define SWEEPCAP_VERSION     "0.1.0.0"
#define SWEEPCAP_VERSION_W  L"0.1.0.0"
#define SWEEPCAP_VER_COMMA   0,1,0,0

// Unique names for the window class and the single-instance mutex.
#define SWEEPCAP_WNDCLASS_W L"SweepCap.MessageWindow"
#define SWEEPCAP_MUTEX_W    L"Local\\SweepCap.SingleInstance"

#endif  // SWEEPCAP_APP_NAME_H
