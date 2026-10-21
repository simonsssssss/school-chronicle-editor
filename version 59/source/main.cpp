// main.cpp
//
// Build WITHOUT an icon:
//   g++ main.cpp -o app.exe -municode -mwindows -lgdi32 -luser32 -lole32 -lshell32 WebView2Loader.dll.lib -O2
//
// Build WITH an icon (app.ico + app.rc next to main.cpp, app.rc contains: 1 ICON "app.ico"):
//   windres app.rc -O coff -o app_res.o
//   g++ main.cpp app_res.o -o app.exe -municode -mwindows -lgdi32 -luser32 -lole32 -lshell32 WebView2Loader.dll.lib -O2

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wrl.h>
#include <wrl/client.h>
#include <string>
#include "WebView2.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// Posted to ourselves when the window gets un-maximized, to maximize it again.
#define WM_APP_KEEP_MAXIMIZED (WM_APP + 1)

using Microsoft::WRL::ComPtr;

const wchar_t CLASS_NAME[]       = L"KronikaSzkolnaWindow";
const wchar_t APP_TITLE[]        = L"Kronika szkolna";
const wchar_t MUTEX_NAME[]       = L"KronikaSzkolna_OneInstance";
const wchar_t DATA_FOLDER_NAME[] = L"KronikaSzkolna";
const wchar_t WEBVIEW2_DOWNLOAD_URL[] = L"https://developer.microsoft.com/microsoft-edge/webview2/";
const int APP_ICON_ID = 1; // matches "1 ICON ..." in app.rc

// Size used only while the window is being created; it is shown maximized.
const int WIN_W = 1000;
const int WIN_H = 650;

// Sizes at 100% display scaling. Px() converts them for the monitor the
// window is currently on.
const int CAPTION_H = 32;
const int BTN_W = 46;

const UINT_PTR TIMER_FADE_IN = 1;
const UINT_PTR TIMER_FADE_OUT = 2;
const int FADE_STEP = 26;
const int FADE_INTERVAL_MS = 12;

enum HotButton { BTN_NONE = 0, BTN_MIN, BTN_CLOSE };

HotButton g_hot = BTN_NONE;
HotButton g_pressed = BTN_NONE;
bool g_tracking = false;
int g_alpha = 0;
bool g_firstNavigationDone = false;

ComPtr<ICoreWebView2Controller> g_webviewController;
ComPtr<ICoreWebView2> g_webview;

int Px(HWND hwnd, int value)
{
    return MulDiv(value, (int)GetDpiForWindow(hwnd), 96);
}

// Layout, left to right: [min][close], close pinned to the far right.
RECT GetCloseRect(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    return { rc.right - Px(hwnd, BTN_W), 0, rc.right, Px(hwnd, CAPTION_H) };
}
RECT GetMinRect(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    return { rc.right - 2 * Px(hwnd, BTN_W), 0, rc.right - Px(hwnd, BTN_W), Px(hwnd, CAPTION_H) };
}
bool PtInR(RECT r, POINT p) { return PtInRect(&r, p); }

void DrawCaptionButtons(HWND hwnd, HDC hdc)
{
    RECT closeR = GetCloseRect(hwnd);
    RECT minR = GetMinRect(hwnd);
    const int s5 = Px(hwnd, 5), s6 = Px(hwnd, 6), penW = Px(hwnd, 1);

    HBRUSH closeBg = CreateSolidBrush(
        g_pressed == BTN_CLOSE ? RGB(139, 10, 20) :
        g_hot == BTN_CLOSE ? RGB(232, 17, 35) : RGB(255, 255, 255));
    HBRUSH minBg = CreateSolidBrush(
        g_pressed == BTN_MIN ? RGB(200, 200, 200) :
        g_hot == BTN_MIN ? RGB(229, 229, 229) : RGB(255, 255, 255));

    FillRect(hdc, &closeR, closeBg);
    FillRect(hdc, &minR, minBg);
    DeleteObject(closeBg);
    DeleteObject(minBg);

    // Minimize icon: always dark
    HPEN minPen = CreatePen(PS_SOLID, penW, RGB(60, 60, 60));
    HPEN oldMinPen = (HPEN)SelectObject(hdc, minPen);
    int mcx = (minR.left + minR.right) / 2;
    int mcy = (minR.top + minR.bottom) / 2;
    MoveToEx(hdc, mcx - s5, mcy, nullptr);
    LineTo(hdc, mcx + s5, mcy);
    SelectObject(hdc, oldMinPen);
    DeleteObject(minPen);

    // Close icon: white when hot/pressed (so it reads over the red fill), dark otherwise
    COLORREF closeIconColor = (g_hot == BTN_CLOSE || g_pressed == BTN_CLOSE) ? RGB(255, 255, 255) : RGB(60, 60, 60);
    HPEN closePen = CreatePen(PS_SOLID, penW, closeIconColor);
    HPEN oldClosePen = (HPEN)SelectObject(hdc, closePen);
    int ccx = (closeR.left + closeR.right) / 2;
    int ccy = (closeR.top + closeR.bottom) / 2;
    MoveToEx(hdc, ccx - s5, ccy - s5, nullptr); LineTo(hdc, ccx + s6, ccy + s6);
    MoveToEx(hdc, ccx + s5, ccy - s5, nullptr); LineTo(hdc, ccx - s6, ccy + s6);
    SelectObject(hdc, oldClosePen);
    DeleteObject(closePen);
}

