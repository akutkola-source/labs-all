#pragma once

#include <windows.h>

#include "Model.h"

class Controller;

// Главное окно приложения
class MainView
{
private:
    HWND hMainWindow;

    HWND hTitle;

    HWND hButton;

    HWND hDateLabel;

    HWND hYearsLabel;
    HWND hMonthsLabel;
    HWND hDaysLabel;
    HWND hMinutesLabel;
    HWND hCoffeeLabel;
    HWND hEpisodesLabel;
    HWND hSocialLabel;

    Controller* controller;

    static LRESULT CALLBACK WindowProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    void CreateControls(HINSTANCE hInstance);

public:
    MainView();

    bool Create(
        HINSTANCE hInstance,
        Controller* controller);

    HWND GetWindowHandle() const;

    void Show(int nCmdShow);

    // Отобразить дату
    void SetBirthDate(const BirthDate& date);

    // Отобразить результаты
    void UpdateResult(const AgeResult& result);

    // Отобразить отсутствие данных
    void ClearResult();
};

// Дополнительное окно для ввода даты
class InputDialog
{
private:
    HWND hDialog;

    HWND hDayEdit;
    HWND hMonthEdit;
    HWND hYearEdit;

    HWND hOkButton;
    HWND hCancelButton;

    Controller* controller;

    static LRESULT CALLBACK DialogProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    void CreateControls(HINSTANCE hInstance);

public:
    InputDialog();

    bool Create(
        HINSTANCE hInstance,
        HWND parent,
        Controller* controller,
        const BirthDate& previousDate);

    HWND GetWindowHandle() const;

    void Show();

    bool GetDate(
        int& day,
        int& month,
        int& year);

    void Close();
};#pragma once
