#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

// ==========================================
// 1. PATTERN OBSERVER (DLYA AKTIVNOY MODELI)
// ==========================================
class IModelObserver {
public:
    virtual ~IModelObserver() = default;
    virtual void onModelChanged() = 0;
};

// ==========================================
// 2. MODEL
// ==========================================
struct AgeStats {
    int years = 0;
    int months = 0;
    int days = 0;
    long long totalMinutes = 0;
    long long coffeeCups = 0;
    long long tvEpisodes = 0;
    double socialFeedKm = 0.0;
};

class AgeModel {
private:
    int lastDay = 1;
    int lastMonth = 1;
    int lastYear = 2000;
    bool hasData = false;
    AgeStats stats;
    std::vector<IModelObserver*> observers;

    long long toJdn(int y, int m, int d) const {
        int a = (14 - m) / 12;
        long long y_adj = y + 4800 - a;
        int m_adj = m + 12 * a - 3;
        return d + (153 * m_adj + 2) / 5 + 365 * y_adj + y_adj / 4 - y_adj / 100 + y_adj / 400 - 32045;
    }

    void notifyObservers() {
        for (auto* obs : observers) {
            obs->onModelChanged();
        }
    }

public:
    void addObserver(IModelObserver* obs) {
        observers.push_back(obs);
    }

    void setBirthDate(int day, int month, int year) {
        lastDay = day;
        lastMonth = month;
        lastYear = year;

        SYSTEMTIME st;
        GetLocalTime(&st);

        long long birthJdn = toJdn(year, month, day);
        long long currentJdn = toJdn(st.wYear, st.wMonth, st.wDay);
        long long totalDays = currentJdn - birthJdn;

        int y = st.wYear - year;
        int m = st.wMonth - month;
        int d = st.wDay - day;

        if (d < 0) {
            m -= 1;
            d += 30;
        }
        if (m < 0) {
            y -= 1;
            m += 12;
        }

        stats.years = y;
        stats.months = m;
        stats.days = d;
        stats.totalMinutes = totalDays * 24 * 60;
        stats.coffeeCups = totalDays * 2;
        stats.tvEpisodes = static_cast<long long>(totalDays * 1.5);
        stats.socialFeedKm = totalDays * 0.08;

        hasData = true;
        notifyObservers();
    }

    bool getHasData() const { return hasData; }
    AgeStats getStats() const { return stats; }
    void getLastInput(int& d, int& m, int& y) const {
        d = lastDay;
        m = lastMonth;
        y = lastYear;
    }
};

class AgeController;

// ==========================================
// 3. VIEW
// ==========================================
class MainWindowView : public IModelObserver {
private:
    HWND hMainWnd = nullptr;
    HWND hBtnInput = nullptr;
    HWND hTextResult = nullptr;
    AgeModel& model;
    AgeController* controller = nullptr;

public:
    MainWindowView(AgeModel& m) : model(m) {
        model.addObserver(this);
    }

    void setController(AgeController* c) { controller = c; }
    void setHwnd(HWND hwnd) { hMainWnd = hwnd; }
    HWND getHwnd() const { return hMainWnd; }

    void createControls(HWND hwnd) {
        hMainWnd = hwnd;

        hBtnInput = CreateWindowW(
            L"BUTTON", L"Vvesti dannye",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            30, 20, 160, 35,
            hwnd, (HMENU)101, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), nullptr
        );

        hTextResult = CreateWindowW(
            L"EDIT", L"Nazhmite 'Vvesti dannye', chtoby rasschitat vozrast.",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            30, 70, 480, 240,
            hwnd, (HMENU)102, (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), nullptr
        );

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(hBtnInput, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hTextResult, WM_SETFONT, (WPARAM)hFont, TRUE);
    }

    void onModelChanged() override {
        if (!model.getHasData()) return;

        AgeStats s = model.getStats();
        std::wstringstream ss;
        ss << L"Vash vozrast v privychnyh i neozhidannyh edinicah:\r\n"
            << L"----------------------------------------------------\r\n"
            << L"* Vozrast: " << s.years << L" let, " << s.months << L" mes., " << s.days << L" dn.\r\n"
            << L"* Vsego minut: " << s.totalMinutes << L"\r\n\r\n"
            << L"V neozhidannyh edinicah:\r\n"
            << L"* Vypito chashek kofe: ~" << s.coffeeCups << L" sht.\r\n"
            << L"* Prosmotreno seriy serialov: ~" << s.tvEpisodes << L" seriy\r\n"
            << L"* Prokrucheno kilometrov lenty socsetey: ~"
            << std::fixed << std::setprecision(2) << s.socialFeedKm << L" km\r\n";

        SetWindowTextW(hTextResult, ss.str().c_str());
    }
};

