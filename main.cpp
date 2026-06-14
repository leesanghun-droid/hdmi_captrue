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
#include <shellapi.h>
#include <string>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <gl/gl.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "iphlpapi.lib")

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

// Transient status shown in the title bar (e.g. drag-and-drop file transfer).
wchar_t   g_drop[200]  = L"";
ULONGLONG g_dropExpire = 0;

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

// Guards g_kvmTarget / g_kvmPort / g_kvmAddr (touched by the UI and ping threads).
CRITICAL_SECTION  g_kvmLock;
// Latest ping result to the KVM target: -2 = unknown/measuring, -1 = no reply,
// >=0 = round-trip latency in ms.
std::atomic<int>  g_pingMs{-2};
HANDLE            g_pingThread = nullptr;
std::atomic<bool> g_pingRun{false};

// Recompute the destination sockaddr from the current target IP / port.
void ApplyKvmTarget()
{
    EnterCriticalSection(&g_kvmLock);
    g_kvmAddr.sin_family = AF_INET;
    g_kvmAddr.sin_port   = htons((u_short)g_kvmPort);
    inet_pton(AF_INET, g_kvmTarget, &g_kvmAddr.sin_addr);
    LeaveCriticalSection(&g_kvmLock);
    g_pingMs.store(-2);   // force a fresh measurement against the new target
}

bool KvmInit()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    ApplyKvmTarget();
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
    // Mouse mapping is pinned to a 1920x1080 (16:9) target so absolute
    // positioning stays correct regardless of the detected capture mode.
    const long w = 1920, h = 1080;
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

// ---------------------------------------------------------------------------
// KVM target options: right-click the title bar to edit the IP / port and to
// watch a live ping (reachability) indicator.
// ---------------------------------------------------------------------------

// Background worker: pings the current KVM target about once a second and
// publishes the round-trip latency in g_pingMs (-2 = measuring, -1 = no reply).
DWORD WINAPI PingThreadProc(LPVOID)
{
    HANDLE icmp = IcmpCreateFile();
    while (g_pingRun.load())
    {
        char ip[64];
        EnterCriticalSection(&g_kvmLock);
        strncpy_s(ip, g_kvmTarget, _TRUNCATE);
        LeaveCriticalSection(&g_kvmLock);

        IN_ADDR dst{};
        if (icmp != INVALID_HANDLE_VALUE && inet_pton(AF_INET, ip, &dst) == 1)
        {
            char  payload[32] = "decklink-kvm-ping";
            char  reply[sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8];
            DWORD n = IcmpSendEcho(icmp, dst.S_un.S_addr, payload, sizeof(payload),
                                   nullptr, reply, sizeof(reply), 800);
            if (n > 0)
            {
                ICMP_ECHO_REPLY* er = reinterpret_cast<ICMP_ECHO_REPLY*>(reply);
                g_pingMs.store(er->Status == IP_SUCCESS ? (int)er->RoundTripTime : -1);
            }
            else g_pingMs.store(-1);
        }
        else g_pingMs.store(-1);

        for (int i = 0; i < 10 && g_pingRun.load(); ++i) Sleep(100);
    }
    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
    return 0;
}

// Build a short, human-readable ping status string and its display colour.
void PingStatus(wchar_t* buf, size_t n, COLORREF* col)
{
    int ms = g_pingMs.load();
    if (ms == -2)      { wcscpy_s(buf, n, L"\uCE21\uC815 \uC911...");                 if (col) *col = RGB(110,110,110); } // 측정 중...
    else if (ms < 0)   { wcscpy_s(buf, n, L"\uC751\uB2F5 \uC5C6\uC74C (timeout)");      if (col) *col = RGB(200, 40, 40); } // 응답 없음 (timeout)
    else               { swprintf_s(buf, n, L"\uC815\uC0C1  (%d ms)", ms);              if (col) *col = RGB(20,140, 40); } // 정상 (n ms)
}

namespace optdlg {
    enum { IDC_IP = 101, IDC_PORT = 102, IDC_PINGTXT = 103, IDB_PINGNOW = 106 };
    bool  g_done = false;
    HFONT g_font = nullptr;

