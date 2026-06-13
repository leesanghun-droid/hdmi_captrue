# DeckLink Live Preview

Low-latency, **pixel-perfect** live preview for Blackmagic DeckLink capture cards
(tested on the **DeckLink Mini Recorder HD**). Win32 + OpenGL, no MFC.

Captured frames are uploaded straight to an OpenGL texture and drawn 1:1 with
`GL_NEAREST` (no filtering), so on-screen pixels match the captured pixels
exactly. The DeckLink GL "screen preview" helper is intentionally *not* used,
because it applies scaling/filtering that softens text.

## Features

- Lossless 1:1 display (point-sampled, no blur)
- Automatic input format detection (resolution / frame rate / color space)
- RGB sources captured as 8-bit ARGB and converted losslessly to BGRA for display
- Per-monitor DPI awareness (1920x1080 client maps to 1080 physical pixels, no OS stretching)
- Aspect-preserving letterbox when the window isn't 16:9
- Window title shows current mode and live FPS

## Controls

| Key        | Action                          |
| ---------- | ------------------------------- |
| `F` / `F11`| Toggle fullscreen (sharpest 1:1)|
| `ESC`      | Exit (or leave fullscreen)      |

## Requirements

- Windows 10/11 (x64)
- Visual Studio 2022 (C++ "Desktop development" workload) — provides `cl`, `midl`, `vcvars64.bat`
- Blackmagic **Desktop Video** driver installed
- Blackmagic **DeckLink SDK 16.0** (for `DeckLinkAPI.idl`)

## Build

`build.bat` invokes `vcvars64`, runs `midl` to generate the COM headers from the
SDK's `DeckLinkAPI.idl`, then compiles `main.cpp`.

Edit the two paths near the top of `build.bat` to match your machine:

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "SDKINC=C:\path\to\Blackmagic DeckLink SDK 16.0\Win\include"
```

Then run:

```bat
build.bat
```

This produces `DeckLinkPreview.exe`.

## Run

```bat
DeckLinkPreview.exe
```

The app opens the first DeckLink device, enables format detection, and shows the
live input. The window title reports the detected mode and FPS, e.g.
`1080p60  1920x1080  ~60 fps`.

## Notes

- Frame rate is determined by the **source** output. The card supports up to
  1080p60; if the source sends 1080p30, the preview shows 30 fps. Change the
  source's HDMI output to 60 Hz and the preview follows automatically.
- The generated `DeckLinkAPI_h.h` / `DeckLinkAPI_i.c` are derived from the
  proprietary Blackmagic SDK and are not committed; `build.bat` regenerates them.
