#include "Controller.h"

void AgeController::handleInput(const std::string& dStr, const std::string& mStr, const std::string& yStr) {
    try {
        int d = std::stoi(dStr);
        int m = std::stoi(mStr);
        int y = std::stoi(yStr);
        model->setBirthDate(d, m, y);
    }
    catch (...) {
        throw std::invalid_argument("Вводите только числа!");
    }
}