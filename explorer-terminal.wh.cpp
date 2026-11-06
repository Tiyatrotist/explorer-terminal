// ==WindhawkMod==
// @id              explorer-terminal
// @name            Explorer Terminal
// @description     A real PowerShell/CMD console docked at the bottom of File Explorer, toggled with a hotkey and following the open folder
// @version         0.3.2
// @author          Tiyatrotist
// @include         explorer.exe
// @compilerOptions -lcomctl32 -lshlwapi -luuid -lole32 -loleaut32 -lgdi32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer Terminal

Press **Ctrl+J** (configurable) in a File Explorer window and a real console
slides in at the bottom of that same window. Press it again, or click the **x**
on its title strip, to hide it; the session keeps running. Type `exit` to end it.

* The terminal is a normal `conhost` console (PowerShell by default), so your
  PowerShell profile, `git` and copy/paste behave as usual.
* **Drag the title strip** up or down to resize the terminal. The height is
  remembered.
* With PowerShell, the terminal follows the folder you open in Explorer: it runs
  `cd` when the prompt is idle, without touching a line you are typing or a
  command that is running.
* One terminal per Explorer window.

## Typing
The terminal is active right after it opens and when you click inside it; the title
strip says "typing here". While it is active your keys go to the terminal, not to
Explorer. Click the file list (or press the hotkey to hide the terminal) and the keys
go back to Explorer. Alt+Tab, Alt+F4, the Windows key and Ctrl+Esc always work as usual.
Input typed in the first seconds, while PowerShell is still starting, can be lost.

## Look
* **Like your Windows Terminal** (default): takes the color scheme and font of
  your Windows Terminal profile (`settings.json`) and applies them to the
  console. Only schemes defined in your `settings.json` (and Campbell) are found;
  if Windows Terminal is not set up, the next option is used instead.
* **Like PowerShell from the Start menu**: uses the colors and font set in the
  console's Properties (the classic blue by default).
* **Plain console**: Windows' default console colors.

The console is still the classic one, so Windows Terminal details such as the
text padding, the bar cursor and tabs are not reproduced.

## Troubleshooting
The mod logs to `%TEMP%\explorer-terminal.log`: when the terminal is created, which
folder it follows, and any problem handing keys to the terminal. If the prompt stays
in `C:\Windows\System32`, Explorer is showing Home or This PC, which are not folders.
If typing goes to Explorer, click inside the terminal until the strip says "typing here".

## Known limits
* Folder following needs PowerShell; with Command Prompt the terminal only
  starts in the current folder.
* The hotkey is read by a low-level keyboard hook and only acts while a File
  Explorer window is the foreground window.
