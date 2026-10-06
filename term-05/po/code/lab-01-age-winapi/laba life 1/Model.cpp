#pragma execution_character_set("utf-8")

#include "Model.h"

#include <ctime>
#include <cmath>

Model::Model()
    : birthDate(1, 1, 2000),
    hasDate(false)
{
    result = {};
}

void Model::SetUpdateCallback(
    std::function<void(const AgeResult&)> callback)
{
    updateCallback = callback;
}

bool Model::IsLeapYear(int year) const
{
    return (year % 400 == 0) ||
        (year % 4 == 0 && year % 100 != 0);
}

int Model::DaysInMonth(int month, int year) const
{
    switch (month)
    {
    case 1:  return 31;
    case 2:  return IsLeapYear(year) ? 29 : 28;
    case 3:  return 31;
    case 4:  return 30;
    case 5:  return 31;
    case 6:  return 30;
    case 7:  return 31;
    case 8:  return 31;
    case 9:  return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 0;
    }
}

bool Model::IsValidDate(
    int day,
    int month,
    int year,
    std::wstring& errorMessage) const
{
    if (year < 1900 || year > 2100)
    {
        errorMessage =
            L"Год должен находиться в диапазоне от 1900 до 2100.";

        return false;
    }

    if (month < 1 || month > 12)
    {
        errorMessage =
            L"Месяц должен находиться в диапазоне от 1 до 12.";

        return false;
    }

    int maxDay = DaysInMonth(month, year);

    if (day < 1 || day > maxDay)
    {
        errorMessage =
            L"Такой даты не существует.";

        return false;
    }

    std::time_t currentTime = std::time(nullptr);

    std::tm currentDate{};

    localtime_s(&currentDate, &currentTime);

    int currentYear = currentDate.tm_year + 1900;
    int currentMonth = currentDate.tm_mon + 1;
    int currentDay = currentDate.tm_mday;

    if (year > currentYear ||
        (year == currentYear && month > currentMonth) ||
        (year == currentYear &&
            month == currentMonth &&
            day > currentDay))
    {
        errorMessage =
            L"Дата рождения не может быть в будущем.";

        return false;
    }

    return true;
}

bool Model::SetBirthDate(
    int day,
    int month,
    int year,
    std::wstring& errorMessage)
{
    if (!IsValidDate(
        day,
        month,
        year,
        errorMessage))
    {
        return false;
    }

    birthDate = BirthDate(day, month, year);

    result = CalculateAge(birthDate);

    hasDate = true;

    // Активная модель самостоятельно уведомляет View
    if (updateCallback)
    {
        updateCallback(result);
    }

    return true;
}

BirthDate Model::GetBirthDate() const
{
    return birthDate;
}

bool Model::HasDate() const
{
    return hasDate;
}

AgeResult Model::GetResult() const
{
    return result;
}

long long Model::DaysFromCivil(
    int year,
    unsigned month,
    unsigned day) const
{
    year -= month <= 2;

    const long long era =
        (year >= 0 ? year : year - 399) / 400;

    const unsigned yoe =
        static_cast<unsigned>(
            year - era * 400);

    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
        + day - 1;

    const unsigned doe =
        yoe * 365 +
        yoe / 4 -
        yoe / 100 +
        doy;

    return era * 146097 +
        static_cast<long long>(doe);
}

AgeResult Model::CalculateAge(
    const BirthDate& date) const
{
    AgeResult age{};

    std::time_t currentTime = std::time(nullptr);

    std::tm currentDate{};

    localtime_s(
        &currentDate,
        &currentTime);

    int currentYear =
        currentDate.tm_year + 1900;

    int currentMonth =
        currentDate.tm_mon + 1;

    int currentDay =
        currentDate.tm_mday;

    // Полные годы
    age.years =
        currentYear - date.year;

    if (currentMonth < date.month ||
        (currentMonth == date.month &&
            currentDay < date.day))
    {
        age.years--;
    }

    // Полные месяцы
    age.months =
        age.years * 12 +
        (currentMonth - date.month);

    if (currentDay < date.day)
    {
        age.months--;
    }

    // Количество дней
    long long birthDays =
        DaysFromCivil(
            date.year,
            static_cast<unsigned>(date.month),
            static_cast<unsigned>(date.day));

    long long currentDays =
        DaysFromCivil(
            currentYear,
            static_cast<unsigned>(currentMonth),
            static_cast<unsigned>(currentDay));

    age.days =
        currentDays - birthDays;

    // Количество минут
    age.minutes =
        age.days * 24LL * 60LL;

    /*
        Необычные единицы.

        Учебные коэффициенты:

        1 чашка кофе = каждые 2 дня
        1 серия сериала = каждые 3 дня
        1 км ленты = каждые 30 минут
    */

    age.coffeeCups =
        age.days / 2.0;

    age.tvEpisodes =
        age.days / 3.0;

    age.socialKilometers =
        age.minutes / 30.0;

    return age;
}