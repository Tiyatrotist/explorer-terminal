// Self-check for the Windows Terminal scheme code of explorer-terminal.wh.cpp.
// It includes the mod source to reach the internal functions. Build and run: tests/run.ps1
#undef Wh_Log
#define Wh_Log(...) ((void)0)

// The Windhawk engine provides these when the mod runs inside Windhawk; here they are inert
// stubs so the test links. The checks below never depend on them.
extern "C" {
BOOL InternalWh_IsLogEnabled(void*) { return FALSE; }
void InternalWh_Log(void*, PCWSTR, va_list) {}
int InternalWh_GetIntValue(void*, PCWSTR, int def) { return def; }
BOOL InternalWh_SetIntValue(void*, PCWSTR, int) { return TRUE; }
int InternalWh_GetIntSetting(void*, PCWSTR, va_list) { return 0; }
PCWSTR InternalWh_GetStringSetting(void*, PCWSTR, va_list) { return L""; }
void InternalWh_FreeStringSetting(void*, PCWSTR) {}
}

#include "../explorer-terminal.wh.cpp"

#include <cstdio>
#include <cstdlib>

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL line %d: %s\n", __LINE__, #c);                  \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

// Shape of a real settings.json (comments and trailing commas included).
static const char kSettings[] = R"JSON(
// top comment
{
  "defaultProfile": "{0caa}",
  "profiles": {
    "defaults": { "colorScheme": "Esinti_Cloud", },
    "list": [
      { "commandline": "%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", "name": "Windows PowerShell" },
      { "commandline": "%SystemRoot%\\System32\\cmd.exe", "name": "Command Prompt",
        "colorScheme": { "dark": "Other", "light": "Other" }, /* block */
        "font": { "face": "Fira Code", "size": 11 } },
    ]
  },
  "schemes": [
    { "name": "Esinti_Cloud", "background": "#0F171E", "foreground": "#F3F4F6",
      "black": "#0C0C0C", "blue": "#3B82F6", "green": "#10B981", "cyan": "#06B6D4",
      "red": "#EF4444", "purple": "#8B5CF6", "yellow": "#F59E0B", "white": "#E5E7EB",
      "brightBlack": "#767676", "brightBlue": "#60A5FA", "brightGreen": "#4ADE80", "brightCyan": "#22D3EE",
      "brightRed": "#F87171", "brightPurple": "#A855F7", "brightYellow": "#FBBF24", "brightWhite": "#FFFFFF" },
    { "name": "Other", "background": "#010203", "foreground": "#040506",
      "black": "#000001", "blue": "#000002", "green": "#000003", "cyan": "#000004",
      "red": "#000005", "purple": "#000006", "yellow": "#000007", "white": "#000008",
      "brightBlack": "#000009", "brightBlue": "#00000A", "brightGreen": "#00000B", "brightCyan": "#00000C",
      "brightRed": "#00000D", "brightPurple": "#00000E", "brightYellow": "#00000F", "brightWhite": "#000010" }
  ]
}
)JSON";

static void TestParser() {
    Scheme s;
    CHECK(SchemeFromText(kSettings, 0, s));  // PowerShell: default scheme
    CHECK(s.bg == RGB(0x0F, 0x17, 0x1E));
    CHECK(s.fg == RGB(0xF3, 0xF4, 0xF6));
    CHECK(s.c[0] == RGB(0x0C, 0x0C, 0x0C));
    CHECK(s.c[1] == RGB(0x3B, 0x82, 0xF6));  // console index 1 is blue
    CHECK(s.c[4] == RGB(0xEF, 0x44, 0x44));  // console index 4 is red
    CHECK(s.c[15] == RGB(0xFF, 0xFF, 0xFF));
    CHECK(s.face == L"Cascadia Mono" && s.fontPx == 16);

    Scheme o;
    CHECK(SchemeFromText(kSettings, 2, o));  // cmd: profile override, {"dark": ...} form, font
    CHECK(o.bg == RGB(1, 2, 3));
    CHECK(o.c[1] == RGB(0, 0, 2));
    CHECK(o.face == L"Fira Code" && o.fontPx == 15);  // 11 pt -> 15 px

    Scheme m;
    CHECK(!SchemeFromText(R"({"profiles":{"defaults":{"colorScheme":"Nope"}}})", 0, m));  // unknown scheme
    CHECK(SchemeFromText("{}", 0, m) && m.bg == RGB(0x0C, 0x0C, 0x0C));                    // empty file: Campbell
    CHECK(!SchemeFromText("not json at all", 1, m) || m.bg == RGB(0x0C, 0x0C, 0x0C));      // garbage must not crash
}

