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

Right-click the title bar for options (KVM target IP, ping status, etc.).

## KVM control (keyboard / video / mouse over IP)

The preview can also drive the captured machine ("target") as a KVM. A small
USB-gadget board (RK3568 / Firefly) plugged into the target presents itself as a
USB keyboard + mouse + mass-storage device. The preview app sends keyboard and
mouse events as UDP packets to a HID daemon (`board/kvm_hid.py`) on the board,
which replays them onto the target.

- **Enter control:** double-click the preview. The cursor is hidden and all
  input is forwarded to the target.
- **Leave control:** move the mouse out of the window (when not dragging).
- Mouse uses absolute positioning pinned to a 1920x1080 target; double-clicks
  are forwarded.

## File transfer (both directions, drag-and-drop)

A FAT32 disk image on the board is exposed to the target as a removable USB
drive (`KVMSHARE`). A tiny resident **agent** on the target
(`agent/agent.ps1`, installed once via `agent/install.bat`) moves files between
that drive and the desktop silently — no console windows, Run dialogs, or
folder pop-ups.

### PC1 → Target (drop onto the preview)

Drag files from Explorer **onto the preview window**. They are staged into the
USB drive's `_drop` folder; the target agent copies them onto the target's
Desktop. The title bar shows transfer progress.

Flow: `main.cpp` (`WM_DROPFILES`) → `kvm-drop.ps1` → `scp` to board inbox →
`board/kvm-drop.sh` (→ `D:\_drop`) → target agent → target Desktop.

### Target → PC1 (drag a file off the screen)

While in KVM control, **grab a file on the target, drag it past the edge of the
preview, and release outside the video.** That gesture transfers the dragged
file to PC1's Desktop. Because PC1 detects the gesture itself, there is no idle
polling — the USB drive blips exactly once, only when you actually do it.

Flow: `main.cpp` detects "button-up outside the video while dragging" →
`GrabWorker` sends **ESC** (cancels the target drag so the file stays selected),
releases the button, then a dedicated grab hotkey (`Ctrl+Alt+Shift+F9`) → the
target agent captures the selected file (Explorer selection via Shell COM, or
clipboard `Ctrl+C` fallback for the Desktop) and writes it to `_pull` →
`kvm-grab.ps1` pulls it (`board/kvm-share.sh reverse` → `revout` → `scp`) onto
PC1's Desktop.

## Components

| Path                  | Runs on | Purpose                                                        |
| --------------------- | ------- | -------------------------------------------------------------- |
| `main.cpp`            | PC1     | Preview + KVM input + drop/drag-out gesture detection          |
| `kvm-drop.ps1`        | PC1     | Stage dropped files onto the board, trigger the target agent   |
| `kvm-grab.ps1`        | PC1     | Reverse pull: fetch the file the target agent staged           |
| `kvm-push/pull/sync.ps1` | PC1  | Manual helpers for the shared USB drive                        |
| `board/kvm_hid.py`    | board   | USB HID daemon (replays KVM input) + mass-storage gadget       |
| `board/kvm-share.sh`  | board   | Mount/sync the disk image; `reverse` stages `_pull` → `revout` |
| `board/kvm-drop.sh`   | board   | Stage inbox files into the drive's `_drop` (size/space checks) |
| `board/deploy.sh`     | board   | One-time board setup (gadget, 8 GB FAT32 image, service)       |
| `agent/agent.ps1`     | target  | Silent resident agent: `_drop` → Desktop, grab hotkey → `_pull`|
| `agent/install.bat`   | target  | Installs the agent to `%APPDATA%`, autostart at logon          |

The board scripts always re-arm the USB gadget on exit (`trap cleanup EXIT`), so
a failed transfer never leaves KVM dead.

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
