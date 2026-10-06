#include <windows.h>
#include <powrprof.h>
#include <gdiplus.h>

#include <vector>
#include <string>
#include <sstream>
#include <cmath>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ============================================================
// IDs
// ============================================================

#define TIMER_ID       1

#define ID_SLEEP       10
#define ID_HIBERNATE   11
#define ID_LOG         12
#define ID_EXIT        13
#define ID_FILTER      20

// ============================================================
// Event type
// ============================================================

enum EventType
{
    EVENT_CHARGER,//git

    EVENT_BATTERY,
    EVENT_SLEEP
};

struct Event
{
    EventType type;
    std::wstring text;
};

// ============================================================
// Global variables
// ============================================================

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_logWindow = nullptr;

SYSTEM_POWER_STATUS g_power{};

BYTE g_lastBattery = 255;
BYTE g_lastAC = 255;

std::vector<Event> g_events;

ULONG_PTR g_gdiToken = 0;

// ============================================================
// Add event to log
// ============================================================

void AddLog(EventType type, const std::wstring& message)
{
    SYSTEMTIME t{};
    GetLocalTime(&t);

    std::wstringstream s;

    s << L"["
        << (t.wHour < 10 ? L"0" : L"")
        << t.wHour
        << L":"
        << (t.wMinute < 10 ? L"0" : L"")
        << t.wMinute
        << L":"
        << (t.wSecond < 10 ? L"0" : L"")
        << t.wSecond
        << L"] "
        << message;

    g_events.push_back({ type, s.str() });

    if (g_logWindow)
    {
        InvalidateRect(
            g_logWindow,
            nullptr,
            TRUE
        );
    }
}

// ============================================================
// Update power information
// ============================================================

void UpdatePower()
{
    SYSTEM_POWER_STATUS p{};

    if (!GetSystemPowerStatus(&p))
        return;

    // ========================================================
    // Charger event
    // ========================================================

    if (g_lastAC != 255 &&
        p.ACLineStatus != g_lastAC)
    {
        if (p.ACLineStatus == 1)
        {
            AddLog(
                EVENT_CHARGER,
                L"Charger connected"
            );
        }
        else
        {
            AddLog(
                EVENT_CHARGER,
                L"Charger disconnected"
            );
        }
    }

    // ========================================================
    // Battery event
    // ========================================================

    if (g_lastBattery != 255 &&
        p.BatteryLifePercent != g_lastBattery &&
        p.BatteryLifePercent != 255)
    {
        std::wstringstream s;

        s << L"Battery changed: "
            << (int)g_lastBattery
            << L"% -> "
            << (int)p.BatteryLifePercent
            << L"%";

        AddLog(
            EVENT_BATTERY,
            s.str()
        );
    }

    // ========================================================
    // Save current information
    // ========================================================

    g_power = p;

    g_lastAC = p.ACLineStatus;
    g_lastBattery = p.BatteryLifePercent;

    // ========================================================
    // Redraw widget
    // ========================================================

    if (g_mainWindow)
    {
        InvalidateRect(
            g_mainWindow,
            nullptr,
            FALSE
        );
    }
}

// ============================================================
// Shutdown privilege
// ============================================================

void EnableShutdownPrivilege()
{
    HANDLE token = nullptr;

    if (!OpenProcessToken(
        GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
        &token))
    {
        return;
    }

    LUID luid{};

    if (LookupPrivilegeValueW(
        nullptr,
        SE_SHUTDOWN_NAME,
        &luid))
    {
        TOKEN_PRIVILEGES tp{};

        tp.PrivilegeCount = 1;

        tp.Privileges[0].Luid = luid;

        tp.Privileges[0].Attributes =
            SE_PRIVILEGE_ENABLED;

        AdjustTokenPrivileges(
            token,
            FALSE,
            &tp,
            sizeof(tp),
            nullptr,
            nullptr
        );
    }

    CloseHandle(token);
}

// ============================================================
// Sleep / Hibernate
// ============================================================

void PowerAction(bool hibernate)
{
    EnableShutdownPrivilege();

    if (hibernate)
    {
        AddLog(
            EVENT_SLEEP,
            L"System entering hibernation"
        );
    }
    else
    {
        AddLog(
            EVENT_SLEEP,
            L"System entering sleep"
        );
    }

    SetSuspendState(
        hibernate ? TRUE : FALSE,
        FALSE,
        FALSE
    );
}

// ============================================================
// Draw main widget
// ============================================================

