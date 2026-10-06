#include "Model.h"
#include <ctime>

long long AgeModel::getDaysLived() const {
    time_t now = time(0);
    tm ltm;
    localtime_s(&ltm, &now);
    int currYear = 1900 + ltm.tm_year;
    int currMonth = 1 + ltm.tm_mon;
    int currDay = ltm.tm_mday;
    // Упрощенный расчет
    return (currYear - year) * 365 + (currMonth - month) * 30 + (currDay - day);
}

void AgeModel::setBirthDate(int d, int m, int y) {
    if (d < 1 || d > 31 || m < 1 || m > 12 || y < 1900 || y > 2024) {
        throw std::invalid_argument("Некорректная дата!");
    }
    day = d; month = m; year = y;
    notifyObservers(); // Активная модель уведомляет View
}

int AgeModel::getYears() const { return getDaysLived() / 365; }
int AgeModel::getMonths() const { return getDaysLived() / 30; }
int AgeModel::getDays() const { return getDaysLived(); }
long long AgeModel::getMinutes() const { return getDaysLived() * 24 * 60; }
int AgeModel::getCoffeeCups() const { return getDaysLived() * 2; }
int AgeModel::getTvSeries() const { return getDaysLived() * 3; }
double AgeModel::getSocialMediaKm() const { return getDaysLived() * 0.015; }