// DeckLink pixel-perfect live preview (Win32 + OpenGL, no MFC).
//
// We do NOT use the DeckLink GL screen-preview helper, because that helper is a
// scaled "monitoring" view and applies filtering that softens text. Instead we
// upload each captured frame to a GL texture and draw it 1:1 with GL_NEAREST,
// so the on-screen pixels match the captured pixels exactly (lossless preview).

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <gl/gl.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "DeckLinkAPI_h.h"

// Tokens not present in the Windows 1.1 <gl/gl.h>
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

namespace {

IDeckLinkInput*           g_input = nullptr;
IDeckLinkVideoConversion* g_conv  = nullptr;

HWND   g_hwnd = nullptr;
HDC    g_hdc  = nullptr;
HGLRC  g_glrc = nullptr;
GLuint g_tex  = 0;

bool            g_fullscreen = false;
WINDOWPLACEMENT g_prevPlace  = { sizeof(WINDOWPLACEMENT) };
DWORD           g_prevStyle  = 0;

std::atomic<unsigned long long> g_frameCount{0};
wchar_t g_modeName[256] = L"(detecting...)";
long    g_modeW = 0, g_modeH = 0;

void Log(const char* msg) { printf("%s\n", msg); fflush(stdout); }

// ---------------------------------------------------------------------------
// KVM input forwarding: capture local keyboard/mouse and send them over UDP to
// the RK3568 board, which re-emits them as a USB HID keyboard + absolute mouse
// to the captured ("target") PC. Toggle with [F8]. Wire protocol:
//   keyboard : [0x01, modifiers, k0..k5]                (8 bytes)
//   mouse    : [0x02, buttons, Xlo, Xhi, Ylo, Yhi, wheel] (7 bytes, X/Y 0..32767)
// ---------------------------------------------------------------------------
SOCKET      g_sock = INVALID_SOCKET;
sockaddr_in g_kvmAddr{};
bool        g_capture  = false;
uint8_t     g_mods     = 0;
uint8_t     g_keys[6]  = {0,0,0,0,0,0};
uint8_t     g_btns     = 0;
uint16_t    g_lastX    = 16384;
uint16_t    g_lastY    = 16384;
char        g_kvmTarget[64] = "192.168.0.8";
int         g_kvmPort  = 50000;

bool KvmInit()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    g_kvmAddr.sin_family = AF_INET;
    g_kvmAddr.sin_port   = htons((u_short)g_kvmPort);
    inet_pton(AF_INET, g_kvmTarget, &g_kvmAddr.sin_addr);
    return true;
}

void KvmSend(const uint8_t* data, int len)
{
    if (g_sock == INVALID_SOCKET) return;
    sendto(g_sock, (const char*)data, len, 0, (const sockaddr*)&g_kvmAddr, sizeof(g_kvmAddr));
}

void SendMouse(int8_t wheel)
{
    uint8_t pkt[7] = {
        0x02, g_btns,
        (uint8_t)(g_lastX & 0xff), (uint8_t)(g_lastX >> 8),
        (uint8_t)(g_lastY & 0xff), (uint8_t)(g_lastY >> 8),
        (uint8_t)wheel,
    };
    KvmSend(pkt, sizeof(pkt));
}

void SendKeyboard()
{
    uint8_t pkt[8] = { 0x01, g_mods, g_keys[0], g_keys[1], g_keys[2], g_keys[3], g_keys[4], g_keys[5] };
    KvmSend(pkt, sizeof(pkt));
}

void KeyDown(uint8_t hid)
{
    for (int i = 0; i < 6; ++i) if (g_keys[i] == hid) return;
    for (int i = 0; i < 6; ++i) if (g_keys[i] == 0) { g_keys[i] = hid; return; }
}

void KeyUp(uint8_t hid)
{
    for (int i = 0; i < 6; ++i) if (g_keys[i] == hid) g_keys[i] = 0;
}