void DrawWidget(HDC hdc)
{
    // ========================================================
    // Background
    // ========================================================

    RECT r{};

    GetClientRect(
        g_mainWindow,
        &r
    );

    HBRUSH background =
        CreateSolidBrush(
            RGB(30, 30, 35)
        );

    FillRect(
        hdc,
        &r,
        background
    );

    DeleteObject(background);

    // ========================================================
    // Battery value
    // ========================================================

    int battery =
        g_power.BatteryLifePercent;

    if (battery == 255)
        battery = 0;

    if (battery < 0)
        battery = 0;

    if (battery > 100)
        battery = 100;

    // ========================================================
    // Battery color
    // ========================================================

    Color color;

    if (battery >= 50)
    {
        color = Color(
            255,
            60,
            200,
            80
        );
    }
    else if (battery >= 20)
    {
        color = Color(
            255,
            240,
            190,
            40
        );
    }
    else
    {
        color = Color(
            255,
            230,
            60,
            60
        );
    }

    // ========================================================
    // GDI+ drawing
    // ========================================================

    Graphics graphics(hdc);

    graphics.SetSmoothingMode(
        SmoothingModeAntiAlias
    );

    RectF circle(
        25.0f,
        25.0f,
        120.0f,
        120.0f
    );

    // ========================================================
    // Gray circle
    // ========================================================

    Pen grayPen(
        Color(255, 70, 70, 75),
        10.0f
    );

    graphics.DrawArc(
        &grayPen,
        circle,
        0.0f,
        360.0f
    );

    // ========================================================
    // Battery arc
    // ========================================================

    Pen batteryPen(
        color,
        10.0f
    );

    graphics.DrawArc(
        &batteryPen,
        circle,
        -90.0f,
        battery * 3.6f
    );

    // ========================================================
    // Percentage
    // ========================================================

    Font percentFont(
        L"Segoe UI",
        20,
        FontStyleBold,
        UnitPixel
    );

    SolidBrush white(
        Color(
            255,
            255,
            255,
            255
        )
    );

    RectF percentRect(
        25.0f,
        60.0f,
        120.0f,
        40.0f
    );

    StringFormat center;

    center.SetAlignment(
        StringAlignmentCenter
    );

    center.SetLineAlignment(
        StringAlignmentCenter
    );

    std::wstring percent =
        std::to_wstring(battery) + L"%";

    graphics.DrawString(
        percent.c_str(),
        -1,
        &percentFont,
        percentRect,
        &center,
        &white
    );

    // ========================================================
    // Power mode
    // ========================================================

    std::wstring mode;

    if (g_power.ACLineStatus == 1)
    {
        mode = L"AC Power";
    }
    else
    {
        mode = L"Battery";
    }

    Font textFont(
        L"Segoe UI",
        13,
        FontStyleRegular,
        UnitPixel
    );

    graphics.DrawString(
        L"Power:",
        -1,
        &textFont,
        PointF(160.0f, 35.0f),
        &white
    );

    graphics.DrawString(
        mode.c_str(),
        -1,
        &textFont,
        PointF(160.0f, 58.0f),
        &white
    );

    // ========================================================
    // Time
    // ========================================================

    SYSTEMTIME t{};

    GetLocalTime(&t);

    wchar_t timeText[50]{};

    swprintf_s(
        timeText,
        L"Time: %02d:%02d:%02d",
        t.wHour,
        t.wMinute,
        t.wSecond
    );

    graphics.DrawString(
        timeText,
        -1,
        &textFont,
        PointF(160.0f, 85.0f),
        &white
    );

    // ========================================================
    // Remaining time
    // ========================================================

    if (g_power.BatteryLifeTime != (DWORD)-1)
    {
        wchar_t remaining[50]{};

        swprintf_s(
            remaining,
            L"Left: %d min",
            g_power.BatteryLifeTime / 60
        );

        graphics.DrawString(
            remaining,
            -1,
            &textFont,
            PointF(160.0f, 110.0f),
            &white
        );
    }

    // ========================================================
    // Hint
    // ========================================================

    graphics.DrawString(
        L"Right click",
        -1,
        &textFont,
        PointF(160.0f, 140.0f),
        &white
    );
}

// ============================================================
// Context menu
// ============================================================

void ShowMenu(HWND hwnd)
{
    HMENU menu =
        CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING,
        ID_SLEEP,
        L"Sleep"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_HIBERNATE,
        L"Hibernate"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_LOG,
        L"Show full log"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenuW(
        menu,
        MF_STRING,
        ID_EXIT,
        L"Exit"
    );

    POINT p{};

    GetCursorPos(&p);

    SetForegroundWindow(hwnd);

    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        p.x,
        p.y,
        0,
        hwnd,
        nullptr
    );

    DestroyMenu(menu);
}

// ============================================================
// Filter helper
// ============================================================

