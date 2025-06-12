#ifndef ITEM_H
#define ITEM_H

#include <string>

class Item {
protected:
    std::string title;

public:
    Item(const std::string& title) : title(title) {}
    virtual ~Item(); // 宣告，但不實現
    virtual std::string getType() const = 0;
    virtual void printDetails() const = 0;
};

#endif // ITEM_H