// ---- Paths ------------------------------------------------------------------

// Folder of app.exe, with a trailing backslash.
std::wstring GetExeDir()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir.erase(dir.find_last_of(L"\\/") + 1);
    return dir;
}

// A proper file:/// address for a path. Every character that has a special
// meaning in addresses (#, %, spaces, Polish letters...) is percent-encoded,
// so any folder name works.
std::wstring ToFileUrl(const std::wstring& path)
{
    std::wstring url = L"file:///";
    int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return url;
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), &utf8[0], len, nullptr, nullptr);

    static const char HEX[] = "0123456789ABCDEF";
    for (unsigned char c : utf8)
    {
        if (c == '\\') c = '/';
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                 || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
        if (safe)
        {
            url += (wchar_t)c;
        }
        else
        {
            url += L'%';
            url += (wchar_t)HEX[c >> 4];
            url += (wchar_t)HEX[c & 15];
        }
    }
    return url;
}

// WebView2 needs a folder it can write to. Next to app.exe that isn't always
// possible (Program Files, locked-down school accounts), so use the user's
// own AppData\Local folder instead.
std::wstring GetUserDataDir()
{
    wchar_t buf[MAX_PATH];
    std::wstring dir;
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        dir = buf;
    }
    else
    {
        GetTempPathW(MAX_PATH, buf);
        dir = buf;
    }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += DATA_FOLDER_NAME;
    CreateDirectoryW(dir.c_str(), nullptr); // fine if it already exists
    return dir;
}

// ---- WebView2 setup (manual COM callbacks — MinGW has no wrl/event.h or __uuidof) ----

void ResizeWebView(HWND hwnd);

// One reusable class for the WebView2 events below: it just calls a function.
template <typename TInterface, typename TSender, typename TArgs>
class CallbackHandler : public TInterface
{
    LONG m_refCount = 1;
    void (*m_fn)(TArgs*);
public:
    explicit CallbackHandler(void (*fn)(TArgs*)) : m_fn(fn) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<TInterface*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(TSender*, TArgs* args) override
    {
        m_fn(args);
        return S_OK;
    }
};

// Decides which keys never reach the WebView (neither the browser's own
// shortcuts nor the page). Everything not listed here works normally.
static bool ShouldBlockKey(UINT key)
{
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;

    // F1..F24: reload (F5), dev tools (F12), find (F3), caret browsing (F7)...
    // Alt+F4 must still close the window.
    if (key >= VK_F1 && key <= VK_F24) return !(alt && !ctrl && key == VK_F4);

    // Dedicated browser keys on some keyboards (back, forward, refresh, ...).
    if (key >= VK_BROWSER_BACK && key <= VK_BROWSER_HOME) return true;

    // Ctrl+Alt is AltGr on Polish keyboards (ą, ę, ł, ...): always allow.
    if (ctrl && alt) return false;

    // Alt+Left / Alt+Right / Alt+Home: back / forward / home page.
    if (alt) return key == VK_LEFT || key == VK_RIGHT || key == VK_HOME;

    if (ctrl)
    {
        switch (key)
        {
        // The only Ctrl shortcuts that stay: select all, copy, cut, paste,
        // and caret movement / word deletion.
        case 'A': case 'C': case 'V': case 'X':
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_BACK: case VK_DELETE:
        case VK_RETURN:
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            return false;
        default:
            // Everything else: bold/italic/underline, undo/redo, reload,
            // print, find, zoom, ...
            return true;
        }
    }
    return false;
}