// Returns true and sets *bit if vk is a modifier key.
bool ModBit(WPARAM vk, LPARAM lParam, uint8_t* bit)
{
    const bool ext = (lParam & 0x01000000) != 0;
    switch (vk)
    {
    case VK_CONTROL: *bit = ext ? 0x10 : 0x01; return true;            // RCtrl : LCtrl
    case VK_MENU:    *bit = ext ? 0x40 : 0x04; return true;            // RAlt  : LAlt
    case VK_SHIFT:   *bit = (((lParam >> 16) & 0xff) == 0x36) ? 0x20 : 0x02; return true; // RShift : LShift
    case VK_LWIN:    *bit = 0x08; return true;
    case VK_RWIN:    *bit = 0x80; return true;
    }
    return false;
}

// Translate a Windows virtual key to a USB HID usage code (0 = unmapped).
uint8_t VkToHid(WPARAM vk, LPARAM lParam)
{
    const bool ext = (lParam & 0x01000000) != 0;
    if (vk >= 'A' && vk <= 'Z') return (uint8_t)(0x04 + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (uint8_t)(0x1E + (vk - '1'));
    if (vk == '0')              return 0x27;
    if (vk >= VK_F1 && vk <= VK_F12) return (uint8_t)(0x3A + (vk - VK_F1));
    switch (vk)
    {
    case VK_RETURN:    return ext ? 0x58 : 0x28;
    case VK_ESCAPE:    return 0x29;
    case VK_BACK:      return 0x2A;
    case VK_TAB:       return 0x2B;
    case VK_SPACE:     return 0x2C;
    case VK_OEM_MINUS: return 0x2D;
    case VK_OEM_PLUS:  return 0x2E;
    case VK_OEM_4:     return 0x2F; // [
    case VK_OEM_6:     return 0x30; // ]
    case VK_OEM_5:     return 0x31; // backslash
    case VK_OEM_1:     return 0x33; // ;
    case VK_OEM_7:     return 0x34; // '
    case VK_OEM_3:     return 0x35; // `
    case VK_OEM_COMMA: return 0x36;
    case VK_OEM_PERIOD:return 0x37;
    case VK_OEM_2:     return 0x38; // /
    case VK_CAPITAL:   return 0x39;
    case VK_SNAPSHOT:  return 0x46;
    case VK_SCROLL:    return 0x47;
    case VK_PAUSE:     return 0x48;
    case VK_INSERT:    return 0x49;
    case VK_HOME:      return 0x4A;
    case VK_PRIOR:     return 0x4B;
    case VK_DELETE:    return 0x4C;
    case VK_END:       return 0x4D;
    case VK_NEXT:      return 0x4E;
    case VK_RIGHT:     return 0x4F;
    case VK_LEFT:      return 0x50;
    case VK_DOWN:      return 0x51;
    case VK_UP:        return 0x52;
    case VK_NUMLOCK:   return 0x53;
    case VK_DIVIDE:    return 0x54;
    case VK_MULTIPLY:  return 0x55;
    case VK_SUBTRACT:  return 0x56;
    case VK_ADD:       return 0x57;
    case VK_NUMPAD0:   return 0x62;
    case VK_NUMPAD1:   return 0x59;
    case VK_NUMPAD2:   return 0x5A;
    case VK_NUMPAD3:   return 0x5B;
    case VK_NUMPAD4:   return 0x5C;
    case VK_NUMPAD5:   return 0x5D;
    case VK_NUMPAD6:   return 0x5E;
    case VK_NUMPAD7:   return 0x5F;
    case VK_NUMPAD8:   return 0x60;
    case VK_NUMPAD9:   return 0x61;
    case VK_DECIMAL:   return 0x63;
    }
    return 0;
}

void ForwardKey(WPARAM vk, LPARAM lParam, bool down)
{
    uint8_t bit;
    if (ModBit(vk, lParam, &bit))
    {
        if (down) g_mods |= bit; else g_mods &= (uint8_t)~bit;
        SendKeyboard();
        return;
    }
    uint8_t hid = VkToHid(vk, lParam);
    if (!hid) return;
    if (down) KeyDown(hid); else KeyUp(hid);
    SendKeyboard();
}

// Map a client-area point to absolute 0..32767 over the displayed video rect.
bool MapMouse(HWND hwnd, int mx, int my, uint16_t* ox, uint16_t* oy)
{
    RECT rc; GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    const long w = g_modeW, h = g_modeH;
    if (w <= 0 || h <= 0 || cw <= 0 || ch <= 0) return false;
    const double srcA = (double)w / (double)h;
    const double dstA = (double)cw / (double)ch;
    int vw = cw, vh = ch, vx = 0, vy = 0;
    if (dstA > srcA) { vw = (int)(ch * srcA + 0.5); vx = (cw - vw) / 2; }
    else             { vh = (int)(cw / srcA + 0.5); vy = (ch - vh) / 2; }
    if (mx < vx || mx >= vx + vw || my < vy || my >= vy + vh) return false;
    double nx = (double)(mx - vx) / (double)(vw > 1 ? vw - 1 : 1);
    double ny = (double)(my - vy) / (double)(vh > 1 ? vh - 1 : 1);
    if (nx < 0) nx = 0; if (nx > 1) nx = 1;
    if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    *ox = (uint16_t)(nx * 32767.0 + 0.5);
    *oy = (uint16_t)(ny * 32767.0 + 0.5);
    return true;
}

void ReleaseAllInput()
{
    g_mods = 0; g_btns = 0;
    for (int i = 0; i < 6; ++i) g_keys[i] = 0;
    SendKeyboard();
    SendMouse(0);
}

bool g_tracking = false;   // WM_MOUSELEAVE armed?

void ArmLeaveTracking(HWND hwnd)
{
    if (g_tracking) return;
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
    if (TrackMouseEvent(&tme)) g_tracking = true;
}

void BeginCapture(HWND hwnd)
{
    if (g_capture) return;
    g_capture = true;
    ReleaseAllInput();
    ArmLeaveTracking(hwnd);
    SetCursor(nullptr);
    printf("KVM capture: ON\n"); fflush(stdout);
}

void EndCapture(HWND hwnd)
{
    if (!g_capture) return;
    g_capture = false;
    ReleaseAllInput();
    if (GetCapture() == hwnd) ReleaseCapture();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    printf("KVM capture: off\n"); fflush(stdout);
}

// Convert (if needed) to BGRA and draw 1:1 with point sampling.
void RenderFrame(IDeckLinkVideoFrame* src)
{
    if (!src || !g_hdc || !g_glrc) return;

    IDeckLinkVideoFrame* bgra = nullptr;
    bool ownBgra = false;
    if (src->GetPixelFormat() == bmdFormat8BitBGRA)
    {
        bgra = src;
    }
    else
    {
        if (!g_conv) return;
        if (g_conv->ConvertNewFrame(src, bmdFormat8BitBGRA, bmdColorspaceUnknown, nullptr, &bgra) != S_OK)
            return;
        ownBgra = true;
    }

    const long w  = bgra->GetWidth();
    const long h  = bgra->GetHeight();
    const long rb = bgra->GetRowBytes();

    IDeckLinkVideoBuffer* buf = nullptr;
    if (bgra->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&buf) == S_OK && buf)
    {
        if (buf->StartAccess(bmdBufferAccessRead) == S_OK)
        {
            void* bytes = nullptr;
            if (buf->GetBytes(&bytes) == S_OK && bytes && wglMakeCurrent(g_hdc, g_glrc))
            {
                if (!g_tex) glGenTextures(1, &g_tex);
                glBindTexture(GL_TEXTURE_2D, g_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rb / 4);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, bytes);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

                RECT rc; GetClientRect(g_hwnd, &rc);
                const int cw = rc.right - rc.left;
                const int ch = rc.bottom - rc.top;

                glViewport(0, 0, cw, ch);
                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);

                // Aspect-preserving, centred viewport (letterbox). With a window
                // sized 1:1 to the source this fills exactly -> pixel perfect.
                int vx = 0, vy = 0, vw = cw, vh = ch;
                if (w > 0 && h > 0 && cw > 0 && ch > 0)
                {
                    const double srcA = (double)w / (double)h;
                    const double dstA = (double)cw / (double)ch;
                    if (dstA > srcA) { vw = (int)(ch * srcA + 0.5); vx = (cw - vw) / 2; }
                    else             { vh = (int)(cw / srcA + 0.5); vy = (ch - vh) / 2; }
                }
                glViewport(vx, vy, vw, vh);

                glEnable(GL_TEXTURE_2D);
                glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 1, 0, 1, -1, 1);
                glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
                glColor3f(1.f, 1.f, 1.f);
                glBegin(GL_QUADS);
                    glTexCoord2f(0.f, 0.f); glVertex2f(0.f, 1.f); // top-left
                    glTexCoord2f(1.f, 0.f); glVertex2f(1.f, 1.f); // top-right
                    glTexCoord2f(1.f, 1.f); glVertex2f(1.f, 0.f); // bottom-right
                    glTexCoord2f(0.f, 1.f); glVertex2f(0.f, 0.f); // bottom-left
                glEnd();
                glDisable(GL_TEXTURE_2D);

                SwapBuffers(g_hdc);
                wglMakeCurrent(nullptr, nullptr);
            }
            buf->EndAccess(bmdBufferAccessRead);
        }
        buf->Release();
    }

    if (ownBgra && bgra) bgra->Release();
}