// ==========================================
// 4. CONTROLLER
// ==========================================
class AgeController {
private:
    AgeModel& model;
    MainWindowView& view;

    bool isLeapYear(int y) const {
        return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    }

    bool isValidDate(int d, int m, int y) const {
        SYSTEMTIME st;
        GetLocalTime(&st);

        if (y < 1900 || y > st.wYear) return false;
        if (m < 1 || m > 12) return false;

        int daysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (m == 2 && isLeapYear(y)) daysInMonth[1] = 29;

        if (d < 1 || d > daysInMonth[m - 1]) return false;

        if (y == st.wYear) {
            if (m > st.wMonth) return false;
            if (m == st.wMonth && d > st.wDay) return false;
        }
        return true;
    }

public:
    AgeController(AgeModel& m, MainWindowView& v) : model(m), view(v) {}

    void onInputRequested(HWND parent) {
        showInputDialog(parent);
    }

    bool submitDate(int d, int m, int y, HWND dlgHwnd) {
        if (!isValidDate(d, m, y)) {
            MessageBoxW(dlgHwnd,
                L"Vvedena nekorrektnaya data rozhdeniya!\nProverte znacheniya dnya, mesyaca i goda.",
                L"Oshibka vvoda", MB_OK | MB_ICONERROR);
            return false;
        }
        model.setBirthDate(d, m, y);
        return true;
    }

    void showInputDialog(HWND parent);
};

// ==========================================
// 5. DIALOG VVODA
// ==========================================
struct DialogContext {
    AgeController* controller;
    AgeModel* model;
};

LRESULT CALLBACK InputDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static DialogContext* ctx = nullptr;
    static HWND hEditDay, hEditMonth, hEditYear;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        ctx = (DialogContext*)cs->lpCreateParams;

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        CreateWindowW(L"STATIC", L"Den (1-31):", WS_CHILD | WS_VISIBLE, 20, 20, 100, 20, hwnd, nullptr, nullptr, nullptr);
        hEditDay = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 130, 20, 80, 22, hwnd, (HMENU)201, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Mesyac (1-12):", WS_CHILD | WS_VISIBLE, 20, 55, 100, 20, hwnd, nullptr, nullptr, nullptr);
        hEditMonth = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 130, 55, 80, 22, hwnd, (HMENU)202, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"God (GGGG):", WS_CHILD | WS_VISIBLE, 20, 90, 100, 20, hwnd, nullptr, nullptr, nullptr);
        hEditYear = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 130, 90, 80, 22, hwnd, (HMENU)203, nullptr, nullptr);

        HWND hBtnOk = CreateWindowW(L"BUTTON", L"Rasschitat", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 30, 135, 90, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);
        HWND hBtnCancel = CreateWindowW(L"BUTTON", L"Otmena", WS_CHILD | WS_VISIBLE, 130, 135, 80, 30, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

        SendMessageW(hEditDay, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hEditMonth, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hEditYear, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        int ld, lm, ly;
        ctx->model->getLastInput(ld, lm, ly);
        SetWindowTextW(hEditDay, std::to_wstring(ld).c_str());
        SetWindowTextW(hEditMonth, std::to_wstring(lm).c_str());
        SetWindowTextW(hEditYear, std::to_wstring(ly).c_str());
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK) {
            wchar_t bDay[16], bMonth[16], bYear[16];
            GetWindowTextW(hEditDay, bDay, 16);
            GetWindowTextW(hEditMonth, bMonth, 16);
            GetWindowTextW(hEditYear, bYear, 16);

            int d = _wtoi(bDay);
            int m = _wtoi(bMonth);
            int y = _wtoi(bYear);

            if (ctx->controller->submitDate(d, m, y, hwnd)) {
                DestroyWindow(hwnd);
            }
        }
        else if (id == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void AgeController::showInputDialog(HWND parent) {
    DialogContext ctx = { this, &model };

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = InputDlgProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"InputDateDialogClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"InputDateDialogClass", L"Vvod daty rozhdeniya",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 260, 220,
        parent, nullptr, wc.hInstance, &ctx
    );

    EnableWindow(parent, FALSE);
    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

// ==========================================
// 6. WINMAIN & GLAVNYY CIKL
// ==========================================
static AgeController* g_controller = nullptr;
static MainWindowView* g_view = nullptr;

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        if (g_view) g_view->createControls(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == 101 && g_controller) {
            g_controller->onInputRequested(hwnd);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    AgeModel model;
    MainWindowView view(model);
    AgeController controller(model, view);

    view.setController(&controller);
    g_controller = &controller;
    g_view = &view;

    const wchar_t CLASS_NAME[] = L"AgeCalculatorMainWindow";

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Vozrast v neozhidannyh edinicah (Variant 20)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 370,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}