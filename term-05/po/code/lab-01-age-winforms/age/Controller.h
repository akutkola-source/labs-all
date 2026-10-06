#pragma once
#include "Model.h"
#include <string>

class AgeController {
private:
    AgeModel* model;
public:
    AgeController(AgeModel* m) : model(m) {}
    void handleInput(const std::string& dStr, const std::string& mStr, const std::string& yStr);
};