class InputCallback : public IDeckLinkInputCallback
{
    std::atomic<ULONG> m_ref{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDeckLinkInputCallback)
            *ppv = static_cast<IDeckLinkInputCallback*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override  { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG v = --m_ref; if (v == 0) delete this; return v; }

    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents /*events*/,
                                                      IDeckLinkDisplayMode* newMode,
                                                      BMDDetectedVideoInputFormatFlags flags) override
    {
        BMDPixelFormat pf = bmdFormat8BitYUV;
        const bool isRGB = (flags & bmdDetectedVideoInputRGB444) != 0;
        if (isRGB)
        {
            if      (flags & bmdDetectedVideoInput8BitDepth)  pf = bmdFormat8BitARGB;
            else if (flags & bmdDetectedVideoInput10BitDepth) pf = bmdFormat10BitRGB;
            else if (flags & bmdDetectedVideoInput12BitDepth) pf = bmdFormat12BitRGB;
        }
        else
        {
            pf = (flags & bmdDetectedVideoInput10BitDepth) ? bmdFormat10BitYUV : bmdFormat8BitYUV;
        }

        g_input->StopStreams();
        g_input->EnableVideoInput(newMode->GetDisplayMode(), pf, bmdVideoInputEnableFormatDetection);
        g_input->StartStreams();

        BSTR name = nullptr;
        if (newMode->GetName(&name) == S_OK && name)
        {
            wcsncpy_s(g_modeName, name, _TRUNCATE);
            SysFreeString(name);
        }
        g_modeW = newMode->GetWidth();
        g_modeH = newMode->GetHeight();

        const char* depthDesc = (flags & bmdDetectedVideoInput12BitDepth) ? "12-bit"
                              : (flags & bmdDetectedVideoInput10BitDepth) ? "10-bit" : "8-bit";
        wprintf(L"[format detected] %ls  (%ld x %ld)\n", g_modeName, g_modeW, g_modeH);
        printf("                  color = %s %s\n", depthDesc, isRGB ? "RGB 4:4:4" : "YCbCr 4:2:2");
        fflush(stdout);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                                     IDeckLinkAudioInputPacket* /*audio*/) override
    {
        if (videoFrame)
        {
            g_frameCount++;
            RenderFrame(videoFrame);
        }
        return S_OK;
    }
};

