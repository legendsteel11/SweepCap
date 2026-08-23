# SweepCap

[한국어](README-ko.md)

A Windows capture utility where the capture is over the moment you finish a
drag with the modifier held. There is no pressing a shortcut, waiting for a
tool to appear, then selecting a region. The drag is the capture.

## Download

Get the latest version from the
[releases page](https://github.com/legendsteel11/SweepCap/releases/latest).
It is a single file, `SweepCap.exe`.

- No installation. Put it anywhere and run it; it lives in the tray.
- No runtime to install.
- No administrator rights required.
- Runs on Windows 11 and later.

## Usage

The default modifier is `Ctrl+Win`, changeable from the tray menu.

| Gesture | Action |
|---|---|
| `Ctrl+Win` + drag | Capture a region |
| `Ctrl+Win` + click | Capture the window under the cursor |
| `Ctrl+Win` + click on the desktop | Capture that whole monitor |
| `Shift` during a drag | Snap the selection to a grid |
| Right click, `ESC` | Cancel |
| `Ctrl+Win` while resizing a window | Snap the window size to a grid on release |

Every capture goes to the clipboard and to a PNG file at the same time. Files
land under `Pictures\SweepCap\<date>\` as `AppName_date_time.png`, and
double-clicking the tray icon opens that folder. The name of the application
you captured is in the file name, which is what makes a capture findable
later.

## The tray menu

Every setting lives in the tray icon's right-click menu.

- **Open capture folder**: opens the folder your captures are saved in.
- **Change shortcut**: picks the shortcut combination. Over RDP, pick a
  combination without the `Win` key.
- **Capture grid**: the grid pitch for grid-snapped captures.
- **Window grid**: the grid pitch for snapped window resizing.
- **Dim outside**: how dark the screen outside the selection gets.
- **Save folder**: changes where captures are saved. The option to keep
  everything in one folder without date subfolders is here too.
- **Run at startup**: starts with Windows.
- **About**: version and author information.
- **Restart**: restarts the application.
- **Exit**: quits the application.

## Why it works this way

Conventional capture tools go through "press a key, a tool appears, select a
region". A keyboard shortcut system cannot see the mouse, so the key press
and the drag are forced apart, and that is where the mode comes from.

SweepCap takes mouse input directly and removes that separation. The moment
the button goes down with the modifier held, the screen is frozen, and the
crop is taken from that frozen frame. That is why it is fast, and why the
selection UI can never appear in the result.

## Features

- Region, window and whole-monitor capture are one gesture: a drag selects a
  region, a click takes a window.
- Window captures keep the Windows 11 rounded corners, with transparency.
- Grid snapping for both captures and window resizing, with the pitch chosen
  in pixels or as screen divisions.
- The clipboard carries both PNG and DIBV5, so alpha survives regardless of
  which application you paste into.
- Multiple monitors and per-monitor scaling (Per-Monitor V2 DPI) are
  supported.
- The UI ships in Korean and English and follows the Windows display
  language.
- Every setting lives in the tray menu: the modifier, the grids, the save
  folder, date subfolders, and run at startup.

## Building

Visual Studio 2022 (Desktop development with C++) and the Windows 11 SDK.
There are no external package dependencies; cloning the repository is enough.

```
build.bat release
```

The result is `build\release\SweepCap.exe`.

## Requests and bug reports

Send them to pjh85336@gmail.com.

## Other tools by the same maker

- [TabStick](https://tabstick.com/), sticky index notes that attach to
  windows.
- [Edgetree](https://github.com/legendsteel11/Edgetree), a VS Code
  explorer-style file browser docked to the screen edge.

## License

MIT, see [LICENSE.md](LICENSE.md). Includes Microsoft's
[WIL](https://github.com/microsoft/wil) (MIT License).

## About the development

This tool was designed and refined by its maker, with the implementation done
in collaboration with Claude Code (Anthropic). Feature decisions, UX design
and testing all come from daily real use.
