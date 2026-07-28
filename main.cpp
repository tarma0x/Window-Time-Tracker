#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <psapi.h>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <ctime>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_TRAYICON        (WM_APP + 1)
#define ID_TRAY_SUMMARY    1001
#define ID_TRAY_OPENFOLDER 1002
#define ID_TRAY_EXIT       1003
#define TIMER_ID_POLL      1
#define POLL_INTERVAL_MS   2000   
#define MIN_SESSION_SECS   1      

static NOTIFYICONDATAW g_nid = {};
static HWND g_hwnd = nullptr;
static std::wstring g_logPath;

struct Session {
    std::wstring processName;
    std::wstring windowTitle;
    time_t       startTime = 0;
    bool         active = false;
};
static Session g_current;

static std::wstring GetLogFilePath() {
    wchar_t appData[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        return L"activity_log.csv"; 
    }
    std::wstring dir = std::wstring(appData) + L"\\WindowTimeTracker";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\activity_log.csv";
}

static bool GetActiveWindowInfo(std::wstring& processName, std::wstring& windowTitle) {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;

    int len = GetWindowTextLengthW(hwnd);
    std::wstring title;
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        GetWindowTextW(hwnd, buf.data(), len + 1);
        title = buf.data();
    }
    if (title.empty()) title = L"(no title)";

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring proc = L"Unknown";
    if (pid != 0) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
                std::wstring full(path);
                size_t pos = full.find_last_of(L"\\/");
                proc = (pos == std::wstring::npos) ? full : full.substr(pos + 1);
            }
            CloseHandle(hProc);
        }
    }

    processName = proc;
    windowTitle = title;
    return true;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};

    int size = WideCharToMultiByte(
        CP_UTF8, 0,
        w.c_str(), (int)w.size(),
        nullptr, 0,
        nullptr, nullptr);

    std::string out(size, '\0');

    WideCharToMultiByte(
        CP_UTF8, 0,
        w.c_str(), (int)w.size(),
        &out[0], size,
        nullptr, nullptr);

    return out;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};

    int size = MultiByteToWideChar(
        CP_UTF8, 0,
        s.c_str(), (int)s.size(),
        nullptr, 0);

    std::wstring out(size, L'\0');

    MultiByteToWideChar(
        CP_UTF8, 0,
        s.c_str(), (int)s.size(),
        &out[0], size);

    return out;
}

static std::wstring CsvEscape(const std::wstring& field) {
    bool needsQuotes = field.find_first_of(L";\"\n") != std::wstring::npos;
    if (!needsQuotes) return field;
    std::wstring out = L"\"";
    for (wchar_t c : field) {
        if (c == L'"') out += L"\"\"";
        else out += c;
    }
    out += L"\"";
    return out;
}