    // Read + validate the edit fields, then apply them as the new KVM target.
    bool Apply(HWND dlg)
    {
        char ip[64] = {0};
        GetDlgItemTextA(dlg, IDC_IP, ip, sizeof(ip));
        IN_ADDR t{};
        if (inet_pton(AF_INET, ip, &t) != 1)
        {
            MessageBoxW(dlg, L"IP \uC8FC\uC18C \uD615\uC2DD\uC774 \uC62C\uBC14\uB974\uC9C0 \uC54A\uC2B5\uB2C8\uB2E4.",
                        L"KVM \uC124\uC815", MB_ICONWARNING | MB_OK);
            return false;
        }
        char ps[16] = {0};
        GetDlgItemTextA(dlg, IDC_PORT, ps, sizeof(ps));
        int port = atoi(ps);
        if (port < 1 || port > 65535)
        {
            MessageBoxW(dlg, L"\uD3EC\uD2B8\uB294 1 ~ 65535 \uBC94\uC704\uC5EC\uC57C \uD569\uB2C8\uB2E4.",
                        L"KVM \uC124\uC815", MB_ICONWARNING | MB_OK);
            return false;
        }
        EnterCriticalSection(&g_kvmLock);
        strncpy_s(g_kvmTarget, ip, _TRUNCATE);
        g_kvmPort = port;
        LeaveCriticalSection(&g_kvmLock);
        ApplyKvmTarget();
        return true;
    }

    LRESULT CALLBACK Proc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            UINT dpi = GetDpiForWindow(dlg);
            auto S = [dpi](int v){ return MulDiv(v, dpi, 96); };
            g_font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HINSTANCE hi = ((LPCREATESTRUCT)lParam)->hInstance;
            struct { const wchar_t* cls; const wchar_t* txt; DWORD style; int id; int x,y,w,h; } ctl[] = {
                { L"STATIC", L"\uB300\uC0C1 IP:",       WS_VISIBLE|WS_CHILD|SS_LEFT,                 0,           15, 18, 70, 20 },
                { L"EDIT",   L"",                        WS_VISIBLE|WS_CHILD|WS_BORDER|ES_AUTOHSCROLL, IDC_IP,    90, 15, 235, 24 },
                { L"STATIC", L"\uD3EC\uD2B8:",           WS_VISIBLE|WS_CHILD|SS_LEFT,                 0,           15, 52, 70, 20 },
                { L"EDIT",   L"",                        WS_VISIBLE|WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER, IDC_PORT, 90, 49, 90, 24 },
                { L"STATIC", L"Ping \uC0C1\uD0DC:",      WS_VISIBLE|WS_CHILD|SS_LEFT,                 0,           15, 88, 70, 20 },
                { L"STATIC", L"\uCE21\uC815 \uC911...",  WS_VISIBLE|WS_CHILD|SS_LEFT,                 IDC_PINGTXT, 90, 88, 235, 20 },
                { L"BUTTON", L"\uC9C0\uAE08 \uD655\uC778", WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,         IDB_PINGNOW, 90, 116, 110, 28 },
                { L"BUTTON", L"\uC800\uC7A5",            WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON,        IDOK,       135, 162, 90, 30 },
                { L"BUTTON", L"\uB2EB\uAE30",            WS_VISIBLE|WS_CHILD|BS_PUSHBUTTON,           IDCANCEL,   235, 162, 90, 30 },
            };
            for (auto& c : ctl)
            {
                HWND h = CreateWindowExW(0, c.cls, c.txt, c.style,
                                         S(c.x), S(c.y), S(c.w), S(c.h),
                                         dlg, (HMENU)(intptr_t)c.id, hi, nullptr);
                SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
            }
            char portbuf[16];
            _itoa_s(g_kvmPort, portbuf, 10);
            SetDlgItemTextA(dlg, IDC_IP, g_kvmTarget);
            SetDlgItemTextA(dlg, IDC_PORT, portbuf);
            SetTimer(dlg, 1, 400, nullptr);
            return 0;
        }
        case WM_TIMER:
        {
            wchar_t s[64]; PingStatus(s, 64, nullptr);
            SetDlgItemTextW(dlg, IDC_PINGTXT, s);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            if ((HWND)lParam == GetDlgItem(dlg, IDC_PINGTXT))
            {
                COLORREF col; wchar_t s[64]; PingStatus(s, 64, &col);
                SetBkMode((HDC)wParam, TRANSPARENT);
                SetTextColor((HDC)wParam, col);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case IDB_PINGNOW: Apply(dlg); return 0;                 // re-target + measure now
            case IDOK:        if (Apply(dlg)) g_done = true; return 0;
            case IDCANCEL:    g_done = true; return 0;
            }
            break;
        case WM_CLOSE:
            g_done = true;
            return 0;
        case WM_DESTROY:
            KillTimer(dlg, 1);
            if (g_font) { DeleteObject(g_font); g_font = nullptr; }
            return 0;
        }
        return DefWindowProcW(dlg, msg, wParam, lParam);
    }
} // namespace optdlg