static void TestApply() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\conhost.exe\" powershell.exe -NoLogo -NoExit";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    HWND con = nullptr;
    for (int i = 0; i < 80 && !con; i++) {
        Sleep(100);
        con = FindConsoleWindow(pi.dwProcessId);
    }
    CHECK(con != nullptr);
    if (con) {
        DWORD shellPid = ChildOf(pi.dwProcessId);
        Scheme s;
        CHECK(SchemeFromText(kSettings, 0, s));
        FreeConsole();  // this test process must not hold a console of its own
        bool applied = false;
        for (int i = 0; i < 20 && !applied; i++) {
            applied = ApplyScheme(shellPid, s);
            if (!applied) Sleep(100);
        }
        CHECK(applied);

        CHECK(AttachConsole(shellPid));
        HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFOEX i{};
        i.cbSize = sizeof(i);
        CHECK(GetConsoleScreenBufferInfoEx(out, &i));
        CHECK(i.ColorTable[0] == s.bg);
        CHECK(i.ColorTable[7] == s.fg);
        CHECK(i.ColorTable[1] == s.c[1]);
        CHECK(i.ColorTable[12] == s.c[12]);
        CONSOLE_FONT_INFOEX f{};
        f.cbSize = sizeof(f);
        GetCurrentConsoleFontEx(out, FALSE, &f);
        printf("info: console font after apply: %ls, height %d\n", f.FaceName, f.dwFontSize.Y);
        CloseHandle(out);
        FreeConsole();
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

// Types `text` into the console of `pid` as key events.
static bool TypeInto(DWORD pid, const std::wstring& text) {
    FreeConsole();
    if (!AttachConsole(pid)) {
        printf("info: TypeInto: AttachConsole(%lu) failed: %lu\n", pid, GetLastError());
        return false;
    }
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    std::vector<INPUT_RECORD> r;
    for (wchar_t c : text) {
        INPUT_RECORD k{};
        k.EventType = KEY_EVENT;
        k.Event.KeyEvent.bKeyDown = TRUE;
        k.Event.KeyEvent.wRepeatCount = 1;
        k.Event.KeyEvent.uChar.UnicodeChar = c;
        if (c == L'\r') k.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        r.push_back(k);
        k.Event.KeyEvent.bKeyDown = FALSE;
        r.push_back(k);
    }
    DWORD n = 0;
    bool ok = in != INVALID_HANDLE_VALUE && WriteConsoleInputW(in, r.data(), (DWORD)r.size(), &n);
    CloseHandle(in);
    FreeConsole();
    return ok;
}

static std::string ReadSmall(const std::wstring& p) { return ReadFileText(p); }

// The shell exactly as Create starts it: typing must reach it and it must follow the cwd file.
static void TestShell(bool withScheme) {
    printf("shell test, scheme applied: %d\n", withScheme);
    wchar_t sys[MAX_PATH], tmp[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    GetTempPathW(MAX_PATH, tmp);
    std::wstring cwd = std::wstring(tmp) + L"et-shell-cwd.txt", typed = std::wstring(tmp) + L"et-shell-typed.txt",
                 where = std::wstring(tmp) + L"et-shell-where.txt";
    DeleteFileW(typed.c_str());
    DeleteFileW(where.c_str());
    WriteCwd(cwd, L"C:\\Users");
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\conhost.exe\" powershell.exe -NoExit -EncodedCommand " +
                       Base64Utf16(InitScript(cwd));
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    HWND con = nullptr;
    for (int i = 0; i < 80 && !con; i++) {
        Sleep(100);
        con = FindConsoleWindow(pi.dwProcessId);
    }
    CHECK(con != nullptr);
    if (con) {
        DWORD shellPid = ChildOf(pi.dwProcessId);
        CHECK(shellPid != 0);
        if (withScheme) {
            Scheme s;
            CHECK(SchemeFromText(kSettings, 0, s));
            FreeConsole();
            bool ok = false;
            for (int i = 0; i < 20 && !ok; i++) {
                ok = ApplyScheme(shellPid, s);
                if (!ok) Sleep(100);
            }
            CHECK(ok);
        }
        Sleep(9000);  // let PowerShell reach its prompt (input sent earlier is discarded) and the OnIdle handler run
        TypeInto(shellPid, L"\r");  // the first input after start is sometimes discarded: warm up
        Sleep(1500);
        CHECK(TypeInto(shellPid, L"'ET_OK' | Out-File -Encoding ascii '" + typed + L"'\r"));
        Sleep(2500);
        CHECK(ReadSmall(typed).find("ET_OK") != std::string::npos);
        WriteCwd(cwd, L"C:\\Windows");
        Sleep(4000);
        CHECK(TypeInto(shellPid, L"(Get-Location).ProviderPath | Out-File -Encoding ascii '" + where + L"'\r"));
        Sleep(2500);
        std::string w = ReadSmall(where);
        printf("info: shell location after cwd file change: %s\n", w.c_str());
        CHECK(w.find("C:\\Windows") != std::string::npos);
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    DeleteFileW(cwd.c_str());
    DeleteFileW(typed.c_str());
    DeleteFileW(where.c_str());
}

// This thread owns the stand-in window, so it has to keep pumping messages while it waits.
static void Pump(DWORD ms) {
    for (DWORD end = GetTickCount() + ms; GetTickCount() < end;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(20);
    }
}

// The mod as it runs, against a stand-in Explorer window (same class names, off screen):
// the toggle must create the terminal and the worker's timer must keep ticking.
static void TestWorker() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCabClass;
    RegisterClassW(&wc);
    wc.lpszClassName = kTabClass;
    RegisterClassW(&wc);
    HWND cab = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCabClass, L"fake", WS_OVERLAPPEDWINDOW, -5000, -5000,
                               900, 600, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(cab != nullptr);
    CreateWindowExW(0, kTabClass, L"", WS_CHILD | WS_VISIBLE, 0, 40, 900, 500, cab, nullptr, wc.hInstance, nullptr);
    ShowWindow(cab, SW_SHOWNA);

    wchar_t log[MAX_PATH];
    GetTempPathW(MAX_PATH, log);
    wcscat_s(log, L"explorer-terminal.log");
    DeleteFileW(log);

    FreeConsole();  // like Explorer, this process has no console of its own
    CHECK(Wh_ModInit());
    g_follow = true;
    PostThreadMessageW(g_workerTid, WM_APP_TOGGLE, 0, (LPARAM)cab);
    Pump(10000);  // creation, the shell reaching its prompt (input sent earlier is discarded), one tick
    Pump(500);
    std::string text = ReadFileText(log);
    printf("info: log of the worker run:\n%s", text.c_str());
    CHECK(text.find("terminal created") != std::string::npos);
    CHECK(text.find("no folder for the active tab") != std::string::npos);  // only the 500 ms tick writes this

    // Typing: keys posted like the keyboard hook posts them reach the shell.
    Term* t = g_terms.count(cab) ? g_terms[cab] : nullptr;
    CHECK(t != nullptr);
    if (t) {
        // A real click anywhere else deactivates the terminal, as it should; click into it first.
        RECT r;
        GetWindowRect(t->con, &r);
        auto clickIn = [&] {
            PostThreadMessageW(g_workerTid, WM_APP_CLICK, (WPARAM)((r.left + r.right) / 2), (LPARAM)((r.top + r.bottom) / 2));
            Pump(300);
        };
        clickIn();
        CHECK(t->active && g_activeCon == t->con);
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring f1 = std::wstring(tmp) + L"et-key-1.txt", f2 = std::wstring(tmp) + L"et-key-2.txt",
                     f3 = std::wstring(tmp) + L"et-key-3.txt";
        DeleteFileW(f1.c_str());
        DeleteFileW(f2.c_str());
        DeleteFileW(f3.c_str());
        auto key = [&](WORD vk, WCHAR ch, DWORD st = 0) {
            for (bool down : {true, false}) {
                PostThreadMessageW(g_workerTid, WM_APP_KEY, 0, (LPARAM) new KeyMsg{t->con, vk, 0, st, down ? ch : (WCHAR)0, down});
            }
        };
        auto type = [&](const std::wstring& s) {
            for (wchar_t c : s) key(c == L' ' ? VK_SPACE : (WORD)(VkKeyScanW(c) & 0xFF), c);
        };
        printf("step: plain text\n");
        // plain text, Enter
        clickIn();
        type(L"'ET_KEYS' | Out-File -Encoding ascii '" + f1 + L"'");
        key(VK_RETURN, L'\r');
        Pump(2500);
        CHECK(ReadSmall(f1).find("ET_KEYS") != std::string::npos);
        printf("step: backspace\n");
        // Backspace: the typo is removed before Enter
        type(L"'ET_BSX");
        key(VK_BACK, 8);
        type(L"Y' | Out-File -Encoding ascii '" + f2 + L"'");
        key(VK_RETURN, L'\r');
        Pump(2500);
        CHECK(ReadSmall(f2).find("ET_BSY") != std::string::npos);
        printf("step: ctrl+c\n");
        // Ctrl+C at the prompt cancels the half-typed line (PSReadLine reads it as a key); what
        // comes next then runs on a clean line. Ctrl+C during a running command is a posted
        // message that needs the real keyboard state: not testable here.
        type(L"garbage that must not run");
        key('C', 3, LEFT_CTRL_PRESSED);
        Pump(500);
        type(L"'ET_AFTER' | Out-File -Encoding ascii '" + f3 + L"'");
        key(VK_RETURN, L'\r');
        Pump(2500);
        CHECK(ReadSmall(f3).find("ET_AFTER") != std::string::npos);
        DeleteFileW(f1.c_str());
        DeleteFileW(f2.c_str());
        DeleteFileW(f3.c_str());
    }
    printf("info: log after typing:\n%s", ReadFileText(log).c_str());
    Wh_ModUninit();
    DestroyWindow(cab);
}

struct CabList { std::vector<HWND> v; };
static BOOL CALLBACK CollectCabs(HWND h, LPARAM l) {
    if (IsClass(h, kCabClass) && IsWindowVisible(h)) ((CabList*)l)->v.push_back(h);
    return TRUE;
}

// Read-only: asks every open Explorer window which folder the mod would follow.
static void TestFolder() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CabList c;
    EnumWindows(CollectCabs, (LPARAM)&c);
    printf("info: %zu visible Explorer windows\n", c.v.size());
    for (HWND h : c.v) {
        wchar_t title[200] = L"";
        GetWindowTextW(h, title, 200);
        HWND tab = VisibleTab(h);
        std::wstring f = FolderOfActiveTab(h);
        printf("info: [%ls] tabs=%zu visibleTab=%p folder=[%ls]\n", title, TabsOf(h).size(), (void*)tab, f.c_str());
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestFolder();
    TestParser();
    TestApply();
    TestShell(false);
    TestShell(true);
    TestWorker();
    printf(g_fail ? "FAILED: %d\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
