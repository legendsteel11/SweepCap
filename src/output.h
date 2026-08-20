#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

struct Bitmap32;

// 캡처 결과를 내보내는 곳. 인코딩은 한 번만 하고 클립보드와 파일이 나눠 쓴다.

// PNG로 인코딩한다. WIC를 쓰므로 호출 스레드가 CoInitialize된 상태여야 한다.
// 실패하면 빈 벡터를 돌려준다.
std::vector<uint8_t> EncodePng(const Bitmap32& bitmap);

// 클립보드에 CF_DIBV5와 등록 포맷 "PNG"를 함께 올린다.
//
// CF_DIB만 넣으면 알파가 날아가고, CF_DIBV5만으로는 받는 앱에 따라 결과가 갈린다.
// CF_DIB와 CF_BITMAP은 셸이 CF_DIBV5에서 자동으로 합성해 준다.
bool CopyToClipboard(HWND owner, const Bitmap32& bitmap, const std::vector<uint8_t>& png);

// Pictures\SweepCap\<날짜>\ 아래에 저장한다.
// 파일명은 CLAUDE.md의 이름 정책을 따른다: 2026-08-18_17-23-33_451.png
// 절대 덮어쓰지 않는다. 겹치면 -2, -3을 붙인다.
// 성공하면 outPath에 실제 경로가 들어간다.
bool SavePng(const std::vector<uint8_t>& png, std::wstring& outPath);

}  // namespace sc
