#ifndef SWEEPCAP_APP_NAME_H
#define SWEEPCAP_APP_NAME_H

// 앱 이름은 이 파일과 리소스 스크립트에만 둔다. 코드명이므로 바뀔 수 있다.
// 이 헤더는 C++와 리소스 컴파일러(rc.exe) 양쪽에서 포함되므로
// 매크로 정의 외에는 아무것도 넣지 않는다.

#define SWEEPCAP_NAME        "SweepCap"
#define SWEEPCAP_NAME_W     L"SweepCap"

#define SWEEPCAP_VERSION     "0.1.0.0"
#define SWEEPCAP_VERSION_W  L"0.1.0.0"
#define SWEEPCAP_VER_COMMA   0,1,0,0

// 창 클래스와 단일 인스턴스 뮤텍스에 쓰는 고유 이름.
#define SWEEPCAP_WNDCLASS_W L"SweepCap.MessageWindow"
#define SWEEPCAP_MUTEX_W    L"Local\\SweepCap.SingleInstance"

#endif  // SWEEPCAP_APP_NAME_H
