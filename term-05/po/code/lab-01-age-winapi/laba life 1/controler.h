#pragma once

#include <windows.h>

#include "Model.h"
#include "View.h"

// Контроллер связывает Model и View.
class Controller
{
private:
    HINSTANCE hInstance;

    Model model;

    MainView mainView;

    InputDialog inputDialog;

public:
    Controller(HINSTANCE hInstance);

    // Инициализация приложения
    bool Initialize();

    // Главный цикл сообщений Windows
    int Run(int nCmdShow);

    // Открыть окно ввода
    void OpenInputDialog();

    // Получить данные из окна ввода
    void ProcessInput();

    // Обработать обновление активной модели
    void OnModelUpdated(
        const AgeResult& result);

    Model& GetModel();

    InputDialog& GetInputDialog();

    MainView& GetMainView();
};#pragma once