bool ShowEvent(
    const Event& event,
    int filter)
{
    // All events
    if (filter == 0)
        return true;

    // Charger
    if (filter == 1 &&
        event.type == EVENT_CHARGER)
    {
        return true;
    }

    // Battery
    if (filter == 2 &&
        event.type == EVENT_BATTERY)
    {
        return true;
    }

    // Sleep
    if (filter == 3 &&
        event.type == EVENT_SLEEP)
    {
        return true;
    }

    return false;
}

// ============================================================
// Refresh log list
// ============================================================

void RefreshLog(
    HWND list,
    int filter)
{
    if (!list)
        return;

    SendMessageW(
        list,
        LB_RESETCONTENT,
        0,
        0
    );

    for (const auto& event : g_events)
    {
        if (ShowEvent(event, filter))
        {
            SendMessageW(
                list,
                LB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(
                    event.text.c_str()
                    )
            );
        }
    }
}

// ============================================================
// Log window procedure
// ============================================================

LRESULT CALLBACK LogProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    static HWND combo = nullptr;
    static HWND list = nullptr;

    switch (message)
    {
        // ========================================================
        // Create log window
        // ========================================================

    case WM_CREATE:
    {
        combo = CreateWindowW(
            L"COMBOBOX",
            nullptr,
            WS_CHILD |
            WS_VISIBLE |
            CBS_DROPDOWNLIST,
            10,
            10,
            220,
            100,
            hwnd,
            (HMENU)ID_FILTER,
            g_instance,
            nullptr
        );

        if (combo)
        {
            SendMessageW(
                combo,
                CB_ADDSTRING,
                0,
                (LPARAM)L"All"
            );

            SendMessageW(
                combo,
                CB_ADDSTRING,
                0,
                (LPARAM)L"Charger"
            );

            SendMessageW(
                combo,
                CB_ADDSTRING,
                0,
                (LPARAM)L"Battery"
            );

            SendMessageW(
                combo,
                CB_ADDSTRING,
                0,
                (LPARAM)L"Sleep"
            );

            SendMessageW(
                combo,
                CB_SETCURSEL,
                0,
                0
            );
        }

        list = CreateWindowW(
            L"LISTBOX",
            nullptr,
            WS_CHILD |
            WS_VISIBLE |
            WS_VSCROLL |
            LBS_NOINTEGRALHEIGHT,
            10,
            45,
            650,
            320,
            hwnd,
            nullptr,
            g_instance,
            nullptr
        );

        RefreshLog(
            list,
            0
        );

        return 0;
    }

    // ========================================================
    // Commands
    // ========================================================

    case WM_COMMAND:
    {
        if (LOWORD(wParam) == ID_FILTER &&
            HIWORD(wParam) == CBN_SELCHANGE)
        {
            int filter =
                (int)SendMessageW(
                    combo,
                    CB_GETCURSEL,
                    0,
                    0
                );

            RefreshLog(
                list,
                filter
            );
        }

        return 0;
    }

    // ========================================================
    // Close log window
    // ========================================================

    case WM_CLOSE:
    {
        g_logWindow = nullptr;

        DestroyWindow(hwnd);

        return 0;
    }

    // ========================================================
    // Destroy log window
    // ========================================================

    case WM_DESTROY:
    {
        combo = nullptr;
        list = nullptr;

        return 0;
    }
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}

// ============================================================
// Show log window
// ============================================================

void ShowLog()
{
    if (g_logWindow)
    {
        ShowWindow(
            g_logWindow,
            SW_SHOW
        );

        SetForegroundWindow(
            g_logWindow
        );

        return;
    }

    WNDCLASSW wc{};

    wc.lpfnWndProc = LogProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = L"PowerLog";
    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassW(&wc);

    g_logWindow =
        CreateWindowW(
            L"PowerLog",
            L"Power Event Log",
            WS_OVERLAPPEDWINDOW,
            300,
            250,
            700,
            420,
            g_mainWindow,
            nullptr,
            g_instance,
            nullptr
        );

    if (g_logWindow)
    {
        ShowWindow(
            g_logWindow,
            SW_SHOW
        );

        UpdateWindow(
            g_logWindow
        );
    }
}

// ============================================================
// Main window procedure
// ============================================================