* Written against the Windows 11 File Explorer window layout
  (`CabinetWClass` > `ShellTabWindowClass`); a future Explorer update may need
  a fix.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hotkey: ctrl-j
  $name: Hotkey (while File Explorer is active)
  $options:
  - ctrl-j: Ctrl+J
  - f12: F12
  - ctrl-backtick: Ctrl+` (key left of 1 on a US keyboard)
- shell: powershell
  $name: Shell
  $options:
  - powershell: Windows PowerShell
  - pwsh: PowerShell 7 (pwsh.exe)
  - cmd: Command Prompt
- look: terminal
  $name: Look
  $options:
  - terminal: Like your Windows Terminal (its color scheme and font)
  - console: Like PowerShell from the Start menu (console Properties colors and font)
  - plain: Plain console (Windows defaults)
- height: 260
  $name: Terminal height
  $description: In pixels at 100% display scaling. Dragging the title strip overrides it until you change this value.
- follow: true
  $name: Follow the folder open in Explorer
  $description: PowerShell only.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <commctrl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr UINT WM_APP_TOGGLE = WM_APP + 1;  // lParam: Explorer window
constexpr UINT WM_APP_DEAD = WM_APP + 2;    // lParam: Explorer window gone
constexpr UINT WM_APP_CLICK = WM_APP + 3;   // wParam/lParam: screen x/y of a left click
constexpr UINT WM_APP_KEY = WM_APP + 4;     // lParam: KeyMsg* (the worker deletes it)
// Thread timers (no window) get their id from the system, whatever id we ask for, so the
// id is stored when the timer is created.
UINT_PTR g_tickTimer = 0;
constexpr UINT kTickMs = 500;
constexpr int kBarDp = 24;       // title strip height at 100% scaling
constexpr int kCloseDp = 36;     // close button width
constexpr int kMinTermDp = 80;   // smallest terminal
constexpr int kKeepExplorerDp = 200;  // room always left for Explorer's own content
constexpr wchar_t kTabClass[] = L"ShellTabWindowClass";
constexpr wchar_t kCabClass[] = L"CabinetWClass";
constexpr wchar_t kBarClass[] = L"ExplorerTerminalBar";
// SID_STopLevelBrowser
constexpr GUID kSidTopLevelBrowser = {
    0x4C96BE40, 0x915C, 0x11CF, {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};

std::atomic<int> g_hotkeyVk{'J'};
std::atomic<bool> g_hotkeyCtrl{true};
std::atomic<const wchar_t*> g_hotkeyName{L"Ctrl+J"};
std::atomic<int> g_heightLogical{260};
std::atomic<int> g_shell{0};       // 0 powershell, 1 pwsh, 2 cmd
std::atomic<int> g_appearance{0};  // 0 Windows Terminal scheme, 1 console properties, 2 plain
std::atomic<bool> g_follow{true};

DWORD g_workerTid = 0;
DWORD g_hookTid = 0;
HANDLE g_workerThread = nullptr;
HANDLE g_hookThread = nullptr;
HINSTANCE g_hInst = nullptr;
UINT g_msgMakeBar = 0;

// One per Explorer window that has (or had) a terminal. The map is touched only
// by the worker thread. The subclass procedures and the strip run on Explorer's
// UI thread and read only `cab`, `con`, `bar` and `shown`.
struct Term {
    HWND cab = nullptr;
    HWND con = nullptr;
    HWND bar = nullptr;      // title strip, owned by Explorer's UI thread
    HANDLE hProc = nullptr;  // conhost.exe
    std::atomic<bool> shown{false};
    bool dead = false;
    bool follow = false;
    bool folderWarned = false;  // "folder not found" is logged once per streak
    DWORD shellPid = 0;         // the shell (a client of the console), not conhost
    std::atomic<bool> active{false};  // typed keys go to this console (see KeyboardProc)
    // UI thread only (strip):
    bool closeHot = false;
    bool closePressed = false;
    bool dragging = false;
    std::wstring cwdFile;
    std::wstring lastPath;
    std::set<HWND> tabs;
};
std::map<HWND, Term*> g_terms;
std::atomic<bool> g_attached{false};  // this process is briefly attached to a shell's console
std::atomic<bool> g_anyShown{false};  // lets the mouse hook skip clicks when no terminal is open
// The terminal that receives typed keys: its Explorer window and console, or null. Written by
// the worker, read by the keyboard hook.
std::atomic<int> g_keyLogBudget{0};  // how many forwarded keys are still logged after an activation
std::atomic<HWND> g_activeCab{nullptr};
std::atomic<HWND> g_activeCon{nullptr};

// One key as the keyboard hook saw it, with everything the console needs.
struct KeyMsg {
    HWND con;
    WORD vk, scan;
    DWORD state;  // dwControlKeyState
    WCHAR ch;
    bool down;
};

// Windhawk's log is only visible while its log viewer is open, so the same lines also
// go to %TEMP%\explorer-terminal.log (emptied when it grows large) for bug reports.
void Dbg(PCWSTR fmt, ...) {
    wchar_t msg[480];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(msg, ARRAYSIZE(msg) - 1, fmt, a);
    va_end(a);
    msg[ARRAYSIZE(msg) - 1] = 0;
    Wh_Log(L"%s", msg);

    wchar_t path[MAX_PATH];
    if (!GetTempPathW(ARRAYSIZE(path), path)) return;
    wcscat_s(path, L"explorer-terminal.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[540];
    int n = _snwprintf(line, ARRAYSIZE(line) - 1, L"%02d:%02d:%02d.%03d [%lu] %s\r\n", st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds, GetCurrentProcessId(), msg);
    char u8[1700];
    int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof(u8), nullptr, nullptr) : 0;
    DWORD w;
    if (len > 0) WriteFile(h, u8, len, &w, nullptr);
    CloseHandle(h);
}

bool IsClass(HWND h, PCWSTR cls) {
    wchar_t buf[64];
    return GetClassNameW(h, buf, ARRAYSIZE(buf)) && !wcscmp(buf, cls);
}

int Scale(HWND w, int dp) {
    UINT dpi = GetDpiForWindow(w);
    return MulDiv(dp, dpi ? dpi : 96, 96);
}

int BarPx(HWND cab) {
    return Scale(cab, kBarDp);
}

// Terminal height in pixels for a window whose client area is `clientH` tall.
int TermPx(HWND cab, int clientH) {
    int lo = Scale(cab, kMinTermDp);
    int hi = std::max(lo, clientH - BarPx(cab) - Scale(cab, kKeepExplorerDp));
    return std::clamp(Scale(cab, g_heightLogical), lo, hi);
}

std::vector<HWND> TabsOf(HWND cab) {
    std::vector<HWND> v;
    for (HWND c = FindWindowExW(cab, nullptr, kTabClass, nullptr); c;
         c = FindWindowExW(cab, c, kTabClass, nullptr)) {
        v.push_back(c);
    }
    return v;
}

HWND VisibleTab(HWND cab) {
    for (HWND tab : TabsOf(cab)) {
        if (IsWindowVisible(tab)) return tab;
    }
    return nullptr;
}

// Keeps Explorer's content area above the terminal and parks the title strip and
// the console in the reserved space. Safe to call from any thread.
void Relayout(Term* t) {
    if (t->dead || !IsWindow(t->cab) || IsIconic(t->cab)) return;
    RECT cr;
    GetClientRect(t->cab, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;

    int barH = BarPx(t->cab);
    int h = t->shown ? TermPx(t->cab, cr.bottom) : 0;
    int reserved = t->shown ? barH + h : 0;
    for (HWND tab : TabsOf(t->cab)) {
        RECT r;
        GetWindowRect(tab, &r);
        MapWindowPoints(nullptr, t->cab, (POINT*)&r, 2);
        int newH = std::max<int>(0, cr.bottom - reserved - r.top);
        if (newH != r.bottom - r.top) {
            SetWindowPos(tab, nullptr, r.left, r.top, r.right - r.left, newH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (t->bar && IsWindow(t->bar)) {
        if (!t->shown) {
            if (IsWindowVisible(t->bar)) ShowWindow(t->bar, SW_HIDE);
        } else {
            RECT r;
            GetWindowRect(t->bar, &r);
            MapWindowPoints(nullptr, t->cab, (POINT*)&r, 2);
            int y = cr.bottom - reserved;
            if (!IsWindowVisible(t->bar) || r.left != 0 || r.top != y ||
                r.right - r.left != cr.right || r.bottom - r.top != barH) {
                SetWindowPos(t->bar, nullptr, 0, y, cr.right, barH,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
    }

    if (h && t->con && IsWindow(t->con)) {
        RECT r;
        GetWindowRect(t->con, &r);
        MapWindowPoints(nullptr, t->cab, (POINT*)&r, 2);
        int y = cr.bottom - h;
        if (r.left != 0 || r.top != y || r.right - r.left != cr.right ||
            r.bottom - r.top != h) {
            // Async: the console belongs to another process, never block Explorer's UI on it.
            SetWindowPos(t->con, nullptr, 0, y, cr.right, h,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    }
}

// ---- Title strip: resize handle and close button -------------------------

void PaintBar(HWND h, HDC dc, Term* t) {
    RECT rc;
    GetClientRect(h, &rc);
    DWORD light = 0, sz = sizeof(light);
    bool dark = !(RegGetValueW(HKEY_CURRENT_USER,
                               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz) == ERROR_SUCCESS &&
                  light);
    COLORREF bg = dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
    COLORREF fg = dark ? RGB(224, 224, 224) : RGB(32, 32, 32);
    COLORREF line = dark ? RGB(72, 72, 72) : RGB(200, 200, 200);

    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &rc, b);
    DeleteObject(b);
    RECT ln = rc;
    ln.bottom = ln.top + 1;
    b = CreateSolidBrush(line);
    FillRect(dc, &ln, b);
    DeleteObject(b);

    UINT dpi = GetDpiForWindow(h);
    if (!dpi) dpi = 96;
    HFONT small = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT big = CreateFontW(-MulDiv(15, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SetBkMode(dc, TRANSPARENT);

    RECT cl = rc;
    cl.left = rc.right - Scale(h, kCloseDp);
    bool hot = t && t->closeHot;
    if (hot) {
        b = CreateSolidBrush(RGB(196, 43, 28));
        FillRect(dc, &cl, b);
        DeleteObject(b);
    }
    HGDIOBJ old = SelectObject(dc, big);
    SetTextColor(dc, hot ? RGB(255, 255, 255) : fg);
    DrawTextW(dc, L"\u00D7", -1, &cl, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, small);
    SetTextColor(dc, fg);
    RECT tx = rc;
    tx.left += Scale(h, 10);
    tx.right = cl.left;
    std::wstring label = t && t->active ? L"Terminal  \u00B7  typing here" : L"Terminal  \u00B7  click inside to type";
    label += L"  \u00B7  drag to resize  \u00B7  ";
    label += g_hotkeyName.load();
    label += L" hides";
    DrawTextW(dc, label.c_str(), -1, &tx, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, old);
    DeleteObject(small);
    DeleteObject(big);
}

void DragTo(Term* t) {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(t->cab, &p);
    RECT cr;
    GetClientRect(t->cab, &cr);
    int barH = BarPx(t->cab);
    int lo = Scale(t->cab, kMinTermDp);
    int hi = std::max(lo, (int)cr.bottom - barH - Scale(t->cab, kKeepExplorerDp));
    int h = std::clamp<int>((int)cr.bottom - (int)p.y - barH / 2, lo, hi);
    UINT dpi = GetDpiForWindow(t->cab);
    g_heightLogical = MulDiv(h, 96, dpi ? dpi : 96);
    Relayout(t);
}

LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    Term* t = (Term*)GetWindowLongPtrW(h, GWLP_USERDATA);
    auto overClose = [&](LPARAM l) {
        RECT rc;
        GetClientRect(h, &rc);
        return GET_X_LPARAM(l) >= rc.right - Scale(h, kCloseDp);
    };
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lParam)->lpCreateParams);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintBar(h, dc, t);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            SetCursor(LoadCursorW(nullptr, overClose(MAKELPARAM(p.x, p.y)) ? IDC_ARROW : IDC_SIZENS));
            return TRUE;
        }
        case WM_MOUSEMOVE:
            if (!t) break;
            if (t->dragging) {
                DragTo(t);
            } else {
                bool hot = overClose(lParam);
                if (hot != t->closeHot) {
                    t->closeHot = hot;
                    InvalidateRect(h, nullptr, FALSE);
                }
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            if (t && t->closeHot) {
                t->closeHot = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_LBUTTONDOWN:
            if (!t) break;
            (overClose(lParam) ? t->closePressed : t->dragging) = true;
            SetCapture(h);
            return 0;
        case WM_LBUTTONUP: {
            if (!t) break;
            bool wasDrag = t->dragging;
            bool wasClose = t->closePressed && overClose(lParam);
            if (GetCapture() == h) ReleaseCapture();  // clears the flags via WM_CAPTURECHANGED
            if (wasDrag) {
                Wh_SetIntValue(L"savedHeight", g_heightLogical);
            } else if (wasClose) {
                PostThreadMessageW(g_workerTid, WM_APP_TOGGLE, 0, (LPARAM)t->cab);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (t) t->dragging = t->closePressed = false;
            break;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

// Clamps every size Explorer asks for on its content area, so a relayout by
// Explorer itself (tab switch, details pane, ...) cannot cover the terminal.
LRESULT CALLBACK TabProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR ref) {
    if (msg == WM_WINDOWPOSCHANGING) {
        Term* t = (Term*)ref;
        auto* p = (WINDOWPOS*)lParam;
        if (t->shown && !(p->flags & SWP_NOSIZE)) {
            RECT cr;
            GetClientRect(t->cab, &cr);
            int maxBottom = cr.bottom - BarPx(t->cab) - TermPx(t->cab, cr.bottom);
            int top = p->y;
            if (p->flags & SWP_NOMOVE) {
                RECT r;
                GetWindowRect(hWnd, &r);
                MapWindowPoints(nullptr, t->cab, (POINT*)&r, 2);
                top = r.top;
            }
            if (top + p->cy > maxBottom) p->cy = std::max<int>(0, maxBottom - top);
        }
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK CabProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR ref) {
    if (g_msgMakeBar && msg == g_msgMakeBar) {
        // Runs on Explorer's UI thread, so the strip is owned by the thread of its parent.
        Term* t = (Term*)lParam;
        t->bar = CreateWindowExW(0, kBarClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, hWnd,
                                 nullptr, g_hInst, t);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        PostThreadMessageW(g_workerTid, WM_APP_DEAD, 0, (LPARAM)hWnd);
    }
    LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);
    if (msg == WM_SIZE && wParam != SIZE_MINIMIZED) Relayout((Term*)ref);
    return r;
}

void EnsureSubclasses(Term* t) {
    for (auto it = t->tabs.begin(); it != t->tabs.end();) {
        it = IsWindow(*it) ? std::next(it) : t->tabs.erase(it);
    }
    for (HWND tab : TabsOf(t->cab)) {
        if (t->tabs.insert(tab).second) {
            WindhawkUtils::SetWindowSubclassFromAnyThread(tab, TabProc, (DWORD_PTR)t);
        }
    }
}

// ---- Folder of the active Explorer tab -----------------------------------

// Folder of one Explorer tab, via the shell's window list. Matches the tab
// through its IShellBrowser window; falls back to the first entry of the frame.
std::wstring FolderOfTab(HWND cab, HWND tab) {
    std::wstring result;
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw)))) {
        return result;
    }
    std::wstring fallback;
    long n = 0;
    sw->get_Count(&n);
    for (long i = 0; i < n && result.empty(); i++) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        IDispatch* d = nullptr;
        if (sw->Item(v, &d) != S_OK || !d) continue;

        IWebBrowser2* wb = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&wb)))) {
            SHANDLE_PTR frame = 0;
            BSTR url = nullptr;
            wb->get_HWND(&frame);
            if ((HWND)frame == cab && SUCCEEDED(wb->get_LocationURL(&url)) && url) {
                wchar_t path[MAX_PATH];
                DWORD len = ARRAYSIZE(path);
                if (SUCCEEDED(PathCreateFromUrlW(url, path, &len, 0))) {
                    bool exact = false;
                    IServiceProvider* sp = nullptr;
                    IShellBrowser* sb = nullptr;
                    if (tab && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&sp)))) {
                        if (SUCCEEDED(sp->QueryService(kSidTopLevelBrowser, IID_PPV_ARGS(&sb)))) {
                            HWND w = nullptr;
                            exact = SUCCEEDED(sb->GetWindow(&w)) && w == tab;
                            sb->Release();
                        }
                        sp->Release();
                    }
                    if (exact) {
                        result = path;
                    } else if (fallback.empty()) {
                        fallback = path;
                    }
                }
                SysFreeString(url);
            }
            wb->Release();
        }
        d->Release();
    }
    sw->Release();
    return result.empty() ? fallback : result;
}

std::wstring FolderOfActiveTab(HWND cab) {
    std::wstring p = FolderOfTab(cab, VisibleTab(cab));
    DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? p : L"";
}

void WriteCwd(const std::wstring& file, const std::wstring& path) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string u8(WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(), u8.data(), (int)u8.size(), nullptr, nullptr);
    DWORD w;
    WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(h);
}

std::wstring Base64Utf16(const std::wstring& s) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char* d = (const unsigned char*)s.data();
    size_t n = s.size() * sizeof(wchar_t);
    std::wstring o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += T[v >> 18 & 63];
        o += T[v >> 12 & 63];
        o += i + 1 < n ? T[v >> 6 & 63] : L'=';
        o += i + 2 < n ? T[v & 63] : L'=';
    }
    return o;
}

// Runs while PowerShell waits at its prompt (PSReadLine raises OnIdle), so it never
// interrupts a running command; InvokePrompt redraws the prompt, keeping typed text.
std::wstring InitScript(const std::wstring& cwdFile) {
    std::wstring f;
    for (wchar_t c : cwdFile) f += (c == L'\'') ? L"''" : std::wstring(1, c);
    // The path is embedded: $Event.MessageData is null for OnIdle events.
    return L"$null=Register-EngineEvent -SourceIdentifier PowerShell.OnIdle -Action {try{$f='" + f +
           L"';if(Test-Path -LiteralPath $f){"
           L"$p=([IO.File]::ReadAllText($f)).Trim();"
           L"if($p -and (Test-Path -LiteralPath $p -PathType Container) -and $p -ne (Get-Location).ProviderPath){"
           L"Set-Location -LiteralPath $p;try{[Microsoft.PowerShell.PSConsoleReadLine]::InvokePrompt()}catch{}}}}catch{}}";
}

// ---- Appearance: Windows Terminal color scheme ----------------------------
// Minimal JSON(C) reader, just enough for settings.json.

size_t SkipWs(const std::string& s, size_t i) {
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return i;
}

size_t EndOfString(const std::string& s, size_t i) {  // s[i] == '"'; index after the closing quote
    for (i++; i < s.size(); i++) {
        if (s[i] == '\\') {
            i++;
        } else if (s[i] == '"') {
            return i + 1;
        }
    }
    return s.size();
}

size_t EndOfValue(const std::string& s, size_t i) {
    i = SkipWs(s, i);
    if (i >= s.size()) return i;
    if (s[i] == '"') return EndOfString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); i++) {
            if (s[i] == '"') {
                i = EndOfString(s, i) - 1;
            } else if (s[i] == '{' || s[i] == '[') {
                depth++;
            } else if (s[i] == '}' || s[i] == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return s.size();
    }
    while (i < s.size() && !strchr(",}] \t\r\n", s[i])) i++;
    return i;
}

std::string Unquote(const std::string& v) {  // JSON string literal -> text; "" if not a string
    if (v.size() < 2 || v[0] != '"') return "";
    std::string o;
    for (size_t i = 1; i + 1 < v.size(); i++) {
        if (v[i] == '\\' && i + 2 < v.size()) i++;
        o += v[i];
    }
    return o;
}

// Removes // and /* */ comments (settings.json allows them) and a UTF-8 BOM.
std::string StripJsonc(const std::string& s) {
    std::string o;
    size_t i = (s.size() >= 3 && s.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
    for (; i < s.size(); i++) {
        if (s[i] == '"') {
            size_t e = EndOfString(s, i);
            o.append(s, i, e - i);
            i = e - 1;
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') i++;
            o += '\n';
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            i = (e == std::string::npos) ? s.size() : e + 1;
        } else {
            o += s[i];
        }
    }
    return o;
}

// fn(key, valueText) per member of a JSON object; stops when fn returns true.
template <class F>
void EachMember(const std::string& obj, F fn) {
    size_t i = SkipWs(obj, 0);
    if (i >= obj.size() || obj[i] != '{') return;
    for (i++;;) {
        i = SkipWs(obj, i);
        if (i >= obj.size() || obj[i] == '}') return;
        if (obj[i] == ',') {
            i++;
            continue;
        }
        if (obj[i] != '"') return;
        size_t ke = EndOfString(obj, i);
        std::string key = Unquote(obj.substr(i, ke - i));
        i = SkipWs(obj, ke);
        if (i >= obj.size() || obj[i] != ':') return;
        i = SkipWs(obj, i + 1);
        size_t ve = EndOfValue(obj, i);
        if (fn(key, obj.substr(i, ve - i))) return;
        i = ve;
    }
}

// fn(elementText) per element of a JSON array; stops when fn returns true.
template <class F>
void EachElement(const std::string& arr, F fn) {
    size_t i = SkipWs(arr, 0);
    if (i >= arr.size() || arr[i] != '[') return;
    for (i++;;) {
        i = SkipWs(arr, i);
        if (i >= arr.size() || arr[i] == ']') return;
        if (arr[i] == ',') {
            i++;
            continue;
        }
        size_t ve = EndOfValue(arr, i);
        if (fn(arr.substr(i, ve - i))) return;
        i = ve;
    }
}

std::string Member(const std::string& obj, const char* key) {
    std::string r;
    EachMember(obj, [&](const std::string& k, const std::string& v) {
        if (k != key) return false;
        r = v;
        return true;
    });
    return r;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// "colorScheme" is either a name or {"dark": name, "light": name}.
std::string SchemeName(const std::string& v) {
    if (!v.empty() && v[0] == '{') return Unquote(Member(v, "dark"));
    return Unquote(v);
}

bool ParseHex(const std::string& v, COLORREF& out) {
    std::string h = Unquote(v);
    unsigned r, g, b;
    if (h.size() != 7 || h[0] != '#' || sscanf(h.c_str() + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    out = RGB(r, g, b);
    return true;
}

struct Scheme {
    COLORREF c[16]{};  // console palette order: black, blue, green, cyan, red, purple, yellow, white, bright...
    COLORREF bg = 0, fg = 0;
    std::wstring face = L"Cascadia Mono";
    int fontPx = 16;
};

constexpr char kCampbell[] =
    R"({"background":"#0C0C0C","foreground":"#CCCCCC","black":"#0C0C0C","red":"#C50F1F","green":"#13A10E",)"
    R"("yellow":"#C19C00","blue":"#0037DA","purple":"#881798","cyan":"#3A96DD","white":"#CCCCCC",)"
    R"("brightBlack":"#767676","brightRed":"#E74856","brightGreen":"#16C60C","brightYellow":"#F9F1A5",)"
    R"("brightBlue":"#3B78FF","brightPurple":"#B4009E","brightCyan":"#61D6D6","brightWhite":"#F2F2F2"})";

// Color scheme and font of the Windows Terminal profile that matches `shell`.
bool SchemeFromText(const std::string& raw, int shell, Scheme& out) {
    std::string text = StripJsonc(raw);
    std::string profiles = Member(text, "profiles");
    std::string defaults = Member(profiles, "defaults");
    std::string scheme = SchemeName(Member(defaults, "colorScheme"));
    std::string face, size;
    auto readFont = [&](const std::string& obj) {
        std::string font = Member(obj, "font");
        std::string f = Unquote(Member(font, "face"));
        if (f.empty()) f = Unquote(Member(obj, "fontFace"));
        if (!f.empty()) face = f;
        std::string s = Member(font, "size");
        if (s.empty()) s = Member(obj, "fontSize");
        if (!s.empty()) size = s;
    };
    readFont(defaults);
    EachElement(Member(profiles, "list"), [&](const std::string& el) {
        std::string cmd = Lower(Unquote(Member(el, "commandline")));
        std::string name = Lower(Unquote(Member(el, "name")));
        bool hit = shell == 0   ? (cmd.find("windowspowershell") != std::string::npos || name == "windows powershell")
                   : shell == 1 ? (cmd.find("pwsh") != std::string::npos || name == "powershell")
                                : (cmd.find("cmd.exe") != std::string::npos || name == "command prompt");
        if (!hit) return false;
        std::string s = SchemeName(Member(el, "colorScheme"));
        if (!s.empty()) scheme = s;
        readFont(el);
        return true;
    });
    if (scheme.empty()) scheme = "Campbell";

    std::string obj;
    EachElement(Member(text, "schemes"), [&](const std::string& el) {
        if (Unquote(Member(el, "name")) != scheme) return false;
        obj = el;
        return true;
    });
    if (obj.empty()) {
        if (scheme != "Campbell") return false;  // built-in schemes other than Campbell are not bundled
        obj = kCampbell;
    }

    static const char* const names[16] = {"black",       "blue",       "green",       "cyan",
                                          "red",         "purple",     "yellow",      "white",
                                          "brightBlack", "brightBlue", "brightGreen", "brightCyan",
                                          "brightRed",   "brightPurple", "brightYellow", "brightWhite"};
    for (int k = 0; k < 16; k++) {
        if (!ParseHex(Member(obj, names[k]), out.c[k])) return false;
    }
    if (!ParseHex(Member(obj, "background"), out.bg) || !ParseHex(Member(obj, "foreground"), out.fg)) return false;
    if (!face.empty()) {
        out.face.assign(MultiByteToWideChar(CP_UTF8, 0, face.c_str(), (int)face.size(), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, face.c_str(), (int)face.size(), out.face.data(), (int)out.face.size());
    }
    if (!size.empty()) out.fontPx = std::clamp((int)(atof(size.c_str()) * 96 / 72 + 0.5), 8, 72);
    return true;
}

std::string ReadFileText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string s;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20)) {
        s.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, s.data(), (DWORD)s.size(), &got, nullptr)) got = 0;
        s.resize(got);
    }
    CloseHandle(h);
    return s;
}

bool LoadTerminalScheme(int shell, Scheme& out) {
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local))) return false;
    static const wchar_t* const rel[] = {
        L"\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json",
        L"\\Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json",
        L"\\Microsoft\\Windows Terminal\\settings.json"};
    for (const wchar_t* r : rel) {
        std::string text = ReadFileText(std::wstring(local) + r);
        if (!text.empty()) return SchemeFromText(text, shell, out);
    }
    return false;
}

// Sets palette and font of the console of process `pid` (the shell, not conhost).
// The scheme's background and foreground replace palette entries 0 and 7, the console's
// default colors, so existing text recolors too.
bool ApplyScheme(DWORD pid, const Scheme& s) {
    g_attached = true;
    if (!AttachConsole(pid)) {
        Dbg(L"scheme: AttachConsole(%lu) failed: %lu", pid, GetLastError());
        g_attached = false;
        return false;
    }
    bool ok = false;
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFOEX i{};
        i.cbSize = sizeof(i);
        if (GetConsoleScreenBufferInfoEx(out, &i)) {
            for (int k = 0; k < 16; k++) i.ColorTable[k] = s.c[k];
            i.ColorTable[0] = s.bg;
            i.ColorTable[7] = s.fg;
            i.wAttributes = (i.wAttributes & ~0xFF) | 0x07;
            i.srWindow.Right++;  // Get/SetConsoleScreenBufferInfoEx shrink the window by one otherwise
            i.srWindow.Bottom++;
            ok = !!SetConsoleScreenBufferInfoEx(out, &i);
        }
        CONSOLE_FONT_INFOEX f{};
        f.cbSize = sizeof(f);
        f.dwFontSize.Y = (SHORT)s.fontPx;
        f.FontFamily = 54;  // TMPF_TRUETYPE | fixed pitch, like Windows' own saved consoles
        f.FontWeight = FW_NORMAL;
        wcsncpy(f.FaceName, s.face.c_str(), LF_FACESIZE - 1);
        SetCurrentConsoleFontEx(out, FALSE, &f);
        CloseHandle(out);
    }
    if (!ok) Dbg(L"scheme: setting the palette failed: %lu", GetLastError());
    FreeConsole();
    g_attached = false;
    return ok;
}

// ---- Console process and window -------------------------------------------

struct ConsoleSearch {
    std::vector<DWORD>* pids;
    HWND found;
};

// A named CALLBACK, not a lambda: Windhawk also builds 32-bit, where a lambda does
// not convert to the stdcall WNDENUMPROC.
BOOL CALLBACK MatchConsoleWindow(HWND h, LPARAM l) {
    auto* c = (ConsoleSearch*)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (std::find(c->pids->begin(), c->pids->end(), pid) != c->pids->end() &&
        IsClass(h, L"ConsoleWindowClass")) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}

// The shell: the first child process of the conhost.exe we started.
DWORD ChildOf(DWORD parent) {
    DWORD child = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok && !child; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ParentProcessID == parent) child = pe.th32ProcessID;
        }
        CloseHandle(snap);
    }
    return child;
}

HWND FindConsoleWindow(DWORD conhostPid) {
    // The console window is reported as owned by the client process (powershell), not conhost.
    std::vector<DWORD> pids{conhostPid};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ParentProcessID == conhostPid) pids.push_back(pe.th32ProcessID);
        }
        CloseHandle(snap);
    }
    ConsoleSearch ctx{&pids, nullptr};
    EnumWindows(MatchConsoleWindow, (LPARAM)&ctx);
    return ctx.found;
}

// Windows keeps the properties a user sets on a console (Properties dialog, colors,
// font) under HKCU\Console\<title of the console>, where the title of a console
// started from the shell is the path of the program. conhost.exe started directly
// has its own path as title, so we give it the shell's path to get the same settings.
std::wstring ConsoleTitleFor(int shell) {
    wchar_t buf[MAX_PATH];
    if (shell == 1) {
        return SearchPathW(nullptr, L"pwsh.exe", nullptr, ARRAYSIZE(buf), buf, nullptr) ? buf : L"";
    }
    GetWindowsDirectoryW(buf, ARRAYSIZE(buf));
    return std::wstring(buf) + (shell == 2 ? L"\\System32\\cmd.exe"
                                           : L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
}

// ---- Typing -----------------------------------------------------------------
// The console is another process's window and Windows refuses to move the keyboard focus
// into it (SetFocus fails with "access denied", also after joining its input queue; measured
// in tests/self-check.cpp). So the focus stays in Explorer and, while a terminal is active,
// the keyboard hook takes the typed keys and the worker writes them into the console's input
// buffer, exactly as a keyboard would. Active = shown and last clicked (or just opened).

// While Explorer is attached to a console, a closing console must not take Explorer with it.
BOOL WINAPI IgnoreConsoleEvents(DWORD) {
    return g_attached;
}

void SetActive(Term* t, bool on) {
    if (on) {
        for (auto& [cab, o] : g_terms) {
            if (o != t && o->active) {
                o->active = false;
                if (o->bar && IsWindow(o->bar)) InvalidateRect(o->bar, nullptr, FALSE);
            }
        }
        g_activeCab = t->cab;
        g_activeCon = t->con;
    } else if (g_activeCon == t->con) {
        g_activeCon = nullptr;
        g_activeCab = nullptr;
    }
    if (t->active != on) {
        Dbg(L"terminal %s", on ? L"active (keys go to it)" : L"inactive (keys go to Explorer)");
        g_keyLogBudget = on ? 4 : 0;
        t->active = on;
        if (t->bar && IsWindow(t->bar)) InvalidateRect(t->bar, nullptr, FALSE);
    }
}

// A left click: inside a console it becomes the active one; on the files it stops being.
void ActivateOnClick(POINT pt) {
    for (auto& [cab, t] : g_terms) {
        if (!t->shown) continue;
        RECT r;
        static int logged = 0;
        if (logged < 8 && GetWindowRect(t->con, &r)) {
            logged++;
            Dbg(L"click (%ld,%ld), console rect (%ld,%ld,%ld,%ld): %s", pt.x, pt.y, r.left, r.top, r.right, r.bottom,
                PtInRect(&r, pt) ? L"inside" : L"outside");
        }
        if (GetWindowRect(t->con, &r) && PtInRect(&r, pt)) {
            SetActive(t, true);
        } else if (t->active && !(t->bar && GetWindowRect(t->bar, &r) && PtInRect(&r, pt))) {
            SetActive(t, false);
        }
    }
}

INPUT_RECORD KeyRecord(WORD vk, WORD scan, WCHAR ch, DWORD state, bool down) {
    INPUT_RECORD r{};
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = down;
    r.Event.KeyEvent.wRepeatCount = 1;
    r.Event.KeyEvent.wVirtualKeyCode = vk;
    r.Event.KeyEvent.wVirtualScanCode = scan;
    r.Event.KeyEvent.uChar.UnicodeChar = ch;
    r.Event.KeyEvent.dwControlKeyState = state;
    return r;
}

// Ctrl+V for a console that is reading a line itself (cmd): the window would paste, the input
// buffer does not, so the clipboard text is typed in. PSReadLine pastes on its own.
void PasteInto(HANDLE in) {
    if (!OpenClipboard(nullptr)) return;
    std::vector<INPUT_RECORD> rec;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* s = (const wchar_t*)GlobalLock(h)) {
            // shortcut: at most 20000 characters per paste, enough for a command line
            for (size_t i = 0; s[i] && rec.size() < 40000; i++) {
                if (s[i] == L'\n' && i > 0 && s[i - 1] == L'\r') continue;
                bool enter = s[i] == L'\r' || s[i] == L'\n';
                WCHAR ch = enter ? L'\r' : s[i];
                rec.push_back(KeyRecord(enter ? VK_RETURN : 0, 0, ch, 0, true));
                rec.push_back(KeyRecord(enter ? VK_RETURN : 0, 0, ch, 0, false));
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    DWORD n;
    if (!rec.empty()) WriteConsoleInputW(in, rec.data(), (DWORD)rec.size(), &n);
}

void TypeKey(const KeyMsg& k) {
    Term* t = nullptr;
    for (auto& [cab, o] : g_terms) {
        if (o->con == k.con) t = o;
    }
    if (!t || !t->shown || !t->shellPid) return;

    g_attached = true;  // armed before the attach
    BOOL attached = AttachConsole(t->shellPid);
    // The first attach after a pause is sometimes refused (error 1, measured); it works a moment later.
    for (int i = 0; i < 5 && !attached; i++) {
        Sleep(20);
        attached = AttachConsole(t->shellPid);
    }
    if (!attached) {
        DWORD err = GetLastError();
        wchar_t exe[MAX_PATH] = L"(gone)";
        DWORD n = ARRAYSIZE(exe);
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, t->shellPid)) {
            QueryFullProcessImageNameW(h, 0, exe, &n);
            CloseHandle(h);
        }
        Dbg(L"typing: AttachConsole(%lu = %s) failed: %lu", t->shellPid, exe, err);
        g_attached = false;
        return;
    }
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        GetConsoleMode(in, &mode);
        bool ctrl = k.state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
        bool alt = k.state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
        bool shift = k.state & SHIFT_PRESSED;
        if (k.down && k.vk == 'C' && ctrl && !alt && !shift && (mode & ENABLE_PROCESSED_INPUT)) {
            // A running command: Ctrl+C has to be a signal, and written as a key it is ignored
            // (measured). The console turns the key message into the signal itself, reading Ctrl from
            // the keyboard state it shares with Explorer. GenerateConsoleCtrlEvent is no option: it
            // reached the console this process was started from instead (measured).
            PostMessageW(k.con, WM_KEYDOWN, 'C', 1 | (LPARAM)k.scan << 16);
            PostMessageW(k.con, WM_KEYUP, 'C', 1 | (LPARAM)k.scan << 16 | 3u << 30);
        } else if (k.down && k.vk == 'V' && ctrl && !alt && (mode & ENABLE_LINE_INPUT)) {
            PasteInto(in);
        } else {
            INPUT_RECORD r = KeyRecord(k.vk, k.scan, k.ch, k.state, k.down);
            DWORD n;
            BOOL wrote = WriteConsoleInputW(in, &r, 1, &n);
            if (!wrote) Dbg(L"typing: WriteConsoleInput failed: %lu", GetLastError());
            if (g_keyLogBudget > 0 && k.down) {
                g_keyLogBudget--;
                // never the key itself: the log must not capture what is typed (passwords)
                Dbg(L"a key was written to the console of pid %lu: %d, mode 0x%lx", t->shellPid, wrote, mode);
            }
        }
        CloseHandle(in);
    } else {
        Dbg(L"typing: CONIN$ failed: %lu", GetLastError());
    }
    FreeConsole();
    g_attached = false;
}

void Release(Term* t, bool windowAlive) {
    if (windowAlive) {
        t->shown = false;
        Relayout(t);
        if (t->bar && IsWindow(t->bar)) SendMessageW(t->bar, WM_CLOSE, 0, 0);  // destroyed on its own thread
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(t->cab, CabProc);
        for (HWND tab : t->tabs) {
            if (IsWindow(tab)) WindhawkUtils::RemoveWindowSubclassFromAnyThread(tab, TabProc);
        }
    }
    SetActive(t, false);
    t->dead = true;
    if (t->hProc) {
        TerminateProcess(t->hProc, 0);  // no-op when the shell already exited
        CloseHandle(t->hProc);
    }
    DeleteFileW(t->cwdFile.c_str());
    g_terms.erase(t->cab);
    delete t;
}

Term* Create(HWND cab) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, ARRAYSIZE(sys));
    int shell = g_shell;
    int appearance = g_appearance;
    std::wstring dir = FolderOfActiveTab(cab);

    // The Windows Terminal look needs a readable settings.json; without one use the
    // console's own Properties, which is what a PowerShell from the Start menu shows.
    Scheme scheme;
    bool useScheme = appearance == 0 && LoadTerminalScheme(shell, scheme);
    if (appearance == 0 && !useScheme) {
        Dbg(L"Windows Terminal settings or color scheme not found, using the console settings");
        appearance = 1;
    }

    auto* t = new Term();
    t->cab = cab;
    t->follow = g_follow && shell != 2;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(ARRAYSIZE(tmp), tmp);
    t->cwdFile = std::wstring(tmp) + L"explorer-terminal-" + std::to_wstring((UINT_PTR)cab) + L".txt";
    WriteCwd(t->cwdFile, dir);
    t->lastPath = dir;

    // Explicit conhost.exe: a classic console window we can reparent, never Windows Terminal.
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\conhost.exe\" ";
    if (shell == 2) {
        cmd += L"cmd.exe";
    } else {
        cmd += (shell == 1 ? L"pwsh.exe" : L"powershell.exe");
        cmd += L" -NoExit";
        if (t->follow) cmd += L" -EncodedCommand " + Base64Utf16(InitScript(t->cwdFile));
    }
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    std::vector<wchar_t> title;
    if (appearance == 1) {
        std::wstring ts = ConsoleTitleFor(shell);
        if (!ts.empty()) title.assign(ts.begin(), ts.end()), title.push_back(0);
    }
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!title.empty()) si.lpTitle = title.data();
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        Dbg(L"CreateProcess failed: %u", GetLastError());
        DeleteFileW(t->cwdFile.c_str());
        delete t;
        return nullptr;
    }
    CloseHandle(pi.hThread);
    t->hProc = pi.hProcess;

    HWND con = nullptr;
    for (int i = 0; i < 80 && !con; i++) {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(100);
        con = FindConsoleWindow(pi.dwProcessId);
    }
    if (!con) {
        Dbg(L"console window not found (shell missing or exited)");
        Release(t, false);
        return nullptr;
    }
    t->con = con;

    t->shellPid = ChildOf(pi.dwProcessId);
    if (!t->shellPid) GetWindowThreadProcessId(con, &t->shellPid);
    if (useScheme) {
        // The shell may not be ready for AttachConsole the moment its window exists.
        bool ok = false;
        for (int i = 0; i < 20 && !ok; i++) {
            ok = ApplyScheme(t->shellPid, scheme);
            if (!ok) Sleep(100);
        }
        if (!ok) Dbg(L"could not apply the Windows Terminal color scheme (shell pid %lu)", t->shellPid);
    }

    SetParent(con, cab);
    LONG_PTR st = GetWindowLongPtrW(con, GWL_STYLE);
    st &= ~(LONG_PTR)(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    st |= WS_CHILD | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(con, GWL_STYLE, st);
    LONG_PTR ex = GetWindowLongPtrW(con, GWL_EXSTYLE);
    ex &= ~(LONG_PTR)(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(con, GWL_EXSTYLE, ex);

    t->shown = true;
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(cab, CabProc, (DWORD_PTR)t)) {
        Dbg(L"could not subclass the Explorer window");
        Release(t, false);
        return nullptr;
    }
    SendMessageW(cab, g_msgMakeBar, 0, (LPARAM)t);
    EnsureSubclasses(t);
    SetWindowPos(con, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    Relayout(t);
    return t;
}

void UpdateAnyShown() {
    bool any = false;
    for (auto& [cab, t] : g_terms) any |= t->shown;
    g_anyShown = any;
}

void Toggle(HWND cab) {
    if (!IsWindow(cab)) return;
    Dbg(L"hotkey: toggle requested");
    auto it = g_terms.find(cab);
    if (it == g_terms.end()) {
        Term* t = Create(cab);
        if (!t) return;
        g_terms[cab] = t;
        Dbg(L"terminal created, folder=[%s], shell pid %lu", t->lastPath.c_str(), t->shellPid);
        SetActive(t, true);
    } else {
        Term* t = it->second;
        if (t->shown) {
            t->shown = false;
            ShowWindow(t->con, SW_HIDE);
            Relayout(t);
            SetActive(t, false);
        } else {
            t->shown = true;
            ShowWindow(t->con, SW_SHOWNA);
            Relayout(t);
            SetActive(t, true);
        }
    }
    UpdateAnyShown();
}

void Tick() {
    std::vector<Term*> gone;
    for (auto& [cab, t] : g_terms) {
        if (!IsWindow(cab) || WaitForSingleObject(t->hProc, 0) == WAIT_OBJECT_0 || !IsWindow(t->con)) {
            gone.push_back(t);
            continue;
        }
        if (!t->shown) continue;
        EnsureSubclasses(t);
        Relayout(t);  // also picks up a changed height setting
        if (t->follow) {
            std::wstring p = FolderOfActiveTab(cab);
            if (p.empty()) {
                // Home, This PC and the like have no folder to cd into: stay where we are.
                if (!t->folderWarned) Dbg(L"no folder for the active tab (special location or lookup failed)");
                t->folderWarned = true;
            } else {
                t->folderWarned = false;
                if (p != t->lastPath) {
                    Dbg(L"following to [%s]", p.c_str());
                    t->lastPath = p;
                    WriteCwd(t->cwdFile, p);
                }
            }
        }
    }
    for (Term* t : gone) Release(t, IsWindow(t->cab));
    UpdateAnyShown();
}

DWORD WINAPI WorkerThread(LPVOID ready) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue
    SetEvent((HANDLE)ready);
    if (!SetConsoleCtrlHandler(IgnoreConsoleEvents, TRUE)) Dbg(L"SetConsoleCtrlHandler failed: %lu", GetLastError());
    g_tickTimer = SetTimer(nullptr, 0, kTickMs, nullptr);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP_TOGGLE) {
            Toggle((HWND)msg.lParam);
        } else if (msg.message == WM_APP_DEAD) {
            auto it = g_terms.find((HWND)msg.lParam);
            if (it != g_terms.end()) Release(it->second, false);
            UpdateAnyShown();
        } else if (msg.message == WM_APP_CLICK) {
            ActivateOnClick({(LONG)(int)msg.wParam, (LONG)(int)msg.lParam});
        } else if (msg.message == WM_APP_KEY) {
            auto* k = (KeyMsg*)msg.lParam;
            TypeKey(*k);
            delete k;
        } else if (msg.message == WM_TIMER && msg.wParam == g_tickTimer) {
            Tick();
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    while (!g_terms.empty()) {
        Term* t = g_terms.begin()->second;
        Release(t, IsWindow(t->cab));
    }
    SetConsoleCtrlHandler(IgnoreConsoleEvents, FALSE);
    CoUninitialize();
    return 0;
}

// Keys that are never handed to the console: modifiers (their state is read from the real
// keyboard), the Windows keys and media/browser keys.
bool PassThroughKey(UINT vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS:
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        case VK_SNAPSHOT: case VK_PROCESSKEY:
            return true;
    }
    return vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2;
}

bool g_swallowed[256];  // keys whose press went to the console; hook thread only

// While a terminal is active and its Explorer window is in front, the typed key goes to the
// console (see "Typing") and not to Explorer. Returns true when the key was taken.
bool ForwardKey(const KBDLLHOOKSTRUCT* k, bool down) {
    HWND con = g_activeCon;
    UINT vk = k->vkCode;
    if (!con || vk >= 256 || PassThroughKey(vk)) return false;
    HWND fg = GetForegroundWindow();
    if (!fg || GetAncestor(fg, GA_ROOT) != g_activeCab.load()) {
        if (g_keyLogBudget > 0 && down) {
            g_keyLogBudget--;
            wchar_t cls[64] = L"?";
            if (fg) GetClassNameW(GetAncestor(fg, GA_ROOT), cls, ARRAYSIZE(cls));
            Dbg(L"a key was not taken: foreground window is %s, not the active Explorer window", cls);
        }
        return false;
    }
    if (!down && !g_swallowed[vk]) return false;  // the release of a key that was not taken

    auto dn = [](int v) { return (GetAsyncKeyState(v) & 0x8000) != 0; };
    bool alt = dn(VK_MENU), ctrl = dn(VK_CONTROL), shift = dn(VK_SHIFT);
    if (dn(VK_LWIN) || dn(VK_RWIN)) return false;
    if (alt && (vk == VK_TAB || vk == VK_F4 || vk == VK_SPACE || vk == VK_ESCAPE)) return false;  // window switching
    if (ctrl && vk == VK_ESCAPE) return false;                                                    // Start menu, task manager

    DWORD st = 0;
    if (dn(VK_LMENU)) st |= LEFT_ALT_PRESSED;
    if (dn(VK_RMENU)) st |= RIGHT_ALT_PRESSED;
    if (dn(VK_LCONTROL)) st |= LEFT_CTRL_PRESSED;
    if (dn(VK_RCONTROL)) st |= RIGHT_CTRL_PRESSED;
    if (shift) st |= SHIFT_PRESSED;
    if (GetKeyState(VK_CAPITAL) & 1) st |= CAPSLOCK_ON;
    if (GetKeyState(VK_NUMLOCK) & 1) st |= NUMLOCK_ON;
    if (GetKeyState(VK_SCROLL) & 1) st |= SCROLLLOCK_ON;
    if (k->flags & LLKHF_EXTENDED) st |= ENHANCED_KEY;

    WCHAR ch = 0;
    if (down) {
        BYTE ks[256] = {};
        ks[VK_SHIFT] = shift ? 0x80 : 0;
        ks[VK_CONTROL] = ctrl ? 0x80 : 0;
        ks[VK_MENU] = alt ? 0x80 : 0;
        ks[VK_CAPITAL] = (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
        HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(fg, nullptr));
        WCHAR buf[4] = {};
        // flag 4: leave the dead-key state of the real keyboard alone
        if (ToUnicodeEx(vk, k->scanCode, ks, buf, 4, 4, layout) >= 1) ch = buf[0];
    }

    auto* m = new KeyMsg{con, (WORD)vk, (WORD)k->scanCode, st, ch, down};
    if (!PostThreadMessageW(g_workerTid, WM_APP_KEY, 0, (LPARAM)m)) {
        delete m;
        return false;
    }
    g_swallowed[vk] = down;
    return true;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lParam;
        bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        if (down && !(k->flags & LLKHF_INJECTED) && (int)k->vkCode == g_hotkeyVk &&
            !!(GetAsyncKeyState(VK_CONTROL) & 0x8000) == g_hotkeyCtrl &&
            !(GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
            // The foreground window is the Explorer window whether or not a terminal is active.
            HWND fg = GetForegroundWindow();
            HWND cab = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
            DWORD pid = 0;
            if (cab && IsClass(cab, kCabClass) && GetWindowThreadProcessId(cab, &pid) &&
                pid == GetCurrentProcessId()) {
                PostThreadMessageW(g_workerTid, WM_APP_TOGGLE, 0, (LPARAM)cab);
                return 1;
            }
            if (cab) {  // only on a hotkey press, so cheap enough
                wchar_t cls[64] = L"?";
                GetClassNameW(cab, cls, ARRAYSIZE(cls));
                Dbg(L"hotkey ignored: foreground root window is %s", cls);
            }
        }
        if ((down || up) && !(k->flags & LLKHF_INJECTED) && ForwardKey(k, down)) return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Low-level mouse hook: tells the worker about left clicks while a terminal is open.
LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN && g_anyShown) {
        auto* m = (MSLLHOOKSTRUCT*)lParam;
        PostThreadMessageW(g_workerTid, WM_APP_CLICK, (WPARAM)(INT_PTR)m->pt.x, (LPARAM)(INT_PTR)m->pt.y);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Dedicated thread: a low-level hook must return fast, so it only forwards the key.
DWORD WINAPI HookThread(LPVOID) {
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, GetModuleHandleW(nullptr), 0);
    if (!hook) Dbg(L"SetWindowsHookEx (keyboard) failed: %u", GetLastError());
    HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!mouse) Dbg(L"SetWindowsHookEx (mouse) failed: %u", GetLastError());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    }
    if (hook) UnhookWindowsHookEx(hook);
    if (mouse) UnhookWindowsHookEx(mouse);
    return 0;
}

void LoadSettings() {
    PCWSTR hk = Wh_GetStringSetting(L"hotkey");
    if (!wcscmp(hk, L"f12")) {
        g_hotkeyVk = VK_F12;
        g_hotkeyCtrl = false;
        g_hotkeyName = L"F12";
    } else if (!wcscmp(hk, L"ctrl-backtick")) {
        g_hotkeyVk = VK_OEM_3;
        g_hotkeyCtrl = true;
        g_hotkeyName = L"Ctrl+`";
    } else {
        g_hotkeyVk = 'J';
        g_hotkeyCtrl = true;
        g_hotkeyName = L"Ctrl+J";
    }
    Wh_FreeStringSetting(hk);

    PCWSTR sh = Wh_GetStringSetting(L"shell");
    g_shell = !wcscmp(sh, L"pwsh") ? 1 : !wcscmp(sh, L"cmd") ? 2 : 0;
    Wh_FreeStringSetting(sh);

    PCWSTR ap = Wh_GetStringSetting(L"look");
    g_appearance = !wcscmp(ap, L"console") ? 1 : !wcscmp(ap, L"plain") ? 2 : 0;
    Wh_FreeStringSetting(ap);

    // A height dragged on the strip is kept until the "height" setting itself changes.
    int setting = std::clamp(Wh_GetIntSetting(L"height"), 100, 1200);
    int saved = Wh_GetIntValue(L"savedHeight", 0);
    if (Wh_GetIntValue(L"settingHeightSeen", -1) != setting) {
        saved = 0;
        Wh_SetIntValue(L"savedHeight", 0);
        Wh_SetIntValue(L"settingHeightSeen", setting);
    }
    g_heightLogical = saved > 0 ? saved : setting;
    g_follow = Wh_GetIntSetting(L"follow") != 0;
}

}  // namespace

