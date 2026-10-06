#pragma once

#include <string>
#include <functional>

// Структура даты рождения
struct BirthDate
{
    int day;
    int month;
    int year;

    BirthDate(int d = 1, int m = 1, int y = 2000)
        : day(d), month(m), year(y)
    {
    }
};

// Результаты расчётов
struct AgeResult
{
    int years;
    int months;
    long long days;
    long long minutes;

    double coffeeCups;
    double tvEpisodes;
    double socialKilometers;
};

// Активная модель MVC.
// Она самостоятельно сообщает представлению,
// когда её данные были изменены.
class Model
{
private:
    BirthDate birthDate;

    AgeResult result;

    bool hasDate;

    // Функция обратного вызова для активной модели
    std::function<void(const AgeResult&)> updateCallback;

    bool IsLeapYear(int year) const;

    int DaysInMonth(int month, int year) const;

    long long DaysFromCivil(
        int year,
        unsigned month,
        unsigned day) const;

public:
    Model();

    // Установить функцию, которую модель будет вызывать
    // после успешного пересчёта.
    void SetUpdateCallback(
        std::function<void(const AgeResult&)> callback);

    // Проверить дату
    bool IsValidDate(
        int day,
        int month,
        int year,
        std::wstring& errorMessage) const;

    // Установить дату рождения и выполнить расчёт
    bool SetBirthDate(
        int day,
        int month,
        int year,
        std::wstring& errorMessage);

    // Получить последнюю введённую дату
    BirthDate GetBirthDate() const;

    // Проверить, была ли дата уже введена
    bool HasDate() const;

    // Получить текущий результат
    AgeResult GetResult() const;

    // Выполнить расчёт возраста
    AgeResult CalculateAge(
        const BirthDate& date) const;
};#pragma once