static void OnAcceleratorKeyPressed(ICoreWebView2AcceleratorKeyPressedEventArgs* args)
{
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    if (FAILED(args->get_KeyEventKind(&kind))) return;
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
        kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return;

    UINT key = 0;
    if (FAILED(args->get_VirtualKey(&key))) return;

    if (ShouldBlockKey(key)) args->put_Handled(TRUE);
}

// The only navigation ever allowed is the very first one, to flipbook.html.
// Anything later (a file dropped on the window, a link, ...) would replace
// the chronicle, so it is cancelled.
static void OnNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args)
{
    if (g_firstNavigationDone) args->put_Cancel(TRUE);
    g_firstNavigationDone = true;
}

// No pop-up windows either.
static void OnNewWindowRequested(ICoreWebView2NewWindowRequestedEventArgs* args)
{
    args->put_Handled(TRUE);
}

class ControllerCompletedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
    HWND m_hwnd;
    LONG m_refCount = 1;
public:
    explicit ControllerCompletedHandler(HWND hwnd) : m_hwnd(hwnd) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override
    {
        if (FAILED(result) || !controller)
        {
            wchar_t buf[128];
            wsprintfW(buf, L"CreateCoreWebView2Controller failed: 0x%08X", (unsigned)result);
            MessageBoxW(m_hwnd, buf, L"WebView2 error", MB_OK | MB_ICONERROR);
            return S_OK;
        }

        g_webviewController = controller;
        g_webviewController->get_CoreWebView2(&g_webview);
        ResizeWebView(m_hwnd);

        if (g_webview)
        {
            // Turn off the browser's right-click menu, dev tools and zooming.
            ComPtr<ICoreWebView2Settings> settings;
            if (SUCCEEDED(g_webview->get_Settings(&settings)))
            {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_AreDevToolsEnabled(FALSE);
                settings->put_IsZoomControlEnabled(FALSE); // Ctrl +/-/0, Ctrl+wheel
            }

            EventRegistrationToken token;

            // Browser shortcuts (see ShouldBlockKey).
            auto* keyHandler = new CallbackHandler<ICoreWebView2AcceleratorKeyPressedEventHandler,
                ICoreWebView2Controller, ICoreWebView2AcceleratorKeyPressedEventArgs>(OnAcceleratorKeyPressed);
            g_webviewController->add_AcceleratorKeyPressed(keyHandler, &token);
            keyHandler->Release(); // WebView2 keeps its own reference

            // Never leave flipbook.html.
            auto* navHandler = new CallbackHandler<ICoreWebView2NavigationStartingEventHandler,
                ICoreWebView2, ICoreWebView2NavigationStartingEventArgs>(OnNavigationStarting);
            g_webview->add_NavigationStarting(navHandler, &token);
            navHandler->Release();

            // Never open other windows.
            auto* popupHandler = new CallbackHandler<ICoreWebView2NewWindowRequestedEventHandler,
                ICoreWebView2, ICoreWebView2NewWindowRequestedEventArgs>(OnNewWindowRequested);
            g_webview->add_NewWindowRequested(popupHandler, &token);
            popupHandler->Release();

            std::wstring htmlUrl = ToFileUrl(GetExeDir() + L"flipbook.html");
            HRESULT navHr = g_webview->Navigate(htmlUrl.c_str());
            if (FAILED(navHr))
            {
                wchar_t buf[256];
                wsprintfW(buf, L"Navigate failed: 0x%08X", (unsigned)navHr);
                MessageBoxW(m_hwnd, buf, L"WebView2 error", MB_OK | MB_ICONERROR);
            }
            g_webviewController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        return S_OK;
    }
};

class EnvironmentCompletedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
    HWND m_hwnd;
    LONG m_refCount = 1;
public:
    explicit EnvironmentCompletedHandler(HWND hwnd) : m_hwnd(hwnd) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override
    {
        if (FAILED(result) || !env)
        {
            wchar_t buf[128];
            wsprintfW(buf, L"CreateCoreWebView2Environment failed: 0x%08X", (unsigned)result);
            MessageBoxW(m_hwnd, buf, L"WebView2 error", MB_OK | MB_ICONERROR);
            return S_OK;
        }
        env->CreateCoreWebView2Controller(m_hwnd, new ControllerCompletedHandler(m_hwnd));
        return S_OK;
    }
};

// The WebView fills the whole window below the custom caption bar.
void ResizeWebView(HWND hwnd)
{
    if (!g_webviewController) return;
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    bounds.top = Px(hwnd, CAPTION_H); // keep the custom caption bar visible above it
    g_webviewController->put_Bounds(bounds);
}

