/*

windres app.rc -O coff -o app_res.o

g++ main.cpp app_res.o -o app.exe -municode -mwindows -O2 -s -static -static-libgcc -static-libstdc++ -lole32 -lshell32 -luser32 -lgdi32 -luuid -ladvapi32

*/

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wrl.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <utility>
#include <cstring>
#include <shobjidl.h>
#include <shlobj.h>
#include "WebView2.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// Posted to ourselves when the window gets un-maximized, to maximize it again.
#define WM_APP_KEEP_MAXIMIZED (WM_APP + 1)
// Posted to ourselves when the page (flipbook.html) sent a message.
#define WM_APP_PAGE_MESSAGE (WM_APP + 2)

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
// Closing: how long to wait for the page to answer before closing anyway
// (only when the page last said everything is saved).
const UINT_PTR TIMER_CLOSE_WAIT = 3;
const int CLOSE_WAIT_MS = 1500;

enum HotButton { BTN_NONE = 0, BTN_MIN, BTN_CLOSE };

HotButton g_hot = BTN_NONE;
HotButton g_pressed = BTN_NONE;
bool g_tracking = false;
int g_alpha = 0;
bool g_firstNavigationDone = false;
HWND g_hwnd = nullptr;
std::vector<std::wstring> g_inbox;  // messages from the page, waiting to be handled
std::wstring g_filePath;            // file the chronicle was saved to / opened from
std::wstring g_pendingOpenPath;     // file just read, until the page accepts it
bool g_dirty = false;               // the page says: content is not in a file
bool g_closeConfirmed = false;      // closing was allowed (nothing to lose, or the user agreed)
std::wstring g_externalOpenPath;    // plik z Eksploratora, czeka na otwarcie przez stronę
bool g_pageReady = false;           // strona już działa i odbiera wiadomości

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

// ---- Saving and opening files ------------------------------------------------
// The page sends "command\ndata" text messages, the answers look the same.

static void PostToPage(const std::wstring& text)
{
    if (g_webview) g_webview->PostWebMessageAsString(text.c_str());
}

// Own copies of the GUIDs (MinGW has no __uuidof).
static const CLSID CLSID_FileOpenDialog_ = {0xDC1C5A9C, 0xE88A, 0x4DDE, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const CLSID CLSID_FileSaveDialog_ = {0xC0B4E2F3, 0xBA21, 0x4773, {0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B}};
static const IID   IID_IFileOpenDialog_  = {0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
static const IID   IID_IFileSaveDialog_  = {0x84BCCD23, 0x5FDE, 0x4CDB, {0xAE, 0xA4, 0xAF, 0x64, 0xB8, 0x3D, 0x78, 0xAB}};

// WebView2 callback interfaces implemented below (see QueryInterface).
static const IID IID_EnvironmentCompletedHandler_ = {0x4E8A3389, 0xC9D8, 0x4BD2, {0xB6, 0xB5, 0x12, 0x4F, 0xEE, 0x6C, 0xC1, 0x4D}};
static const IID IID_ControllerCompletedHandler_  = {0x6C4819F3, 0xC9B7, 0x4260, {0x81, 0x27, 0xC9, 0xF5, 0xBD, 0xE7, 0xF6, 0x8C}};
static const IID IID_WebMessageReceivedHandler_   = {0x57213F19, 0x00E6, 0x49FA, {0x8E, 0x07, 0x89, 0x8E, 0xA0, 0x1E, 0xCB, 0xD2}};
static const IID IID_NavigationStartingHandler_   = {0x9ADBE429, 0xF36D, 0x432B, {0x9D, 0xDC, 0xF8, 0x88, 0x1F, 0xBD, 0x76, 0xE3}};
static const IID IID_NewWindowRequestedHandler_   = {0xD4C185FE, 0xC81C, 0x4989, {0x97, 0xAF, 0x2D, 0x3F, 0xA7, 0xAB, 0x56, 0x51}};
static const IID IID_AcceleratorKeyHandler_       = {0xB29C7E28, 0xFA79, 0x41A8, {0x8E, 0x44, 0x65, 0x81, 0x1C, 0x76, 0xDC, 0xB2}};
static const IID IID_ProcessFailedHandler_        = {0x79E0AEA4, 0x990B, 0x42D9, {0xAA, 0x1D, 0x0F, 0xCC, 0x2E, 0x5B, 0xC7, 0xF1}};

// Only IUnknown and the object's own interface are supported.
template <typename T>
static HRESULT QueryOwnInterface(T* self, const IID& own, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, own))
    {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    *ppv = self;
    self->AddRef();
    return S_OK;
}

const wchar_t FILE_FILTER_NAME[] = L"Kronika szkolna (*.kronika)";
const wchar_t FILE_FILTER_SPEC[] = L"*.kronika";
// Skojarzenie plików .kronika z programem (dwuklik w Eksploratorze).
const wchar_t FILE_PROG_ID[] = L"KronikaSzkolna.Plik";
// WM_COPYDATA: druga kopia programu przekazuje ścieżkę pliku do otwarcia.
const ULONG_PTR COPYDATA_OPEN_FILE = 0x4B524F4E;   // "KRON"

const wchar_t LAYOUTS_FILE_NAME[]     = L"uklady-punktow.json";
// Name used by earlier versions, when the file lay next to app.exe.
const wchar_t OLD_LAYOUTS_FILE_NAME[] = L"uklady-punktow.json";

static std::wstring GetLayoutsPath()
{
    return GetUserDataDir() + L"\\" + LAYOUTS_FILE_NAME;
}

static bool FileMissing(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

static bool ResultPath(IFileDialog* dlg, std::wstring& out)
{
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

static bool PickOpenPath(HWND hwnd, std::wstring& out, const wchar_t* title = nullptr)
{
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog_, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog_, reinterpret_cast<void**>(dlg.GetAddressOf()))))
        return false;
    COMDLG_FILTERSPEC types[] = { { FILE_FILTER_NAME, FILE_FILTER_SPEC } };
    dlg->SetFileTypes(1, types);
    if (title) dlg->SetTitle(title);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    // No FOS_ALLOWMULTISELECT: one file only.
    dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(hwnd))) return false;   // "Anuluj" too
    return ResultPath(dlg.Get(), out);
}

