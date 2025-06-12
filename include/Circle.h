#ifndef CIRCLE_H
#define CIRCLE_H

#include "Shape.h"

class Circle : public Shape {
private:
    double radius;

public:
    Circle(double r);
    double getArea() const override;
    std::string getName() const override;
};

#endif // CIRCLE_H