static void EnsureCsvHeader() {
    std::ifstream test(g_logPath, std::ios::binary);
    bool exists = test.good() && test.peek() != std::ifstream::traits_type::eof();
    test.close();
    if (!exists) {
        std::ofstream out(g_logPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
        out << "sep=;\r\n";
        out << "Date;StartTime;EndTime;DurationSeconds;Process;WindowTitle\r\n";
    }
}

static void WriteSession(const Session& s, time_t endTime) {
    double duration = difftime(endTime, s.startTime);
    if (duration < MIN_SESSION_SECS) return;

    tm startTm, endTm;
    localtime_s(&startTm, &s.startTime);
    localtime_s(&endTm, &endTime);

    wchar_t dateBuf[16], startBuf[16], endBuf[16];
    wcsftime(dateBuf, 16, L"%Y-%m-%d", &startTm);
    wcsftime(startBuf, 16, L"%H:%M:%S", &startTm);
    wcsftime(endBuf, 16, L"%H:%M:%S", &endTm);

    std::wstringstream lineW;
    lineW << dateBuf << L";" << startBuf << L";" << endBuf << L";"
        << (long)duration << L";"
        << CsvEscape(s.processName) << L";"
        << CsvEscape(s.windowTitle) << L"\r\n";

    std::ofstream out(g_logPath, std::ios::binary | std::ios::app);
    if (!out.is_open()) return;
    std::string lineUtf8 = WideToUtf8(lineW.str());
    out.write(lineUtf8.data(), (std::streamsize)lineUtf8.size());
}

static std::vector<std::wstring> ParseCsvLine(const std::wstring& line) {
    std::vector<std::wstring> fields;
    std::wstring cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); i++) {
        wchar_t c = line[i];
        if (inQuotes) {
            if (c == L'"') {
                if (i + 1 < line.size() && line[i + 1] == L'"') { cur += L'"'; i++; }
                else inQuotes = false;
            }
            else cur += c;
        }
        else {
            if (c == L'"') inQuotes = true;
            else if (c == L';') { fields.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    fields.push_back(cur);
    return fields;
}

static void ShowTodaySummary() {
    time_t now = time(nullptr);
    tm nowTm;
    localtime_s(&nowTm, &now);
    wchar_t todayBuf[16];
    wcsftime(todayBuf, 16, L"%Y-%m-%d", &nowTm);
    std::wstring today(todayBuf);

    std::ifstream in(g_logPath, std::ios::binary);
    if (!in.is_open()) {
        MessageBoxW(nullptr, L"No data registered yet.", L"Today's Summary", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::map<std::wstring, long> totals;
    std::string lineUtf8;
    bool first = true;
    while (std::getline(in, lineUtf8)) {
        if (!lineUtf8.empty() && lineUtf8.back() == '\r') lineUtf8.pop_back();
        if (!lineUtf8.empty() && (unsigned char)lineUtf8[0] == 0xEF &&
            lineUtf8.size() >= 3 && (unsigned char)lineUtf8[1] == 0xBB && (unsigned char)lineUtf8[2] == 0xBF) {
            lineUtf8 = lineUtf8.substr(3);
        }
        if (lineUtf8.rfind("sep=", 0) == 0) continue; 
        std::wstring line = Utf8ToWide(lineUtf8);
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        auto f = ParseCsvLine(line);
        if (f.size() < 6) continue;
        if (f[0] != today) continue;
        long dur = _wtol(f[3].c_str());
        totals[f[4]] += dur;
    }

    if (totals.empty()) {
        MessageBoxW(nullptr, L"No activities registered today.", L"Today's Summary", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::vector<std::pair<std::wstring, long>> sorted(totals.begin(), totals.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });

    std::wstringstream msg;
    msg << L"Today's Summary (" << today << L"):\n\n";
    for (auto& p : sorted) {
        long secs = p.second;
        int h = (int)(secs / 3600);
        int m = (int)((secs % 3600) / 60);
        int s = (int)(secs % 60);
        wchar_t buf[64];
        swprintf(buf, 64, L"%02d:%02d:%02d", h, m, s);
        msg << p.first << L" — " << buf << L"\n";
    }

    MessageBoxW(nullptr, msg.str().c_str(), L"Today's Summary", MB_OK | MB_ICONINFORMATION);
}

static void AddTrayIcon(HWND hwnd) {
    g_nid = {};
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Window Time Tracker");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_SUMMARY, L"Today's Summary");
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPENFOLDER, L"Open Log Folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void PollActiveWindow() {
    std::wstring proc, title;
    if (!GetActiveWindowInfo(proc, title)) return;

    if (g_current.active && g_current.processName == proc && g_current.windowTitle == title) {
        return;
    }

    time_t now = time(nullptr);
    if (g_current.active) {
        WriteSession(g_current, now);
    }

    g_current.processName = proc;
    g_current.windowTitle = title;
    g_current.startTime = now;
    g_current.active = true;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        SetTimer(hwnd, TIMER_ID_POLL, POLL_INTERVAL_MS, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID_POLL) PollActiveWindow();
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        else if (lParam == WM_LBUTTONDBLCLK) {
            ShowTodaySummary();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_SUMMARY:
            ShowTodaySummary();
            break;
        case ID_TRAY_OPENFOLDER: {
            std::wstring dir = g_logPath.substr(0, g_logPath.find_last_of(L"\\/"));
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        if (g_current.active) {
            WriteSession(g_current, time(nullptr));
        }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"WindowTimeTracker_SingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Window Time Tracker is already running (see the system tray).",
            L"Already running", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    g_logPath = GetLogFilePath();
    EnsureCsvHeader();

    const wchar_t CLASS_NAME[] = L"WindowTimeTrackerClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, CLASS_NAME, L"WindowTimeTracker", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