// Shuts the WebView down. Safe to call more than once. Used when the window
// is destroyed and when Windows ends the session (shutdown, restart, log off).
void CloseWebView()
{
    if (g_webviewController)
    {
        g_webviewController->Close();
        g_webviewController = nullptr;
        g_webview = nullptr;
    }
}

void InitWebView(HWND hwnd)
{
    std::wstring dataDir = GetUserDataDir();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), nullptr,
        new EnvironmentCompletedHandler(hwnd));

    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"CreateCoreWebView2EnvironmentWithOptions failed: 0x%08X", (unsigned)hr);
        MessageBoxW(hwnd, buf, L"WebView2 error", MB_OK | MB_ICONERROR);
    }
}

// True if the WebView2 Runtime is installed on this computer.
static bool IsWebView2Installed()
{
    LPWSTR version = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    bool ok = SUCCEEDED(hr) && version != nullptr;
    if (version) CoTaskMemFree(version);
    return ok;
}

// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        InitWebView(hwnd);
        return 0;

    // Zero-size non-client frame: DWM still treats this as a normal
    // captioned window (native animations), but nothing is drawn there.
    case WM_NCCALCSIZE:
        if (wParam == TRUE)
            return 0;
        break;

    // Prevent Windows from repainting the native caption (icon, title,
    // buttons, active/inactive colors) whenever focus changes. Passing -1
    // as lParam tells DefWindowProc to update internal state only, with
    // no non-client repaint, so the custom look never gets replaced.
    case WM_NCACTIVATE:
        return DefWindowProc(hwnd, msg, wParam, -1);

    case WM_SETFOCUS:
        if (g_webviewController)
            g_webviewController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        return 1;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawCaptionButtons(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        // Safety net: if Windows un-maximized the window some other way
        // (e.g. Win+Left/Right snapping), maximize it again.
        if (wParam == SIZE_RESTORED && IsWindowVisible(hwnd))
            PostMessage(hwnd, WM_APP_KEEP_MAXIMIZED, 0, 0);
        ResizeWebView(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE); // the caption buttons move with the right edge
        break;

    // The window moved to a monitor with a different scaling (e.g. with
    // Win+Shift+Arrow): take the size Windows suggests for it, then redo
    // everything that depends on the scale.
    case WM_DPICHANGED:
    {
        const RECT* r = (const RECT*)lParam;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeWebView(hwnd);
        if (g_webviewController) g_webviewController->NotifyParentWindowPositionChanged();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_NCHITTEST:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);

        if (PtInR(GetCloseRect(hwnd), pt)) return HTCLIENT;
        if (PtInR(GetMinRect(hwnd), pt)) return HTCLIENT;

        if (pt.y >= 0 && pt.y < Px(hwnd, CAPTION_H)) return HTCAPTION;

        return HTCLIENT;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HotButton newHot = BTN_NONE;
        if (PtInR(GetCloseRect(hwnd), pt)) newHot = BTN_CLOSE;
        else if (PtInR(GetMinRect(hwnd), pt)) newHot = BTN_MIN;

        if (newHot != g_hot)
        {
            g_hot = newHot;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!g_tracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = false;
        g_hot = BTN_NONE;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInR(GetCloseRect(hwnd), pt)) g_pressed = BTN_CLOSE;
        else if (PtInR(GetMinRect(hwnd), pt)) g_pressed = BTN_MIN;
        else break;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        HotButton released = g_pressed;
        g_pressed = BTN_NONE;
        ReleaseCapture();

        if (released == BTN_CLOSE && PtInR(GetCloseRect(hwnd), pt))
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        else if (released == BTN_MIN && PtInR(GetMinRect(hwnd), pt))
            SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        g_pressed = BTN_NONE;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
        if (wParam == HTCAPTION) return 0;
        break;

    case WM_NCLBUTTONDBLCLK:
        if (wParam == HTCAPTION) return 0;
        break;

    case WM_SYSCOMMAND:
    {
        UINT cmd = (UINT)(wParam & 0xFFF0);
        if (cmd == SC_KEYMENU || cmd == SC_MOUSEMENU || cmd == SC_MAXIMIZE) return 0;
        // Win+Down (and similar) must not shrink the maximized window.
        // Restoring from the taskbar after minimizing is still allowed.
        if (cmd == SC_RESTORE && !IsIconic(hwnd)) return 0;
        // No dragging the title bar to pull the window out of maximized.
        if ((cmd == SC_MOVE || cmd == SC_SIZE) && IsZoomed(hwnd)) return 0;
        break;
    }

    case WM_APP_KEEP_MAXIMIZED:
        if (!IsZoomed(hwnd) && !IsIconic(hwnd)) ShowWindow(hwnd, SW_MAXIMIZE);
        return 0;

    case WM_TIMER:
    {
        if (wParam == TIMER_FADE_IN)
        {
            g_alpha += FADE_STEP;
            if (g_alpha >= 255)
            {
                g_alpha = 255;
                KillTimer(hwnd, TIMER_FADE_IN);
                SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);

                LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
                RedrawWindow(hwnd, nullptr, nullptr,
                    RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
                return 0;
            }
            SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
            return 0;
        }
        if (wParam == TIMER_FADE_OUT)
        {
            g_alpha -= FADE_STEP;
            if (g_alpha <= 0)
            {
                KillTimer(hwnd, TIMER_FADE_OUT);
                DestroyWindow(hwnd);
                return 0;
            }
            SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
    {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);

        KillTimer(hwnd, TIMER_FADE_IN);
        SetTimer(hwnd, TIMER_FADE_OUT, FADE_INTERVAL_MS, nullptr);
        return 0;
    }

    // Windows asks whether the session may end: shutdown, restart, log off,
    // or an installer (Restart Manager) that needs the app closed. On these
    // occasions WM_CLOSE is never sent, so the normal closing path above
    // does not run.
    //
    // Nothing needs saving yet, so the answer is always "yes". Once the
    // chronicle can be saved, this is where saving starts: call
    // ShutdownBlockReasonCreate(hwnd, L"Zapisywanie kroniki...") so Windows
    // shows why it has to wait, and answer "yes" when the save is done.
    case WM_QUERYENDSESSION:
        return TRUE;

    // The session really ends (wParam TRUE). Close down right away, without
    // the fade-out: Windows may end the process soon after this returns.
    case WM_ENDSESSION:
        if (wParam)
        {
            KillTimer(hwnd, TIMER_FADE_IN);
            KillTimer(hwnd, TIMER_FADE_OUT);
            CloseWebView();
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        CloseWebView();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Only one copy at a time: starting the app again just brings the
    // window that is already open to the front.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND running = FindWindowW(CLASS_NAME, nullptr);
        if (running)
        {
            if (IsIconic(running)) ShowWindow(running, SW_RESTORE);
            SetForegroundWindow(running);
        }
        if (instanceMutex) CloseHandle(instanceMutex);
        return 0;
    }

    // Without the WebView2 Runtime nothing can be shown: explain that
    // instead of failing with a technical error.
    if (!IsWebView2Installed())
    {
        int answer = MessageBoxW(nullptr,
            L"Na tym komputerze brakuje sk\u0142adnika Microsoft Edge WebView2 Runtime, "
            L"bez kt\u00f3rego Kronika szkolna nie mo\u017ce dzia\u0142a\u0107.\n\n"
            L"Czy otworzy\u0107 stron\u0119, z kt\u00f3rej mo\u017cna go pobra\u0107?",
            APP_TITLE, MB_YESNO | MB_ICONERROR);
        if (answer == IDYES)
            ShellExecuteW(nullptr, L"open", WEBVIEW2_DOWNLOAD_URL, nullptr, nullptr, SW_SHOWNORMAL);
        if (instanceMutex) { ReleaseMutex(instanceMutex); CloseHandle(instanceMutex); }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // required before any WebView2 / COM calls

    // App icon from app.rc if it was compiled in, otherwise the standard one.
    HICON bigIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(APP_ICON_ID));
    HICON smallIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(APP_ICON_ID), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!bigIcon)   bigIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!smallIcon) smallIcon = bigIcon;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hIcon = bigIcon;
    wc.hIconSm = smallIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    DWORD exStyle = WS_EX_LAYERED | WS_EX_APPWINDOW;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - WIN_W) / 2;
    int y = (screenH - WIN_H) / 2;

    HWND hwnd = CreateWindowExW(exStyle, CLASS_NAME, APP_TITLE, style,
        x, y, WIN_W, WIN_H, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
    {
        CoUninitialize();
        if (instanceMutex) { ReleaseMutex(instanceMutex); CloseHandle(instanceMutex); }
        return 0;
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    g_alpha = 0;
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_MAXIMIZE);
    UpdateWindow(hwnd);
    SetTimer(hwnd, TIMER_FADE_IN, FADE_INTERVAL_MS, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    if (instanceMutex) { ReleaseMutex(instanceMutex); CloseHandle(instanceMutex); }
    return 0;
}