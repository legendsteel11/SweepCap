# tools

## make_icon.ps1

`res/sweepcap.ico`를 생성한다. 아이콘은 손으로 그린 것이 아니라 이 스크립트의
출력이므로, 모양을 바꿀 때는 .ico가 아니라 스크립트를 고치고 다시 돌린다.

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/make_icon.ps1 res/sweepcap.ico
```

16, 20, 24, 32, 40, 48, 64, 128, 256px을 32bpp BGRA로 담는다.
24px 미만은 브래킷이 뭉개져서 사각형 외곽선으로 바뀌고, 안티에일리어싱을 끈 채
픽셀에 스냅한다. 트레이가 실제로 쓰는 크기가 여기라서 그렇다.
