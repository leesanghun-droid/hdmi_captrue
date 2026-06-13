// DeckLink pixel-perfect live preview (Win32 + OpenGL, no MFC).
//
// We do NOT use the DeckLink GL screen-preview helper, because that helper is a
// scaled "monitoring" view and applies filtering that softens text. Instead we
// upload each captured frame to a GL texture and draw it 1:1 with GL_NEAREST,
// so the on-screen pixels match the captured pixels exactly (lossless preview).

#include <windows.h>
#include <gl/gl.h>
#include <atomic>
#include <cstdio>

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
        wchar_t title[320];
        swprintf_s(title, L"DeckLink Live Preview  -  %ls  %ldx%ld  ~%llu fps  (frames: %llu)",
                   g_modeName, g_modeW, g_modeH, fps, now);
        SetWindowTextW(hwnd, title);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (g_fullscreen) ToggleFullscreen(hwnd);
            else              PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        else if (wParam == 'F' || wParam == VK_F11)
        {
            ToggleFullscreen(hwnd);
        }
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

int main()
{
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

    Log("Live preview started (pixel-perfect 1:1).  [F]/[F11] = fullscreen   [ESC] = exit");
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
    CoUninitialize();
    return exitCode;
}
