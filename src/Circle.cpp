#include "Circle.h"
#include <cmath>

Circle::Circle(double r) : radius(r) {}

double Circle::getArea() const {
    return M_PI * radius * radius;
}

std::string Circle::getName() const {
    return "Circle";
}