// Modal KVM options dialog (programmatic, no .rc resource needed).
void ShowKvmOptions(HWND owner)
{
    static bool registered = false;
    HINSTANCE hi = GetModuleHandle(nullptr);
    if (!registered)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = optdlg::Proc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DeckLinkKvmOptions";
        RegisterClassW(&wc);
        registered = true;
    }

    UINT dpi = GetDpiForWindow(owner);
    int W = MulDiv(360, dpi, 96), H = MulDiv(250, dpi, 96);
    RECT o; GetWindowRect(owner, &o);
    int x = o.left + ((o.right - o.left) - W) / 2;
    int y = o.top  + ((o.bottom - o.top) - H) / 2;

    optdlg::g_done = false;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DeckLinkKvmOptions",
                               L"KVM \uC124\uC815",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, W, H, owner, nullptr, hi, nullptr);
    if (!dlg) return;

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG m;
    while (!optdlg::g_done)
    {
        BOOL r = GetMessage(&m, nullptr, 0, 0);
        if (r == 0) { PostQuitMessage((int)m.wParam); break; }  // re-post WM_QUIT to outer loop
        if (r == -1) break;
        if (!IsDialogMessage(dlg, &m)) { TranslateMessage(&m); DispatchMessage(&m); }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (IsWindow(dlg)) DestroyWindow(dlg);
}

// Title-bar right-click menu command IDs.
enum { IDM_OPTIONS = 201, IDM_FULLSCREEN = 202, IDM_EXIT = 203 };

// Runs kvm-drop.ps1 (passed as a full command line) and waits for it, updating
// the title-bar status. Takes ownership of the heap-allocated command line.
DWORD WINAPI DropWorker(LPVOID param)
{
    wchar_t* cmdline = (wchar_t*)param;
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 600000);   // large files can take a while
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code == 0) wcscpy_s(g_drop, L"\uD0C0\uAC9F \uBC14\uD0D5\uD654\uBA74 \uC804\uC1A1 \uC644\uB8CC");        // "sent to target Desktop"
        else           wcscpy_s(g_drop, L"\uC804\uC1A1 \uC2E4\uD328 (\uC6A9\uB7C9 \uCD08\uACFC?)");  // "send failed (too big?)"
    }
    else
    {
        wcscpy_s(g_drop, L"\uC804\uC1A1 \uC2E4\uD328");  // "send failed"
    }
    g_dropExpire = GetTickCount64() + 6000;
    free(cmdline);
    return 0;
}

