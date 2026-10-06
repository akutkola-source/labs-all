#pragma execution_character_set("utf-8")

#include "View.h"
#include "Controller.h"

#include <string>

#define IDC_INPUT_BUTTON 1001

#define IDC_DAY_EDIT     2001
#define IDC_MONTH_EDIT   2002
#define IDC_YEAR_EDIT    2003

#define IDC_OK_BUTTON    2004
#define IDC_CANCEL_BUTTON 2005

// ============================================================
// ГЛАВНОЕ ОКНО
// ============================================================

MainView::MainView()
    : hMainWindow(nullptr),
    hTitle(nullptr),
    hButton(nullptr),
    hDateLabel(nullptr),
    hYearsLabel(nullptr),
    hMonthsLabel(nullptr),
    hDaysLabel(nullptr),
    hMinutesLabel(nullptr),
    hCoffeeLabel(nullptr),
    hEpisodesLabel(nullptr),
    hSocialLabel(nullptr),
    controller(nullptr)
{
}

bool MainView::Create(
    HINSTANCE hInstance,
    Controller* controller)
{
    this->controller = controller;

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        MainView::WindowProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        L"AgeUnexpectedMainWindow";

    wc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW);

    wc.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_WINDOW + 1);

    wc.style =
        CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassW(&wc))
    {
        MessageBoxW(
            nullptr,
            L"Не удалось зарегистрировать главное окно.",
            L"Ошибка",
            MB_OK | MB_ICONERROR);

        return false;
    }

    hMainWindow =
        CreateWindowExW(
            0,
            L"AgeUnexpectedMainWindow",
            L"Возраст в неожиданных единицах",
            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU |
            WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            650,
            620,
            nullptr,
            nullptr,
            hInstance,
            this);

    if (!hMainWindow)
        return false;

    CreateControls(hInstance);

    return true;
}

