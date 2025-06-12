#include "Rectangle.h"

Rectangle::Rectangle(double w, double h) : width(w), height(h) {}

double Rectangle::getArea() const {
        return width * height;
}

std::string Rectangle::getName() const {
        return "Rectangle";
}