// Reverse drag-out: tell the target agent to stage the dragged file, then pull
// it to PC1's Desktop. Runs on a worker thread so it can pace the HID sequence.
DWORD WINAPI GrabWorker(LPVOID)
{
    const uint8_t kbUp[8]   = {0x01,0,0,0,0,0,0,0};
    const uint8_t escDown[8]= {0x01,0,0x29,0,0,0,0,0};                 // ESC
    const uint8_t mUp[7]    = {0x02,0x00,0x00,0x40,0x00,0x40,0x00};    // all buttons up
    const uint8_t hkDown[8] = {0x01,0x07,0x42,0,0,0,0,0};             // Ctrl+Alt+Shift+F9

    Sleep(50);
    KvmSend(escDown, 8); Sleep(40); KvmSend(kbUp, 8); Sleep(150);     // cancel the drag
    KvmSend(mUp, 7);     Sleep(60);                                   // release the button
    KvmSend(hkDown, 8);  Sleep(60); KvmSend(kbUp, 8); Sleep(700);     // signal the agent

    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(exeDir, L'\\')) *s = 0;
    std::wstring cmd = L"powershell.exe -ExecutionPolicy Bypass -NoProfile -WindowStyle Hidden -File \"";
    cmd += exeDir; cmd += L"\\kvm-grab.ps1\"";
    wchar_t* cl = _wcsdup(cmd.c_str());
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cl, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (code == 0) wcscpy_s(g_drop, L"\uD0C0\uAC9F\uC5D0\uC11C \uAC00\uC838\uC634 -> \uBC14\uD0D5\uD654\uBA74");
        else           wcscpy_s(g_drop, L"\uAC00\uC838\uC624\uAE30 \uC2E4\uD328 (\uC120\uD0DD\uB41C \ud30c\uc77c \uc5c6\uc74c?)");
    }
    else wcscpy_s(g_drop, L"\uAC00\uC838\uC624\uAE30 \uC2E4\uD328");
    g_dropExpire = GetTickCount64() + 6000;
    free(cl);
    return 0;
}