void MainView::CreateControls(
    HINSTANCE hInstance)
{
    HFONT hFont =
        CreateFontW(
            18,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");

    HFONT hTitleFont =
        CreateFontW(
            25,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");

    hTitle =
        CreateWindowW(
            L"STATIC",
            L"Возраст в неожиданных единицах",
            WS_CHILD |
            WS_VISIBLE |
            SS_CENTER,
            50,
            25,
            550,
            45,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    SendMessageW(
        hTitle,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            hTitleFont),
        TRUE);

    hButton =
        CreateWindowW(
            L"BUTTON",
            L"Ввести данные",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            215,
            85,
            220,
            45,
            hMainWindow,
            reinterpret_cast<HMENU>(
                IDC_INPUT_BUTTON),
            hInstance,
            nullptr);

    SendMessageW(
        hButton,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            hFont),
        TRUE);

    hDateLabel =
        CreateWindowW(
            L"STATIC",
            L"Дата рождения: данные не введены",
            WS_CHILD |
            WS_VISIBLE |
            SS_CENTER,
            50,
            145,
            550,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    SendMessageW(
        hDateLabel,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            hFont),
        TRUE);

    hYearsLabel =
        CreateWindowW(
            L"STATIC",
            L"Возраст в годах: —",
            WS_CHILD |
            WS_VISIBLE,
            80,
            200,
            500,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    hMonthsLabel =
        CreateWindowW(
            L"STATIC",
            L"Возраст в месяцах: —",
            WS_CHILD |
            WS_VISIBLE,
            80,
            235,
            500,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    hDaysLabel =
        CreateWindowW(
            L"STATIC",
            L"Возраст в днях: —",
            WS_CHILD |
            WS_VISIBLE,
            80,
            270,
            500,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    hMinutesLabel =
        CreateWindowW(
            L"STATIC",
            L"Возраст в минутах: —",
            WS_CHILD |
            WS_VISIBLE,
            80,
            305,
            500,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    hCoffeeLabel =
        CreateWindowW(
            L"STATIC",
            L"Чашки кофе: —",
            WS_CHILD |
            WS_VISIBLE,
            80,
            340,
            500,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    hEpisodesLabel =
        CreateWindowW(
            L"STATIC",
            L"Серии сериала: —",
            WS_CHILD |
            WS_VISIBLE,
            80,
            375,
            500,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    hSocialLabel =
        CreateWindowW(
            L"STATIC",
            L"Километры ленты соцсетей: —",
            WS_CHILD |
            WS_VISIBLE,
            80,
            410,
            500,
            30,
            hMainWindow,
            nullptr,
            hInstance,
            nullptr);

    HWND labels[] =
    {
        hYearsLabel,
        hMonthsLabel,
        hDaysLabel,
        hMinutesLabel,
        hCoffeeLabel,
        hEpisodesLabel,
        hSocialLabel
    };

    for (HWND label : labels)
    {
        SendMessageW(
            label,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(
                hFont),
            TRUE);
    }
}

HWND MainView::GetWindowHandle() const
{
    return hMainWindow;
}

void MainView::Show(int nCmdShow)
{
    ShowWindow(
        hMainWindow,
        nCmdShow);

    UpdateWindow(
        hMainWindow);
}

void MainView::SetBirthDate(
    const BirthDate& date)
{
    std::wstring text =
        L"Дата рождения: " +
        std::to_wstring(date.day) +
        L"." +
        std::to_wstring(date.month) +
        L"." +
        std::to_wstring(date.year);

    SetWindowTextW(
        hDateLabel,
        text.c_str());
}

void MainView::UpdateResult(
    const AgeResult& result)
{
    SetWindowTextW(
        hYearsLabel,
        (L"Возраст в годах: " +
            std::to_wstring(result.years)).c_str());

    SetWindowTextW(
        hMonthsLabel,
        (L"Возраст в месяцах: " +
            std::to_wstring(result.months)).c_str());

    SetWindowTextW(
        hDaysLabel,
        (L"Возраст в днях: " +
            std::to_wstring(result.days)).c_str());

    SetWindowTextW(
        hMinutesLabel,
        (L"Возраст в минутах: " +
            std::to_wstring(result.minutes)).c_str());

    SetWindowTextW(
        hCoffeeLabel,
        (L"Чашки кофе: " +
            std::to_wstring(result.coffeeCups)).c_str());

    SetWindowTextW(
        hEpisodesLabel,
        (L"Серии сериала: " +
            std::to_wstring(result.tvEpisodes)).c_str());

    SetWindowTextW(
        hSocialLabel,
        (L"Километры ленты соцсетей: " +
            std::to_wstring(result.socialKilometers)).c_str());
}

void MainView::ClearResult()
{
    SetWindowTextW(
        hDateLabel,
        L"Дата рождения: данные не введены");

    SetWindowTextW(
        hYearsLabel,
        L"Возраст в годах: —");

    SetWindowTextW(
        hMonthsLabel,
        L"Возраст в месяцах: —");

    SetWindowTextW(
        hDaysLabel,
        L"Возраст в днях: —");

    SetWindowTextW(
        hMinutesLabel,
        L"Возраст в минутах: —");

    SetWindowTextW(
        hCoffeeLabel,
        L"Чашки кофе: —");

    SetWindowTextW(
        hEpisodesLabel,
        L"Серии сериала: —");

    SetWindowTextW(
        hSocialLabel,
        L"Километры ленты соцсетей: —");
}

LRESULT CALLBACK MainView::WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    MainView* view =
        reinterpret_cast<MainView*>(
            GetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW* createStruct =
            reinterpret_cast<CREATESTRUCTW*>(
                lParam);

        view =
            reinterpret_cast<MainView*>(
                createStruct->lpCreateParams);

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(
                view));

        return TRUE;
    }

    switch (message)
    {
    case WM_COMMAND:

        if (LOWORD(wParam) ==
            IDC_INPUT_BUTTON)
        {
            if (view &&
                view->controller)
            {
                view->controller->
                    OpenInputDialog();
            }

            return 0;
        }

        break;

    case WM_DESTROY:

        PostQuitMessage(0);

        return 0;
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam);
}

// ============================================================
// ОКНО ВВОДА
// ============================================================

InputDialog::InputDialog()
    : hDialog(nullptr),
    hDayEdit(nullptr),
    hMonthEdit(nullptr),
    hYearEdit(nullptr),
    hOkButton(nullptr),
    hCancelButton(nullptr),
    controller(nullptr)
{
}

bool InputDialog::Create(
    HINSTANCE hInstance,
    HWND parent,
    Controller* controller,
    const BirthDate& previousDate)
{
    this->controller = controller;

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        InputDialog::DialogProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        L"AgeUnexpectedInputWindow";

    wc.hCursor =
        LoadCursor(
            nullptr,
            IDC_ARROW);

    wc.hbrBackground =
        reinterpret_cast<HBRUSH>(
            COLOR_WINDOW + 1);

    static bool registered = false;

    if (!registered)
    {
        RegisterClassW(&wc);
        registered = true;
    }

    hDialog =
        CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            L"AgeUnexpectedInputWindow",
            L"Ввод даты рождения",
            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            420,
            330,
            parent,
            nullptr,
            hInstance,
            this);

    if (!hDialog)
        return false;

    CreateControls(hInstance);

    // Восстанавливаем последнюю дату
    SetWindowTextW(
        hDayEdit,
        std::to_wstring(
            previousDate.day).c_str());

    SetWindowTextW(
        hMonthEdit,
        std::to_wstring(
            previousDate.month).c_str());

    SetWindowTextW(
        hYearEdit,
        std::to_wstring(
            previousDate.year).c_str());

    return true;
}

void InputDialog::CreateControls(
    HINSTANCE hInstance)
{
    HFONT hFont =
        CreateFontW(
            18,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");

    HWND title =
        CreateWindowW(
            L"STATIC",
            L"Введите дату рождения:",
            WS_CHILD |
            WS_VISIBLE,
            35,
            25,
            330,
            30,
            hDialog,
            nullptr,
            hInstance,
            nullptr);

    SendMessageW(
        title,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            hFont),
        TRUE);

    HWND dayLabel =
        CreateWindowW(
            L"STATIC",
            L"День:",
            WS_CHILD |
            WS_VISIBLE,
            35,
            75,
            100,
            25,
            hDialog,
            nullptr,
            hInstance,
            nullptr);

    SendMessageW(
        dayLabel,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            hFont),
        TRUE);

    hDayEdit =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_NUMBER |
            ES_CENTER,
            150,
            72,
            180,
            30,
            hDialog,
            reinterpret_cast<HMENU>(
                IDC_DAY_EDIT),
            hInstance,
            nullptr);

    HWND monthLabel =
        CreateWindowW(
            L"STATIC",
            L"Месяц:",
            WS_CHILD |
            WS_VISIBLE,
            35,
            120,
            100,
            25,
            hDialog,
            nullptr,
            hInstance,
            nullptr);

    SendMessageW(
        monthLabel,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            hFont),
        TRUE);

    hMonthEdit =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_NUMBER |
            ES_CENTER,
            150,
            117,
            180,
            30,
            hDialog,
            reinterpret_cast<HMENU>(
                IDC_MONTH_EDIT),
            hInstance,
            nullptr);

    HWND yearLabel =
        CreateWindowW(
            L"STATIC",
            L"Год:",
            WS_CHILD |
            WS_VISIBLE,
            35,
            165,
            100,
            25,
            hDialog,
            nullptr,
            hInstance,
            nullptr);

    SendMessageW(
        yearLabel,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            hFont),
        TRUE);

    hYearEdit =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_NUMBER |
            ES_CENTER,
            150,
            162,
            180,
            30,
            hDialog,
            reinterpret_cast<HMENU>(
                IDC_YEAR_EDIT),
            hInstance,
            nullptr);

    hOkButton =
        CreateWindowW(
            L"BUTTON",
            L"ОК",
            WS_CHILD |
            WS_VISIBLE |
            BS_DEFPUSHBUTTON,
            75,
            220,
            110,
            40,
            hDialog,
            reinterpret_cast<HMENU>(
                IDC_OK_BUTTON),
            hInstance,
            nullptr);

    hCancelButton =
        CreateWindowW(
            L"BUTTON",
            L"Отмена",
            WS_CHILD |
            WS_VISIBLE |
            BS_PUSHBUTTON,
            220,
            220,
            110,
            40,
            hDialog,
            reinterpret_cast<HMENU>(
                IDC_CANCEL_BUTTON),
            hInstance,
            nullptr);

    HWND controls[] =
    {
        hDayEdit,
        hMonthEdit,
        hYearEdit,
        hOkButton,
        hCancelButton
    };

    for (HWND control : controls)
    {
        SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(
                hFont),
            TRUE);
    }
}

