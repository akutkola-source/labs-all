#pragma execution_character_set("utf-8")

#include "Controller.h"

Controller::Controller(
    HINSTANCE hInstance)
    : hInstance(hInstance)
{
}

bool Controller::Initialize()
{
    if (!mainView.Create(
        hInstance,
        this))
    {
        return false;
    }

    // Активная модель сообщает контроллеру
    // об изменении результата
    model.SetUpdateCallback(
        [this](const AgeResult& result)
        {
            this->OnModelUpdated(result);
        });

    return true;
}

int Controller::Run(
    int nCmdShow)
{
    mainView.Show(nCmdShow);

    MSG message{};

    while (GetMessageW(
        &message,
        nullptr,
        0,
        0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(
        message.wParam);
}

void Controller::OpenInputDialog()
{
    if (inputDialog.GetWindowHandle())
    {
        SetForegroundWindow(
            inputDialog.GetWindowHandle());

        return;
    }

    BirthDate previousDate =
        model.GetBirthDate();

    if (!inputDialog.Create(
        hInstance,
        mainView.GetWindowHandle(),
        this,
        previousDate))
    {
        MessageBoxW(
            mainView.GetWindowHandle(),
            L"Не удалось открыть окно ввода.",
            L"Ошибка",
            MB_OK | MB_ICONERROR);

        return;
    }

    inputDialog.Show();
}

void Controller::ProcessInput()
{
    int day = 0;
    int month = 0;
    int year = 0;

    if (!inputDialog.GetDate(
        day,
        month,
        year))
    {
        MessageBoxW(
            inputDialog.GetWindowHandle(),
            L"Введите числа в поля дня, месяца и года.",
            L"Ошибка ввода",
            MB_OK | MB_ICONERROR);

        return;
    }

    std::wstring errorMessage;

    if (!model.SetBirthDate(
        day,
        month,
        year,
        errorMessage))
    {
        MessageBoxW(
            inputDialog.GetWindowHandle(),
            errorMessage.c_str(),
            L"Некорректная дата",
            MB_OK | MB_ICONERROR);

        return;
    }

    inputDialog.Close();
}

void Controller::OnModelUpdated(
    const AgeResult& result)
{
    mainView.SetBirthDate(
        model.GetBirthDate());

    mainView.UpdateResult(result);
}

Model& Controller::GetModel()
{
    return model;
}

InputDialog& Controller::GetInputDialog()
{
    return inputDialog;
}

MainView& Controller::GetMainView()
{
    return mainView;
}