// The normal Windows "Save as" window: folder and file name.
static bool PickSavePath(HWND hwnd, std::wstring& out)
{
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog_, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileSaveDialog_, reinterpret_cast<void**>(dlg.GetAddressOf()))))
        return false;
    COMDLG_FILTERSPEC types[] = { { FILE_FILTER_NAME, FILE_FILTER_SPEC } };
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"kronika");
    dlg->SetFileName(L"Kronika szkolna");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(hwnd))) return false;
    return ResultPath(dlg.Get(), out);
}

static bool ReadUtf8File(const std::wstring& path, std::wstring& out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart > 512LL * 1024 * 1024)
    {
        CloseHandle(f);
        return false;
    }
    std::string data((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    bool ok = data.empty() ||
              (ReadFile(f, &data[0], (DWORD)data.size(), &read, nullptr) && read == data.size());
    CloseHandle(f);
    if (!ok) return false;

    size_t start = (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
                    (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, data.data() + start, (int)(data.size() - start), nullptr, 0);
    out.assign(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, data.data() + start, (int)(data.size() - start), &out[0], n);
    return true;
}

// Writes to "<name>.tmp" first and only then replaces the real file, so a
// failed save (full disk, pulled USB stick) never damages the old file.
static bool WriteUtf8FileSafely(const std::wstring& path, const std::wstring& text)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len > 0 ? len : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], len, nullptr, nullptr);

    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = utf8.empty() ||
              (WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr) && written == utf8.size());
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// ---- View-only copy -----------------------------------------------------------
// "Export" makes a copy of app.exe with a package glued to its end: the files
// the page needs (flipbook.html, lib, fonts, WebView2Loader.dll) and one
// chronicle. A copy that finds such a package at its own end starts as a
// viewer: it unpacks those files, shows that chronicle and offers no tools.
//
// Package: entries [name length, 4 bytes][data length, 4 bytes][name][data],
// then [size of all entries, 8 bytes][PACKAGE_MAGIC, 8 bytes].

const char PACKAGE_MAGIC[]         = "KRONIKA1";   // exactly 8 characters
const char PACKAGE_DOC_NAME[]      = "kronika";    // the chronicle (never written to disk)
const wchar_t VIEWER_FOLDER_NAME[] = L"Podglad";
const wchar_t VIEWER_CLASS_NAME[]  = L"KronikaSzkolnaViewerWindow";
const wchar_t LOADER_DLL_NAME[]    = L"WebView2Loader.dll";

bool g_viewer = false;          // this copy only shows its own chronicle
std::wstring g_pageDir;         // folder of flipbook.html, with a trailing backslash
std::wstring g_viewerDoc;       // viewer: the chronicle, until the page asks for it
std::wstring g_exportSource;    // export: the chosen .kronika file...
std::wstring g_exportContent;   // ...and its content, until the page has checked it

struct PackedFile { std::string name; std::string data; };
enum PackageState { PACKAGE_NONE, PACKAGE_OK, PACKAGE_BAD };

// ---- Ustawienia programu (powiększenie książki) -------------------------------
// Jeden plik w folderze danych użytkownika, wspólny dla wszystkich kronik.
// Zostaje po zamknięciu programu i po ponownym uruchomieniu komputera.
// Wersja tylko do przeglądania ma swój własny plik (w folderze "Podglad"),
// więc nie zmienia powiększenia zwykłego programu.
const wchar_t SETTINGS_FILE_NAME[] = L"ustawienia.json";
const int ZOOM_MIN = 10;        // te same granice co suwak w flipbook.html
const int ZOOM_MAX = 400;
const int ZOOM_DEFAULT = 100;

static std::wstring GetSettingsPath()
{
    std::wstring dir = GetUserDataDir();
    if (g_viewer)
    {
        dir = dir + L"\\" + VIEWER_FOLDER_NAME;
        CreateDirectoryW(dir.c_str(), nullptr);   // fine if it already exists
    }
    return dir + L"\\" + SETTINGS_FILE_NAME;
}

// Liczba całkowita z tekstu (same cyfry), albo -1.
static int ParseZoomNumber(const std::wstring& text, size_t pos)
{
    while (pos < text.size() && (text[pos] == L' ' || text[pos] == L'\t' || text[pos] == L'\r' || text[pos] == L'\n'))
        pos++;
    int value = 0, digits = 0;
    while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9' && digits < 6)
    {
        value = value * 10 + (text[pos] - L'0');
        pos++;
        digits++;
    }
    return digits > 0 ? value : -1;
}

// Zapamiętane powiększenie w procentach. Brak pliku albo zła wartość: 100.
static int LoadSavedZoom()
{
    std::wstring content;
    if (!ReadUtf8File(GetSettingsPath(), content)) return ZOOM_DEFAULT;
    size_t key = content.find(L"\"zoom\"");
    if (key == std::wstring::npos) return ZOOM_DEFAULT;
    size_t colon = content.find(L':', key);
    if (colon == std::wstring::npos) return ZOOM_DEFAULT;
    int zoom = ParseZoomNumber(content, colon + 1);
    return (zoom >= ZOOM_MIN && zoom <= ZOOM_MAX) ? zoom : ZOOM_DEFAULT;
}

