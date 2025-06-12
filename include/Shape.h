#ifndef SHAPE_H
#define SHAPE_H

#include <string>

class Shape {
public:
    virtual ~Shape() {}                              // 虛擬析構函式
    virtual double getArea() const = 0;             // 純虛函式，計算面積
    virtual std::string getName() const = 0;        // 純虛函式，獲取形狀名稱
};

#endif // SHAPE_H