void StartReverseGrab(HWND hwnd)
{
    // Stop controlling locally WITHOUT sending releases (the worker sends ESC +
    // button-up itself, in the right order, so the target drag is cancelled, not
    // dropped somewhere).
    g_capture = false;
    g_mods = 0; g_btns = 0;
    for (int i = 0; i < 6; ++i) g_keys[i] = 0;
    if (GetCapture() == hwnd) ReleaseCapture();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    wcscpy_s(g_drop, L"\uD0C0\uAC9F\uC5D0\uC11C \uAC00\uC838\uC624\uB294 \uC911...");
    g_dropExpire = GetTickCount64() + 120000;
    HANDLE th = CreateThread(nullptr, 0, GrabWorker, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    printf("KVM reverse grab triggered\n"); fflush(stdout);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_DROPFILES:
    {
        // Files dropped onto the preview -> copy them to the target PC's Desktop
        // (via the board's USB drive, then a KVM-triggered go.bat).
        HDROP hDrop = (HDROP)wParam;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0)
        {
            wchar_t exeDir[MAX_PATH];
            GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
            if (wchar_t* s = wcsrchr(exeDir, L'\\')) *s = 0;

            std::wstring cmd = L"powershell.exe -ExecutionPolicy Bypass -NoProfile -WindowStyle Hidden -File \"";
            cmd += exeDir; cmd += L"\\kvm-drop.ps1\"";
            for (UINT i = 0; i < count; ++i)
            {
                wchar_t f[MAX_PATH];
                if (DragQueryFileW(hDrop, i, f, MAX_PATH)) { cmd += L" \""; cmd += f; cmd += L"\""; }
            }
            wchar_t* cmdline = _wcsdup(cmd.c_str());
            swprintf_s(g_drop, L"%u\uAC1C \uD30C\uC77C \uC804\uC1A1 \uC911...", count);  // "sending N file(s)..."
            g_dropExpire = GetTickCount64() + 90000;
            HANDLE th = CreateThread(nullptr, 0, DropWorker, cmdline, 0, nullptr);
            if (th) CloseHandle(th); else { free(cmdline); g_dropExpire = 0; }
        }
        DragFinish(hDrop);
        return 0;
    }
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
        int pms = g_pingMs.load();
        wchar_t ping[24];
        if (pms == -2)      wcscpy_s(ping, L"...");
        else if (pms < 0)   wcscpy_s(ping, L"x");
        else                swprintf_s(ping, L"%dms", pms);
        wchar_t title[420];
        swprintf_s(title, L"DeckLink Live Preview  -  %ls  %ldx%ld  ~%llu fps   [KVM %ls -> %hs  ping %ls]   %ls",
                   g_modeName, g_modeW, g_modeH, fps,
                   g_capture ? L"CONTROLLING" : L"off", g_kvmTarget, ping,
                   g_capture ? L"(leave window to release)" : L"(right-click title bar for options)");
        if (GetTickCount64() < g_dropExpire && g_drop[0])
        {
            wcscat_s(title, L"   << ");
            wcscat_s(title, g_drop);
            wcscat_s(title, L" >>");
        }
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
        // Outside control: double-click in the preview enters remote-control mode.
        // While controlling, CS_DBLCLKS delivers the 2nd press of a double-click
        // as WM_LBUTTONDBLCLK (not WM_LBUTTONDOWN), so it must be forwarded as a
        // button-down (fall through) or the target never sees the double-click.
        if (!g_capture) { BeginCapture(hwnd); return 0; }
        // fall through
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    {
        const int mx = GET_X_LPARAM(lParam);
        const int my = GET_Y_LPARAM(lParam);
        if (g_capture)
        {
            uint16_t x, y;
            const bool overVideo = MapMouse(hwnd, mx, my, &x, &y);
            // Drag-out gesture: left button released while dragging a file off the
            // target screen -> grab that file from the target onto PC1's Desktop.
            if (msg == WM_LBUTTONUP && (g_btns & 0x01) && !overVideo)
            {
                StartReverseGrab(hwnd);
                return 0;
            }
            if (overVideo) { g_lastX = x; g_lastY = y; }
            switch (msg)
            {
            case WM_LBUTTONDBLCLK:
            case WM_LBUTTONDOWN: g_btns |= 0x01; SetCapture(hwnd); break;
            case WM_LBUTTONUP:   g_btns &= ~0x01; break;
            case WM_RBUTTONDBLCLK:
            case WM_RBUTTONDOWN: g_btns |= 0x02; SetCapture(hwnd); break;
            case WM_RBUTTONUP:   g_btns &= ~0x02; break;
            case WM_MBUTTONDBLCLK:
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
        else if (wParam == 'O')
        {
            ShowKvmOptions(hwnd);   // KVM IP / ping options
        }
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (g_capture) { ForwardKey(wParam, lParam, false); return 0; }
        return 0;
    case WM_NCRBUTTONUP:
        // Right-click on the title bar -> our own options menu (replaces the
        // default system menu).
        if (wParam == HTCAPTION)
        {
            SetForegroundWindow(hwnd);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING,    IDM_OPTIONS,    L"KVM \uC124\uC815 / Ping...");
            AppendMenuW(m, MF_SEPARATOR, 0,              nullptr);
            AppendMenuW(m, MF_STRING,    IDM_FULLSCREEN, L"\uC804\uCCB4\uD654\uBA74 \uC804\uD658");
            AppendMenuW(m, MF_STRING,    IDM_EXIT,       L"\uC885\uB8CC");
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                     GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0, hwnd, nullptr);
            DestroyMenu(m);
            if      (cmd == IDM_OPTIONS)    ShowKvmOptions(hwnd);
            else if (cmd == IDM_FULLSCREEN) ToggleScreen(hwnd);
            else if (cmd == IDM_EXIT)       PostMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
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
    InitializeCriticalSection(&g_kvmLock);

    // Optional args: KVM target IP and UDP port (defaults: 192.168.0.8 50000).
    if (argc >= 2) { strncpy_s(g_kvmTarget, argv[1], _TRUNCATE); }
    if (argc >= 3) { g_kvmPort = atoi(argv[2]); }
    if (KvmInit()) printf("KVM: forwarding input to %s:%d (double-click window to control, leave to release)\n", g_kvmTarget, g_kvmPort);
    else           Log("KVM: UDP socket init failed (input forwarding disabled)");

    // Start the background ping monitor for the KVM target.
    g_pingRun.store(true);
    g_pingThread = CreateThread(nullptr, 0, PingThreadProc, nullptr, 0, nullptr);

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

        // Accept files dropped from Explorer onto the preview -> send to target.
        DragAcceptFiles(g_hwnd, TRUE);
        ChangeWindowMessageFilterEx(g_hwnd, WM_DROPFILES,  MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA,   MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(g_hwnd, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);
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
    if (g_pingThread)
    {
        g_pingRun.store(false);
        WaitForSingleObject(g_pingThread, 1500);
        CloseHandle(g_pingThread);
        g_pingThread = nullptr;
    }
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
    DeleteCriticalSection(&g_kvmLock);
    CoUninitialize();
    return exitCode;
}