// Zapisuje powiększenie (bezpiecznie: najpierw plik .tmp, patrz WriteUtf8FileSafely).
static void SaveZoom(int zoom)
{
    if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) return;
    wchar_t text[64];
    wsprintfW(text, L"{\n  \"zoom\": %d\n}\n", zoom);
    WriteUtf8FileSafely(GetSettingsPath(), text);
}

// WebView2Loader.dll is loaded by hand, not listed among the DLLs Windows
// needs to start app.exe: a view-only copy has it inside itself, so at start
// there is no such file next to it yet.
typedef HRESULT (STDAPICALLTYPE *CreateEnvironmentFn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
typedef HRESULT (STDAPICALLTYPE *GetBrowserVersionFn)(PCWSTR, LPWSTR*);
CreateEnvironmentFn g_createEnvironment = nullptr;
GetBrowserVersionFn g_getBrowserVersion = nullptr;

static bool LoadWebView2Loader(const std::wstring& dllPath)
{
    HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) return false;
    g_createEnvironment = (CreateEnvironmentFn)(void*)GetProcAddress(dll, "CreateCoreWebView2EnvironmentWithOptions");
    g_getBrowserVersion = (GetBrowserVersionFn)(void*)GetProcAddress(dll, "GetAvailableCoreWebView2BrowserVersionString");
    return g_createEnvironment && g_getBrowserVersion;
}

static std::wstring GetExePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return exePath;
}

static std::string ToUtf8(const std::wstring& text)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], len, nullptr, nullptr);
    return out;
}

static std::wstring FromUtf8(const std::string& text)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out(len > 0 ? len : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], len);
    return out;
}

// ReadFile / WriteFile until everything is done (they take at most 4 GB at once).
static bool ReadExact(HANDLE f, void* buf, size_t size)
{
    char* p = static_cast<char*>(buf);
    while (size > 0)
    {
        DWORD part = size > 0x4000000 ? 0x4000000 : (DWORD)size, done = 0;
        if (!ReadFile(f, p, part, &done, nullptr) || done == 0) return false;
        p += done;
        size -= done;
    }
    return true;
}

static bool WriteAll(HANDLE f, const void* buf, size_t size)
{
    const char* p = static_cast<const char*>(buf);
    while (size > 0)
    {
        DWORD part = size > 0x4000000 ? 0x4000000 : (DWORD)size, done = 0;
        if (!WriteFile(f, p, part, &done, nullptr) || done == 0) return false;
        p += done;
        size -= done;
    }
    return true;
}

// A whole file exactly as it is on disk (no text conversion).
static bool ReadFileBytes(const std::wstring& path, std::string& out)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart <= 0x7FFFFFFF;
    if (ok)
    {
        out.assign((size_t)size.QuadPart, '\0');
        ok = out.empty() || ReadExact(f, &out[0], out.size());
    }
    CloseHandle(f);
    return ok;
}

// "<name>.<process number>.tmp": two copies started at once never share it.
static std::wstring TempNameFor(const std::wstring& path)
{
    wchar_t suffix[32];
    wsprintfW(suffix, L".%u.tmp", (unsigned)GetCurrentProcessId());
    return path + suffix;
}

static bool FileHasSize(const std::wstring& path, size_t size)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    return info.nFileSizeHigh == 0 && info.nFileSizeLow == size;
}

