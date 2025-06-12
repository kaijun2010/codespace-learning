#include "Library.h"
#include <iostream>

// 添加物品到圖書館
void Library::addItem(std::unique_ptr<Item> item) {
    items.push_back(std::move(item));
}

// 列印所有物品資訊
void Library::printAllItems() const {
    std::cout << "Library Items:\n";
    for (const auto& item : items) {
        item->printDetails();
        std::cout << "-----------------\n";
    }
}