bool SetupOpenGL(HWND hwnd)
{
    g_hdc = GetDC(hwnd);
    if (!g_hdc) return false;

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (pf == 0 || !SetPixelFormat(g_hdc, pf, &pfd)) return false;

    g_glrc = wglCreateContext(g_hdc);
    return g_glrc != nullptr;
}

void ToggleFullscreen(HWND hwnd)
{
    if (!g_fullscreen)
    {
        g_prevStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowPlacement(hwnd, &g_prevPlace);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLong(hwnd, GWL_STYLE, g_prevStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = true;
    }
    else
    {
        SetWindowLong(hwnd, GWL_STYLE, g_prevStyle);
        SetWindowPlacement(hwnd, &g_prevPlace);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        g_fullscreen = false;
    }
}

// Restore a windowed, pixel-perfect 1:1 client area matching the source size.
void SetOriginalSize(HWND hwnd)
{
    if (g_fullscreen) ToggleFullscreen(hwnd);
    const int w = (g_modeW > 0) ? (int)g_modeW : 1920;
    const int h = (g_modeH > 0) ? (int)g_modeH : 1080;
    UINT dpi = GetDpiForWindow(hwnd);
    RECT cr{ 0, 0, w, h };
    AdjustWindowRectExForDpi(&cr, (DWORD)GetWindowLong(hwnd, GWL_STYLE), FALSE, 0, dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

// Single toggle: fullscreen <-> original 1:1 windowed.
void ToggleScreen(HWND hwnd)
{
    if (g_fullscreen) SetOriginalSize(hwnd);
    else              ToggleFullscreen(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
    {
        static unsigned long long last = 0;
        unsigned long long now = g_frameCount.load();
        unsigned long long fps = now - last;
        last = now;
        wchar_t title[384];
        swprintf_s(title, L"DeckLink Live Preview  -  %ls  %ldx%ld  ~%llu fps   [KVM %ls -> %hs]   %ls",
                   g_modeName, g_modeW, g_modeH, fps,
                   g_capture ? L"CONTROLLING" : L"off", g_kvmTarget,
                   g_capture ? L"(leave window to release)" : L"(double-click to control)");
        SetWindowTextW(hwnd, title);
        return 0;
    }
    case WM_SETCURSOR:
        // Hide the local cursor over the video while remote-controlling.
        if (g_capture && LOWORD(lParam) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_MOUSEMOVE:
        if (g_capture)
        {
            ArmLeaveTracking(hwnd);   // keep WM_MOUSELEAVE armed
            uint16_t x, y;
            if (MapMouse(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &x, &y))
            {
                g_lastX = x; g_lastY = y;
                SendMouse(0);
            }
            return 0;
        }
        return 0;
    case WM_MOUSELEAVE:
        g_tracking = false;
        EndCapture(hwnd);    // leaving the window releases control immediately
        return 0;
    case WM_LBUTTONDBLCLK:
        // Double-click inside the preview enters remote-control mode.
        if (!g_capture) { BeginCapture(hwnd); return 0; }
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    {
        const int mx = GET_X_LPARAM(lParam);
        const int my = GET_Y_LPARAM(lParam);
        if (g_capture)
        {
            uint16_t x, y;
            if (MapMouse(hwnd, mx, my, &x, &y)) { g_lastX = x; g_lastY = y; }
            switch (msg)
            {
            case WM_LBUTTONDOWN: g_btns |= 0x01; SetCapture(hwnd); break;
            case WM_LBUTTONUP:   g_btns &= ~0x01; break;
            case WM_RBUTTONDOWN: g_btns |= 0x02; SetCapture(hwnd); break;
            case WM_RBUTTONUP:   g_btns &= ~0x02; break;
            case WM_MBUTTONDOWN: g_btns |= 0x04; SetCapture(hwnd); break;
            case WM_MBUTTONUP:   g_btns &= ~0x04; break;
            }
            if (g_btns == 0 && GetCapture() == hwnd) ReleaseCapture();
            SendMouse(0);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (g_capture)
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            if (delta >  127) delta =  127;
            if (delta < -127) delta = -127;
            SendMouse((int8_t)delta);
            return 0;
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (g_capture) { ForwardKey(wParam, lParam, true); return 0; }
        if (wParam == VK_ESCAPE)
        {
            if (g_fullscreen) ToggleFullscreen(hwnd);
            else              PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        else if (wParam == 'F' || wParam == VK_F11)
        {
            ToggleScreen(hwnd);
        }
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (g_capture) { ForwardKey(wParam, lParam, false); return 0; }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int main(int argc, char** argv)
{
    // Optional args: KVM target IP and UDP port (defaults: 192.168.0.8 50000).
    if (argc >= 2) { strncpy_s(g_kvmTarget, argv[1], _TRUNCATE); }
    if (argc >= 3) { g_kvmPort = atoi(argv[2]); }
    if (KvmInit()) printf("KVM: forwarding input to %s:%d (double-click window to control, leave to release)\n", g_kvmTarget, g_kvmPort);
    else           Log("KVM: UDP socket init failed (input forwarding disabled)");

    // Per-monitor DPI awareness: 1920x1080 client maps 1:1 to physical pixels on
    // a 4K display (no OS bitmap stretching).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { Log("CoInitializeEx failed"); return 1; }

    int exitCode = 1;
    IDeckLinkIterator*          iterator = nullptr;
    IDeckLink*                  deckLink = nullptr;
    IDeckLinkProfileAttributes* attrs    = nullptr;
    InputCallback*              cb        = nullptr;
    bool                        supportsDetection = false;

    if (CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                         IID_IDeckLinkIterator, (void**)&iterator) != S_OK || !iterator)
    {
        Log("Could not create DeckLink iterator. Is Desktop Video installed?");
        goto cleanup;
    }
    if (iterator->Next(&deckLink) != S_OK || !deckLink)
    {
        Log("No DeckLink devices found.");
        goto cleanup;
    }
    {
        BSTR name = nullptr;
        if (deckLink->GetDisplayName(&name) == S_OK && name)
        {
            wprintf(L"Device: %ls\n", name);
            SysFreeString(name);
        }
    }
    if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&g_input) != S_OK || !g_input)
    {
        Log("Selected device has no input interface.");
        goto cleanup;
    }
    if (deckLink->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attrs) == S_OK && attrs)
    {
        BOOL fd = FALSE;
        if (attrs->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &fd) == S_OK)
            supportsDetection = (fd != FALSE);
    }
    Log(supportsDetection ? "Input format auto-detection: supported"
                          : "Input format auto-detection: NOT supported");

    if (CoCreateInstance(CLSID_CDeckLinkVideoConversion, nullptr, CLSCTX_ALL,
                         IID_IDeckLinkVideoConversion, (void**)&g_conv) != S_OK || !g_conv)
        Log("Warning: could not create video converter (non-BGRA sources may not render)");

    // Create preview window
    {
        WNDCLASSEX wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;   // receive WM_LBUTTONDBLCLK
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"DeckLinkPreviewWnd";
        RegisterClassEx(&wc);

        RECT r{0, 0, 1920, 1080};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_hwnd = CreateWindowEx(0, wc.lpszClassName, L"DeckLink Live Preview",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_hwnd) { Log("CreateWindow failed"); goto cleanup; }

        // Force client area to exactly 1920x1080 physical pixels for this DPI.
        {
            UINT dpi = GetDpiForWindow(g_hwnd);
            RECT cr{0, 0, 1920, 1080};
            AdjustWindowRectExForDpi(&cr, (DWORD)GetWindowLong(g_hwnd, GWL_STYLE), FALSE, 0, dpi);
            SetWindowPos(g_hwnd, nullptr, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
    }

    if (!SetupOpenGL(g_hwnd)) { Log("OpenGL setup failed"); goto cleanup; }

    cb = new InputCallback();
    g_input->SetCallback(cb);
    {
        BMDVideoInputFlags flags = supportsDetection ? bmdVideoInputEnableFormatDetection
                                                      : bmdVideoInputFlagDefault;
        if (g_input->EnableVideoInput(bmdModeHD1080p6000, bmdFormat8BitYUV, flags) != S_OK)
        { Log("EnableVideoInput failed"); goto cleanup; }
        if (g_input->StartStreams() != S_OK)
        { Log("StartStreams failed"); goto cleanup; }
    }

    Log("Live preview started (pixel-perfect 1:1).  [F]/[F11] = fullscreen   [F8] = KVM capture   [ESC] = exit");
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, 1, 1000, nullptr);

    {
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    exitCode = 0;

cleanup:
    if (g_input)
    {
        g_input->StopStreams();
        g_input->SetCallback(nullptr);
        g_input->DisableVideoInput();
    }
    if (cb)       cb->Release();
    if (g_conv)   g_conv->Release();
    if (g_glrc)   wglDeleteContext(g_glrc);
    if (g_hdc && g_hwnd) ReleaseDC(g_hwnd, g_hdc);
    if (attrs)    attrs->Release();
    if (g_input)  g_input->Release();
    if (deckLink) deckLink->Release();
    if (iterator) iterator->Release();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
    CoUninitialize();
    return exitCode;
}