// Every file of a folder that lies next to app.exe ("lib", "fonts").
static bool AddFolderFiles(const std::wstring& folder, std::vector<PackedFile>& files)
{
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((GetExeDir() + folder + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        PackedFile pf;
        pf.name = ToUtf8(folder + L"/" + fd.cFileName);
        ok = ReadFileBytes(GetExeDir() + folder + L"\\" + fd.cFileName, pf.data);
        if (ok) files.push_back(std::move(pf));
    } while (ok && FindNextFileW(find, &fd));
    FindClose(find);
    return ok;
}

// Export: writes the view-only copy. As with saving, a "...tmp" file is
// written first, so a failed export never damages a file that was there.
static bool BuildViewerFile(const std::wstring& target, const std::wstring& chronicle)
{
    std::string exe;
    if (!ReadFileBytes(GetExePath(), exe)) return false;

    std::vector<PackedFile> files(2);
    files[0].name = "flipbook.html";
    files[1].name = ToUtf8(LOADER_DLL_NAME);
    if (!ReadFileBytes(GetExeDir() + L"flipbook.html", files[0].data)) return false;
    if (!ReadFileBytes(GetExeDir() + LOADER_DLL_NAME, files[1].data)) return false;
    if (!AddFolderFiles(L"lib", files)) return false;
    AddFolderFiles(L"fonts", files);   // the cover font: the page has a replacement for it

    PackedFile doc;
    doc.name = PACKAGE_DOC_NAME;
    doc.data = ToUtf8(chronicle);
    files.push_back(std::move(doc));

    std::wstring tmp = TempNameFor(target);
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    ULONGLONG packageSize = 0;
    bool ok = WriteAll(f, exe.data(), exe.size());
    for (size_t i = 0; ok && i < files.size(); i++)
    {
        DWORD nameLen = (DWORD)files[i].name.size(), dataLen = (DWORD)files[i].data.size();
        ok = WriteAll(f, &nameLen, 4) && WriteAll(f, &dataLen, 4)
          && WriteAll(f, files[i].name.data(), nameLen)
          && WriteAll(f, files[i].data.data(), dataLen);
        packageSize += 8ULL + nameLen + dataLen;
    }
    ok = ok && WriteAll(f, &packageSize, 8) && WriteAll(f, PACKAGE_MAGIC, 8) && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Start: looks for a package at the end of this very .exe file.
// PACKAGE_NONE: the normal program. PACKAGE_BAD: a damaged view-only copy.
static PackageState ReadPackage(std::vector<PackedFile>& files, std::string& chronicle)
{
    HANDLE f = CreateFileW(GetExePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return PACKAGE_NONE;

    PackageState state = PACKAGE_NONE;
    LARGE_INTEGER size, pos;
    ULONGLONG packageSize = 0;
    char magic[8];
    if (GetFileSizeEx(f, &size) && size.QuadPart > 16)
    {
        pos.QuadPart = size.QuadPart - 16;
        if (SetFilePointerEx(f, pos, nullptr, FILE_BEGIN)
            && ReadExact(f, &packageSize, 8) && ReadExact(f, magic, 8)
            && memcmp(magic, PACKAGE_MAGIC, 8) == 0)
        {
            state = PACKAGE_BAD;
            bool ok = packageSize <= (ULONGLONG)size.QuadPart - 16;
            if (ok)
            {
                pos.QuadPart = size.QuadPart - 16 - (LONGLONG)packageSize;
                ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) != 0;
            }
            bool haveDoc = false;
            ULONGLONG left = packageSize;
            while (ok && left > 0)
            {
                DWORD nameLen = 0, dataLen = 0;
                ok = left >= 8 && ReadExact(f, &nameLen, 4) && ReadExact(f, &dataLen, 4);
                if (!ok) break;
                left -= 8;
                ok = nameLen > 0 && nameLen <= 512 && (ULONGLONG)nameLen + dataLen <= left;
                if (!ok) break;
                PackedFile pf;
                pf.name.assign(nameLen, '\0');
                pf.data.assign(dataLen, '\0');
                ok = ReadExact(f, &pf.name[0], nameLen)
                  && (dataLen == 0 || ReadExact(f, &pf.data[0], dataLen));
                if (!ok) break;
                left -= (ULONGLONG)nameLen + dataLen;
                if (pf.name == PACKAGE_DOC_NAME) { chronicle.swap(pf.data); haveDoc = true; }
                else files.push_back(std::move(pf));
            }
            if (ok && haveDoc) state = PACKAGE_OK;
        }
    }
    CloseHandle(f);
    return state;
}

// Viewer start: puts the page's files into the user's data folder and gives
// back that folder (with a trailing backslash). The folder's name is made
// from the files themselves, so copies exported by the same version of the
// program share one folder, and another version never overwrites it.
static bool UnpackViewerFiles(const std::vector<PackedFile>& files, std::wstring& dirOut)
{
    ULONGLONG hash = 1469598103934665603ULL;
    for (const PackedFile& pf : files)
    {
        for (unsigned char c : pf.name) { hash ^= c; hash *= 1099511628211ULL; }
        for (unsigned char c : pf.data) { hash ^= c; hash *= 1099511628211ULL; }
    }
    wchar_t id[32];
    wsprintfW(id, L"%08X%08X", (unsigned)(hash >> 32), (unsigned)(hash & 0xFFFFFFFF));

    std::wstring dir = GetUserDataDir() + L"\\" + VIEWER_FOLDER_NAME;
    CreateDirectoryW(dir.c_str(), nullptr);   // fine if it already exists
    dir = dir + L"\\" + id;
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L'\\';

    for (const PackedFile& pf : files)
    {
        // Only plain names like "flipbook.html" or "lib/react.js".
        if (pf.name.empty() || pf.name[0] == '/' || pf.name.find("..") != std::string::npos
            || pf.name.find(':') != std::string::npos || pf.name.find('\\') != std::string::npos)
            return false;

        std::wstring rel = FromUtf8(pf.name);
        for (wchar_t& c : rel) if (c == L'/') c = L'\\';
        size_t slash = rel.find_last_of(L'\\');
        if (slash != std::wstring::npos) CreateDirectoryW((dir + rel.substr(0, slash)).c_str(), nullptr);

        std::wstring path = dir + rel;
        if (FileHasSize(path, pf.data.size())) continue;   // unpacked earlier

        std::wstring tmp = TempNameFor(path);
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = f != INVALID_HANDLE_VALUE;
        if (ok)
        {
            ok = WriteAll(f, pf.data.data(), pf.data.size());
            CloseHandle(f);
            if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) DeleteFileW(tmp.c_str());
        }
        // Another copy started at the same moment may have just written it.
        if (!ok && !FileHasSize(path, pf.data.size())) return false;
    }
    dirOut = dir;
    return true;
}

// The Windows "Save as" window for the view-only copy: an .exe file,
// suggested name = name of the chosen chronicle file.
static bool PickExportPath(HWND hwnd, const std::wstring& source, std::wstring& out)
{
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog_, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileSaveDialog_, reinterpret_cast<void**>(dlg.GetAddressOf()))))
        return false;
    COMDLG_FILTERSPEC types[] = { { L"Kronika do przegl\u0105dania (*.exe)", L"*.exe" } };
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"exe");
    dlg->SetTitle(L"Zapisz wersj\u0119 do przegl\u0105dania");

    std::wstring name = source.substr(source.find_last_of(L"\\/") + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.erase(dot);
    dlg->SetFileName(name.c_str());

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(hwnd))) return false;
    return ResultPath(dlg.Get(), out);
}

// ---- Skojarzenie plików .kronika z programem ---------------------------------
// Zapisywane w rejestrze bieżącego użytkownika (bez uprawnień administratora)
// przy każdym uruchomieniu, więc działa też po przeniesieniu folderu programu.

