#include "Rectangle.h"
#include "Circle.h"
#include <vector>
#include <memory>
#include <iostream>

int main() {
    // 使用多型管理不同的形狀
    std::vector<std::unique_ptr<Shape>> shapes;

    // 添加一些形狀
    shapes.push_back(std::make_unique<Rectangle>(5.0, 3.0));
    shapes.push_back(std::make_unique<Circle>(2.0));
    shapes.push_back(std::make_unique<Rectangle>(10.0, 4.0));
    shapes.push_back(std::make_unique<Circle>(3.0));

    // 列印每個形狀的面積
    for (const auto& shape : shapes) {
        std::cout << shape->getName() << " area: " << shape->getArea() << std::endl;
    }

    return 0;
}
