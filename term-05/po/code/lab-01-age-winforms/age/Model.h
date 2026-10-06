#pragma once
#include <vector>
#include <stdexcept>
#include "Observer.h"

class AgeModel {
private:
    int day, month, year;
    std::vector<IObserver*> observers;

    long long getDaysLived() const;

public:
    AgeModel() : day(1), month(1), year(2000) {}

    void addObserver(IObserver* obs) { observers.push_back(obs); }
    void notifyObservers() {
        for (auto obs : observers) obs->update();
    }

    void setBirthDate(int d, int m, int y);

    int getDay() const { return day; }
    int getMonth() const { return month; }
    int getYear() const { return year; }

    int getYears() const;
    int getMonths() const;
    int getDays() const;
    long long getMinutes() const;

    // Неожиданные единицы
    int getCoffeeCups() const;
    int getTvSeries() const;
    double getSocialMediaKm() const;
};