LRESULT CALLBACK MainProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
        // ========================================================
        // Create main window
        // ========================================================

    case WM_CREATE:
    {
        SetTimer(
            hwnd,
            TIMER_ID,
            1000,
            nullptr
        );

        UpdatePower();

        return 0;
    }

    // ========================================================
    // Timer
    // ========================================================

    case WM_TIMER:
    {
        if (wParam == TIMER_ID)
        {
            UpdatePower();
        }

        return 0;
    }

    // ========================================================
    // Paint
    // ========================================================

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};

        HDC hdc =
            BeginPaint(
                hwnd,
                &ps
            );

        DrawWidget(hdc);

        EndPaint(
            hwnd,
            &ps
        );

        return 0;
    }

    // ========================================================
    // Move window with left mouse button
    // ========================================================

    case WM_LBUTTONDOWN:
    {
        ReleaseCapture();

        SendMessageW(
            hwnd,
            WM_NCLBUTTONDOWN,
            HTCAPTION,
            0
        );

        return 0;
    }

    // ========================================================
    // Right mouse button
    // ========================================================

    case WM_RBUTTONUP:
    {
        ShowMenu(hwnd);

        return 0;
    }

    // ========================================================
    // Keyboard
    // ========================================================

    case WM_KEYDOWN:
    {
        // ESC = exit
        if (wParam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
        }

        return 0;
    }

    // ========================================================
    // Power events
    // ========================================================

    case WM_POWERBROADCAST:
    {
        if (wParam == PBT_APMPOWERSTATUSCHANGE ||
            wParam == PBT_POWERSETTINGCHANGE)
        {
            UpdatePower();
        }

        if (wParam == PBT_APMSUSPEND)
        {
            AddLog(
                EVENT_SLEEP,
                L"System entered sleep"
            );
        }

        if (wParam == PBT_APMRESUMEAUTOMATIC ||
            wParam == PBT_APMRESUMESUSPEND)
        {
            AddLog(
                EVENT_SLEEP,
                L"System resumed"
            );

            UpdatePower();
        }

        return TRUE;
    }

    // ========================================================
    // Menu commands
    // ========================================================

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
            // ====================================================
            // Sleep
            // ====================================================

        case ID_SLEEP:
        {
            PowerAction(false);

            break;
        }

        // ====================================================
        // Hibernate
        // ====================================================

        case ID_HIBERNATE:
        {
            PowerAction(true);

            break;
        }

        // ====================================================
        // Show log
        // ====================================================

        case ID_LOG:
        {
            ShowLog();

            break;
        }

        // ====================================================
        // Exit
        // ====================================================

        case ID_EXIT:
        {
            // Destroy main window.
            // WM_DESTROY will call PostQuitMessage().
            DestroyWindow(hwnd);

            break;
        }
        }

        return 0;
    }

    // ========================================================
    // Window close
    // ========================================================

    case WM_CLOSE:
    {
        DestroyWindow(hwnd);

        return 0;
    }

    // ========================================================
    // Window destruction
    // ========================================================

    case WM_DESTROY:
    {
        // Stop timer
        KillTimer(
            hwnd,
            TIMER_ID
        );

        // Close log window
        if (g_logWindow)
        {
            HWND log = g_logWindow;

            g_logWindow = nullptr;

            DestroyWindow(log);
        }

        // Tell Windows to terminate
        // the message loop.
        PostQuitMessage(0);

        return 0;
    }
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}

// ============================================================
// Program entry point
// ============================================================

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    g_instance = hInstance;

    // ========================================================
    // IMPORTANT:
    //
    // AUTORUN IS COMPLETELY DISABLED.
    //
    // There is NO call to:
    //
    // EnableAutorun();
    //
    // and there is NO registry modification.
    // ========================================================

    // ========================================================
    // GDI+
    // ========================================================

    GdiplusStartupInput input{};

    if (GdiplusStartup(
        &g_gdiToken,
        &input,
        nullptr) != Ok)
    {
        return 1;
    }

    // ========================================================
    // Main window class
    // ========================================================

    WNDCLASSW wc{};

    wc.lpfnWndProc = MainProc;

    wc.hInstance = hInstance;

    wc.lpszClassName =
        L"PowerWidget";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    wc.hbrBackground =
        (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc))
    {
        GdiplusShutdown(
            g_gdiToken
        );

        return 1;
    }

    // ========================================================
    // Create main window
    // ========================================================

    g_mainWindow =
        CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"PowerWidget",
            L"Power Widget",
            WS_POPUP,
            300,
            200,
            300,
            200,
            nullptr,
            nullptr,
            hInstance,
            nullptr
        );

    if (!g_mainWindow)
    {
        GdiplusShutdown(
            g_gdiToken
        );

        return 1;
    }

    // ========================================================
    // Show window
    // ========================================================

    ShowWindow(
        g_mainWindow,
        SW_SHOW
    );

    UpdateWindow(
        g_mainWindow
    );

    // ========================================================
    // Main message loop
    // ========================================================

    MSG message{};

    while (GetMessageW(
        &message,
        nullptr,
        0,
        0) > 0)
    {
        TranslateMessage(
            &message
        );

        DispatchMessageW(
            &message
        );
    }

    // ========================================================
    // Shutdown GDI+
    // ========================================================

    GdiplusShutdown(
        g_gdiToken
    );

    return (int)message.wParam;
}       