// Ustawia domyślną wartość klucza. true, jeśli coś się zmieniło.
static bool SetRegText(const std::wstring& subkey, const std::wstring& value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    std::wstring old(1024, L'\0');
    DWORD type = 0, size = (DWORD)((old.size() - 1) * sizeof(wchar_t));
    bool same = RegQueryValueExW(key, nullptr, nullptr, &type, (BYTE*)&old[0], &size) == ERROR_SUCCESS
             && type == REG_SZ && lstrcmpW(old.c_str(), value.c_str()) == 0;
    bool changed = !same && RegSetValueExW(key, nullptr, 0, REG_SZ, (const BYTE*)value.c_str(),
                        (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return changed;
}

static void RegisterFileType()
{
    const std::wstring exe = GetExePath();
    const std::wstring cls = std::wstring(L"Software\\Classes\\") + FILE_PROG_ID;
    bool changed = false;
    changed |= SetRegText(L"Software\\Classes\\.kronika", FILE_PROG_ID);
    changed |= SetRegText(cls, L"Kronika szkolna");
    changed |= SetRegText(cls + L"\\DefaultIcon", L"\"" + exe + L"\",0");
    changed |= SetRegText(cls + L"\\shell\\open\\command", L"\"" + exe + L"\" \"%1\"");
    // Eksplorator od razu pokazuje nową ikonę i nowe skojarzenie.
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// The fade-out that ends with DestroyWindow (see WM_TIMER).
static void StartFadeOut(HWND hwnd)
{
    KillTimer(hwnd, TIMER_CLOSE_WAIT);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
    KillTimer(hwnd, TIMER_FADE_IN);
    SetTimer(hwnd, TIMER_FADE_OUT, FADE_INTERVAL_MS, nullptr);
}

// Handles the page's messages. Runs from the message loop, not from inside
// a WebView2 event, because the file windows are modal.
static void ProcessPageMessages(HWND hwnd)
{
    std::vector<std::wstring> batch;
    batch.swap(g_inbox);
    for (const std::wstring& msg : batch)
    {
        size_t nl = msg.find(L'\n');
        std::wstring cmd = msg.substr(0, nl);
        std::wstring rest = (nl == std::wstring::npos) ? L"" : msg.substr(nl + 1);
        // A view-only copy: the page only asks for its chronicle.
        if (g_viewer)
        {
            if (cmd == L"viewerReady")
            {
                PostToPage(L"viewerDoc\n" + g_viewerDoc);
                g_viewerDoc.clear();
                g_viewerDoc.shrink_to_fit();
            }
            else if (cmd == L"zoomSave")   // powiększenie książki (własny plik ustawień)
            {
                SaveZoom(ParseZoomNumber(rest, 0));
            }
            continue;
        }
        if (cmd == L"dirty")
        {
            g_dirty = (rest == L"1");
        }
        else if (cmd == L"save" || cmd == L"saveAs")
        {
            // "saveAs": zawsze pyta o miejsce, nawet gdy kronika jest już plikiem.
            std::wstring path = (cmd == L"saveAs") ? std::wstring() : g_filePath;
            if (path.empty() && !PickSavePath(hwnd, path)) { PostToPage(L"saveCancelled"); continue; }
            bool ok = WriteUtf8FileSafely(path, rest);

            if (!ok && cmd == L"save" && !g_filePath.empty())
            {
                MessageBoxW(hwnd,
                    L"Nie uda\u0142o si\u0119 zapisa\u0107 pliku w dotychczasowym miejscu.\n\n"
                    L"Wybierz inne miejsce zapisu.",
                    APP_TITLE, MB_OK | MB_ICONWARNING);
                if (!PickSavePath(hwnd, path)) { PostToPage(L"saveCancelled"); continue; }
                ok = WriteUtf8FileSafely(path, rest);
            }

            if (ok)
            {
                g_filePath = path;
                PostToPage(L"saved");
            }
            else
            {
                PostToPage(L"saveFailed");
            }
        }
        else if (cmd == L"open")
        {
            std::wstring path, content;
            if (!PickOpenPath(hwnd, path)) { PostToPage(L"openCancelled"); continue; }
            if (!ReadUtf8File(path, content)) { PostToPage(L"openFailed"); continue; }
            g_pendingOpenPath = path;
            PostToPage(L"opened\n" + content);
        }
        else if (cmd == L"openAccepted")   // the page could read the file
        {
            g_filePath = g_pendingOpenPath;
            g_pendingOpenPath.clear();
        }
        else if (cmd == L"openRejected")   // not a chronicle file: keep the old path
        {
            g_pendingOpenPath.clear();
        }
        else if (cmd == L"pageReady")      // strona działa: może czeka plik z Eksploratora
        {
            g_pageReady = true;
            if (!g_externalOpenPath.empty()) PostToPage(L"externalOpen");
        }
        else if (cmd == L"openPending")    // otwórz plik przekazany z Eksploratora
        {
            std::wstring path, content;
            path.swap(g_externalOpenPath);
            if (path.empty()) { PostToPage(L"openCancelled"); continue; }
            if (!ReadUtf8File(path, content)) { PostToPage(L"openFailed"); continue; }
            g_pendingOpenPath = path;
            PostToPage(L"opened\n" + content);
        }
        else if (cmd == L"layoutsRead")    // content of the layouts file
        {
            std::wstring path = GetLayoutsPath(), content;
            // No new file yet: take the layouts saved by an earlier version.
            if (FileMissing(path)) path = GetExeDir() + OLD_LAYOUTS_FILE_NAME;

            if (FileMissing(path))
                PostToPage(L"layoutsData\n");              // no file yet: empty list
            else if (ReadUtf8File(path, content))
                PostToPage(L"layoutsData\n" + content);
            else
                PostToPage(L"layoutsReadFailed");
        }
        else if (cmd == L"layoutsWrite")   // the whole layouts file, already changed
        {
            PostToPage(WriteUtf8FileSafely(GetLayoutsPath(), rest)
                       ? L"layoutsWritten" : L"layoutsWriteFailed");
        }
        else if (cmd == L"layoutsSetAside")   // the file isn't a layouts file
        {
            // Keep it under another name and start again from an empty list.
            std::wstring path = GetLayoutsPath();
            if (FileMissing(path)) path = GetExeDir() + OLD_LAYOUTS_FILE_NAME;
            if (!FileMissing(path))
                MoveFileExW(path.c_str(), (path + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
            WriteUtf8FileSafely(GetLayoutsPath(), L"");   // empty file = empty list
        }
        else if (cmd == L"zoomSave")       // użytkownik zmienił powiększenie książki
        {
            SaveZoom(ParseZoomNumber(rest, 0));
        }
        else if (cmd == L"newDoc")         // nowa, pusta kronika: nie jest jeszcze plikiem
        {
            g_filePath.clear();
            g_pendingOpenPath.clear();
        }
        else if (cmd == L"export")         // view-only copy, step 1: which chronicle file
        {
            std::wstring path, content;
            if (!PickOpenPath(hwnd, path, L"Wybierz plik kroniki")) { PostToPage(L"exportCancelled"); continue; }
            if (!ReadUtf8File(path, content)) { PostToPage(L"exportReadFailed"); continue; }
            g_exportSource = path;
            g_exportContent.swap(content);
            PostToPage(L"exportCheck\n" + g_exportContent);   // the page checks that it is a chronicle
        }
        else if (cmd == L"exportRejected") // not a chronicle file
        {
            g_exportSource.clear();
            g_exportContent.clear();
        }
        else if (cmd == L"exportAccepted") // step 2: where to save, then write the file
        {
            std::wstring target;
            bool picked = !g_exportSource.empty() && PickExportPath(hwnd, g_exportSource, target);
            bool ok = picked && BuildViewerFile(target, g_exportContent);
            g_exportSource.clear();
            g_exportContent.clear();
            g_exportContent.shrink_to_fit();
            PostToPage(!picked ? L"exportCancelled" : ok ? L"exported" : L"exportFailed");
        }
        else if (cmd == L"closeAsked")     // the page shows its closing window
        {
            KillTimer(hwnd, TIMER_CLOSE_WAIT);
        }
        else if (cmd == L"close")          // nothing to lose, or the user agreed
        {
            g_closeConfirmed = true;
            ShutdownBlockReasonDestroy(hwnd);
            StartFadeOut(hwnd);
        }
    }
}

// ---- WebView2 setup (manual COM callbacks — MinGW has no wrl/event.h or __uuidof) ----

void ResizeWebView(HWND hwnd);

// One reusable class for the WebView2 events below: it just calls a function.
template <typename TInterface, typename TSender, typename TArgs>
class CallbackHandler : public TInterface
{
    LONG m_refCount = 1;
    const IID& m_iid;
    void (*m_fn)(TArgs*);
public:
    CallbackHandler(const IID& iid, void (*fn)(TArgs*)) : m_iid(iid), m_fn(fn) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        return QueryOwnInterface(static_cast<TInterface*>(this), m_iid, riid, ppv);
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

    // Ctrl+S: the same as the "Zapisz" button. Holding the keys down
    // doesn't save again and again.
    if (key == 'S' && kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN
        && (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        && !(GetAsyncKeyState(VK_MENU) & 0x8000)
        && !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
    {
        args->put_Handled(TRUE);
        COREWEBVIEW2_PHYSICAL_KEY_STATUS status = {};
        if (SUCCEEDED(args->get_PhysicalKeyStatus(&status)) && !status.WasKeyDown)
            PostToPage(L"saveShortcut");
        return;
    }

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

// A message from the page. Only stored here; handled from the message loop.
static void OnWebMessageReceived(ICoreWebView2WebMessageReceivedEventArgs* args)
{
    LPWSTR text = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&text)) || !text) return;
    g_inbox.emplace_back(text);
    CoTaskMemFree(text);
    PostMessage(g_hwnd, WM_APP_PAGE_MESSAGE, 0, 0);
}

// The page itself is gone (its process ended): it can't answer
// "may I close?" any more, and its content is lost anyway, so closing
// must not wait for it. A crash of a helper process (e.g. graphics)
// leaves the page and its content intact, so it changes nothing.
static void OnProcessFailed(ICoreWebView2ProcessFailedEventArgs* args)
{
    COREWEBVIEW2_PROCESS_FAILED_KIND kind;
    if (FAILED(args->get_ProcessFailedKind(&kind))) return;
    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ||
        kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED)
        g_dirty = false;
}

class ControllerCompletedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
    HWND m_hwnd;
    LONG m_refCount = 1;
public:
    explicit ControllerCompletedHandler(HWND hwnd) : m_hwnd(hwnd) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        return QueryOwnInterface(
            static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this),
            IID_ControllerCompletedHandler_, riid, ppv);
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

            auto* keyHandler = new CallbackHandler<ICoreWebView2AcceleratorKeyPressedEventHandler,
                ICoreWebView2Controller, ICoreWebView2AcceleratorKeyPressedEventArgs>(IID_AcceleratorKeyHandler_, OnAcceleratorKeyPressed);
            g_webviewController->add_AcceleratorKeyPressed(keyHandler, &token);
            keyHandler->Release(); // WebView2 keeps its own reference

            auto* navHandler = new CallbackHandler<ICoreWebView2NavigationStartingEventHandler,
                ICoreWebView2, ICoreWebView2NavigationStartingEventArgs>(IID_NavigationStartingHandler_, OnNavigationStarting);
            g_webview->add_NavigationStarting(navHandler, &token);
            navHandler->Release();

            auto* popupHandler = new CallbackHandler<ICoreWebView2NewWindowRequestedEventHandler,
                ICoreWebView2, ICoreWebView2NewWindowRequestedEventArgs>(IID_NewWindowRequestedHandler_, OnNewWindowRequested);
            g_webview->add_NewWindowRequested(popupHandler, &token);
            popupHandler->Release();
            auto* msgHandler = new CallbackHandler<ICoreWebView2WebMessageReceivedEventHandler,
                ICoreWebView2, ICoreWebView2WebMessageReceivedEventArgs>(IID_WebMessageReceivedHandler_, OnWebMessageReceived);
            g_webview->add_WebMessageReceived(msgHandler, &token);
            msgHandler->Release();
            auto* failHandler = new CallbackHandler<ICoreWebView2ProcessFailedEventHandler,
                ICoreWebView2, ICoreWebView2ProcessFailedEventArgs>(IID_ProcessFailedHandler_, OnProcessFailed);
            g_webview->add_ProcessFailed(failHandler, &token);
            failHandler->Release();
            std::wstring htmlUrl = ToFileUrl(g_pageDir + L"flipbook.html");
            // Zapamiętane powiększenie: strona od razu startuje w tym rozmiarze.
            // Wersja tylko do przeglądania: sama książka (i suwak powiększenia).
            wchar_t zoomArg[48];
            wsprintfW(zoomArg, g_viewer ? L"?viewer=1&zoom=%d" : L"?zoom=%d", LoadSavedZoom());
            htmlUrl += zoomArg;
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

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        return QueryOwnInterface(
            static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this),
            IID_EnvironmentCompletedHandler_, riid, ppv);
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
        auto* handler = new ControllerCompletedHandler(m_hwnd);
        env->CreateCoreWebView2Controller(m_hwnd, handler);
        handler->Release(); // WebView2 keeps its own reference
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
    if (g_viewer) dataDir = dataDir + L"\\" + VIEWER_FOLDER_NAME;   // separate from the editor's
    auto* handler = new EnvironmentCompletedHandler(hwnd);
    HRESULT hr = g_createEnvironment(
        nullptr, dataDir.c_str(), nullptr, handler);
    handler->Release(); // WebView2 keeps its own reference

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
    HRESULT hr = g_getBrowserVersion(nullptr, &version);
    bool ok = SUCCEEDED(hr) && version != nullptr;
    if (version) CoTaskMemFree(version);
    return ok;
}

// ---------------------------------------------------------------------------

#ifndef ABM_GETAUTOHIDEBAREX
#define ABM_GETAUTOHIDEBAREX 0x0000000b
#endif

// Maximized: Windows pushes the window a few pixels past every screen edge
// (that is where the normal frame would be). The whole window is our client
// area, so cut it back to the monitor's work area.
static void FitMaximizedClient(RECT& rc)
{
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    rc = mi.rcWork;

    // Auto-hidden taskbar: a window covering the whole monitor would stop it
    // from sliding out, so leave 1 pixel free on the edge where it hides.
    APPBARDATA state = { sizeof(state) };
    if (!(SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE)) return;
    for (UINT edge = ABE_LEFT; edge <= ABE_BOTTOM; edge++)
    {
        APPBARDATA bar = { sizeof(bar) };
        bar.uEdge = edge;
        bar.rc = mi.rcMonitor;
        if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar)) continue;
        if (edge == ABE_LEFT)        rc.left += 1;
        else if (edge == ABE_TOP)    rc.top += 1;
        else if (edge == ABE_RIGHT)  rc.right -= 1;
        else                         rc.bottom -= 1;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_hwnd = hwnd;
        InitWebView(hwnd);
        return 0;

    case WM_APP_PAGE_MESSAGE:
        ProcessPageMessages(hwnd);
        return 0;

    // Druga kopia programu (dwuklik pliku w Eksploratorze) przysyła ścieżkę.
    case WM_COPYDATA:
    {
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lParam;
        if (g_viewer || !cds || cds->dwData != COPYDATA_OPEN_FILE || !cds->lpData
            || cds->cbData < sizeof(wchar_t))
            return FALSE;
        std::wstring path((const wchar_t*)cds->lpData, cds->cbData / sizeof(wchar_t));
        while (!path.empty() && path.back() == L'\0') path.pop_back();
        if (path.empty()) return FALSE;
        g_externalOpenPath = path;
        if (g_pageReady) PostToPage(L"externalOpen");   // inaczej zrobi to "pageReady"
        return TRUE;
    }

    // Zero-size non-client frame: DWM still treats this as a normal
    // captioned window (native animations), but nothing is drawn there.
    // Maximized: the client area is cut back to the screen (see above).
    case WM_NCCALCSIZE:
        if (wParam == TRUE)
        {
            if (IsZoomed(hwnd))
                FitMaximizedClient(((NCCALCSIZE_PARAMS*)lParam)->rgrc[0]);
            return 0;
        }
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
        // Minimized: leave the WebView at its size. Otherwise the page would
        // rebuild the book for a tiny window and show that for a moment
        // after coming back.
        if (wParam != SIZE_MINIMIZED) ResizeWebView(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE); // the caption buttons move with the right edge
        break;

    // The window moved: WebView2 must know, or its pop-ups (e.g. the font
    // list) open in the old place.
    case WM_MOVE:
        if (g_webviewController) g_webviewController->NotifyParentWindowPositionChanged();
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
        if (cmd == SC_KEYMENU || cmd == SC_MOUSEMENU) return 0;
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
        if (wParam == TIMER_CLOSE_WAIT)
        {
            // The page didn't answer (still loading, failed to start, or hung).
            KillTimer(hwnd, TIMER_CLOSE_WAIT);
            if (g_dirty)
            {
                // There is something to lose: only the user may decide.
                int answer = MessageBoxW(hwnd,
                    L"Program nie odpowiada.\n\n"
                    L"Zamkn\u0105\u0107 go mimo to? Niezapisane zmiany zostan\u0105 utracone.",
                    APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                if (g_closeConfirmed) return 0;   // the page answered meanwhile
                if (answer != IDYES) return 0;
            }
            g_closeConfirmed = true;
            StartFadeOut(hwnd);
            return 0;
        }
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

    // Closing (X button, Alt+F4, taskbar): first the page checks whether
    // the content is in a file. If not, it shows its own window and
    // answers "close" only when the user agrees.
    case WM_CLOSE:
        if (!g_closeConfirmed && g_webview)
        {
            PostToPage(L"confirmClose");
            SetTimer(hwnd, TIMER_CLOSE_WAIT, CLOSE_WAIT_MS, nullptr);
            return 0;
        }
        StartFadeOut(hwnd);
        return 0;
    // Windows wants to end the session (shutdown, restart, log off). If the
    // content is not in a file, ask Windows to wait and show the page's
    // closing window.
    case WM_QUERYENDSESSION:
        if (g_dirty && !g_closeConfirmed && g_webview)
        {
            ShutdownBlockReasonCreate(hwnd, L"Zawarto\u015b\u0107 kroniki nie jest zapisana w pliku.");
            PostToPage(L"confirmClose");
            return FALSE;
        }
        return TRUE;

    case WM_ENDSESSION:
        if (wParam)
        {
            KillTimer(hwnd, TIMER_FADE_IN);
            KillTimer(hwnd, TIMER_FADE_OUT);
            CloseWebView();
            DestroyWindow(hwnd);
        }
        else
        {
            ShutdownBlockReasonDestroy(hwnd);   // shutdown was cancelled
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

    // (A view-only copy is not limited: any number of them may be open.)
    std::vector<PackedFile> packedFiles;
    std::string packedDoc;
    PackageState package = ReadPackage(packedFiles, packedDoc);
    g_viewer = (package != PACKAGE_NONE);

    // Plik podany przy uruchomieniu (dwuklik pliku .kronika w Eksploratorze).
    std::wstring openPath;
    if (!g_viewer)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2 && argv[1][0])
        {
            wchar_t full[MAX_PATH * 4];
            const DWORD cap = (DWORD)(sizeof(full) / sizeof(full[0]));
            DWORD n = GetFullPathNameW(argv[1], cap, full, nullptr);
            openPath = (n > 0 && n < cap) ? full : argv[1];
        }
        if (argv) LocalFree(argv);
    }

    HANDLE instanceMutex = g_viewer ? nullptr : CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND running = FindWindowW(CLASS_NAME, nullptr);
        if (running)
        {
            if (IsIconic(running)) ShowWindow(running, SW_RESTORE);
            SetForegroundWindow(running);
            // Plik z Eksploratora: otwiera go program, który już działa.
            if (!openPath.empty())
            {
                COPYDATASTRUCT cds = { COPYDATA_OPEN_FILE,
                    (DWORD)((openPath.size() + 1) * sizeof(wchar_t)), (PVOID)openPath.c_str() };
                SendMessageTimeoutW(running, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, nullptr);
            }
        }
        CloseHandle(instanceMutex);
        return 0;
    }
    g_externalOpenPath = openPath;          // otworzy się, gdy strona będzie gotowa
    if (!g_viewer) RegisterFileType();      // dwuklik pliku .kronika otwiera ten program

    // Where flipbook.html and WebView2Loader.dll are: next to app.exe, or
    // (view-only copy) in the folder they were just unpacked to.
    g_pageDir = GetExeDir();
    if (g_viewer)
    {
        if (package != PACKAGE_OK || !UnpackViewerFiles(packedFiles, g_pageDir))
        {
            MessageBoxW(nullptr,
                L"Ten plik jest uszkodzony albo nie uda\u0142o si\u0119 przygotowa\u0107 go do uruchomienia.\n\n"
                L"Kronika nie mo\u017ce zosta\u0107 wy\u015bwietlona.",
                APP_TITLE, MB_OK | MB_ICONERROR);
            return 0;
        }
        g_viewerDoc = FromUtf8(packedDoc);
        packedDoc.clear();
        packedDoc.shrink_to_fit();
        packedFiles.clear();
        g_closeConfirmed = true;   // nothing can be lost: closing never asks
    }

    if (!LoadWebView2Loader(g_pageDir + LOADER_DLL_NAME))
    {
        MessageBoxW(nullptr,
            L"Nie uda\u0142o si\u0119 wczyta\u0107 pliku WebView2Loader.dll.\n\n"
            L"Kronika szkolna nie mo\u017ce si\u0119 uruchomi\u0107.",
            APP_TITLE, MB_OK | MB_ICONERROR);
        if (instanceMutex) { ReleaseMutex(instanceMutex); CloseHandle(instanceMutex); }
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

    if (FileMissing(g_pageDir + L"flipbook.html"))
    {
        MessageBoxW(nullptr,
            L"Brakuje pliku flipbook.html, kt\u00f3ry musi le\u017ce\u0107 w tym samym folderze co program.\n\n"
            L"Kronika szkolna nie mo\u017ce si\u0119 uruchomi\u0107.",
            APP_TITLE, MB_OK | MB_ICONERROR);
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
    wc.lpszClassName = g_viewer ? VIEWER_CLASS_NAME : CLASS_NAME;
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

    HWND hwnd = CreateWindowExW(exStyle, g_viewer ? VIEWER_CLASS_NAME : CLASS_NAME, APP_TITLE, style,
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