HWND InputDialog::GetWindowHandle() const
{
    return hDialog;
}

void InputDialog::Show()
{
    ShowWindow(
        hDialog,
        SW_SHOW);

    UpdateWindow(
        hDialog);

    SetForegroundWindow(
        hDialog);

    SetFocus(
        hDayEdit);
}

bool InputDialog::GetDate(
    int& day,
    int& month,
    int& year)
{
    wchar_t dayBuffer[32]{};
    wchar_t monthBuffer[32]{};
    wchar_t yearBuffer[32]{};

    GetWindowTextW(
        hDayEdit,
        dayBuffer,
        32);

    GetWindowTextW(
        hMonthEdit,
        monthBuffer,
        32);

    GetWindowTextW(
        hYearEdit,
        yearBuffer,
        32);

    if (wcslen(dayBuffer) == 0 ||
        wcslen(monthBuffer) == 0 ||
        wcslen(yearBuffer) == 0)
    {
        return false;
    }

    try
    {
        day =
            std::stoi(dayBuffer);

        month =
            std::stoi(monthBuffer);

        year =
            std::stoi(yearBuffer);
    }
    catch (...)
    {
        return false;
    }

    return true;
}

void InputDialog::Close()
{
    if (hDialog)
    {
        DestroyWindow(hDialog);

        hDialog = nullptr;
    }
}

LRESULT CALLBACK InputDialog::DialogProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    InputDialog* dialog =
        reinterpret_cast<InputDialog*>(
            GetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        CREATESTRUCTW* createStruct =
            reinterpret_cast<CREATESTRUCTW*>(
                lParam);

        dialog =
            reinterpret_cast<InputDialog*>(
                createStruct->lpCreateParams);

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(
                dialog));

        return TRUE;
    }

    switch (message)
    {
    case WM_COMMAND:

        if (LOWORD(wParam) ==
            IDC_OK_BUTTON)
        {
            if (dialog &&
                dialog->controller)
            {
                dialog->controller->
                    ProcessInput();
            }

            return 0;
        }

        if (LOWORD(wParam) ==
            IDC_CANCEL_BUTTON)
        {
            if (dialog)
                dialog->Close();

            return 0;
        }

        break;

    case WM_CLOSE:

        if (dialog)
            dialog->Close();

        return 0;
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam);
}