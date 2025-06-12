#ifndef LIBRARY_H
#define LIBRARY_H

#include "Item.h"
#include <vector>
#include <memory>

// Library 類：頭文件中只聲明接口
class Library {
private:
    std::vector<std::unique_ptr<Item>> items;

public:
    void addItem(std::unique_ptr<Item> item);
    void printAllItems() const;
};

#endif // LIBRARY_H