BOOL Wh_ModInit() {
    LoadSettings();

    wchar_t logPath[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetTempPathW(ARRAYSIZE(logPath), logPath) && !wcscat_s(logPath, L"explorer-terminal.log") &&
        GetFileAttributesExW(logPath, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 100000) {
        DeleteFileW(logPath);
    }
    Dbg(L"Explorer Terminal 0.3.2 loaded");

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&BarProc, &g_hInst);
    g_msgMakeBar = RegisterWindowMessageW(L"ExplorerTerminal_MakeBar");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = BarProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kBarClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZENS);
    if (!RegisterClassExW(&wc)) return FALSE;

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_workerThread = CreateThread(nullptr, 0, WorkerThread, ready, 0, &g_workerTid);
    if (!g_workerThread || WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0) {
        CloseHandle(ready);
        return FALSE;
    }
    CloseHandle(ready);
    g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookTid);
    return TRUE;
}

void Wh_ModUninit() {
    if (g_hookThread) {
        PostThreadMessageW(g_hookTid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 3000);
        CloseHandle(g_hookThread);
    }
    if (g_workerThread) {
        PostThreadMessageW(g_workerTid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_workerThread, 8000);
        CloseHandle(g_workerThread);
    }
    UnregisterClassW(kBarClass, g_hInst);
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
